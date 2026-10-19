# systemd as pid 1 (`--init --cgroup`)

Goal: boot the guest's systemd as pid 1 without root, user namespaces
or `unshare`. Everything below is emulation inside tawcroot; nothing is
kernel isolation.

```
tawcroot -r <rootfs> -b /proc:/proc -b /sys:/sys -b /dev:/dev \
    -b <dir>:/run -b <pty>:/dev/console --init --cgroup -- /lib/systemd/systemd
```

Set `container=<name>` in the environment so systemd takes its
container paths. `/dev/console` must be a tty or systemd logs nothing.

## Pid 1 (`--init`)
See [seccomp-filter.md](seccomp-filter.md) for the PID filter and
subreaper. On top of that, in an init session:
- `/proc/1/...` resolves to the init's real `/proc/<pid>`, and
  `/proc/<pid>/stat|status` (also `self`, `task/<tid>`) are rewritten so
  pid/ppid/pgrp/session/`NSpid` say 1 for init and its ppid says 0.
  systemd checks children's ppid this way.
- Fake capabilities (`capget`/`capset`) start at systemd-nspawn's
  default set (`0xfdecafff`, bounding set dropped to match) and
  `PR_CAPBSET_READ/DROP`, `PR_CAP_AMBIENT`, `PR_SET_SECUREBITS` succeed
  against shadow state; `/proc/<pid>/status` `Cap*` lines show it.
- Looks like a user-namespaced container: `uid_map`/`gid_map` read
  `0 <uid> 1`, `setgroups` reads `deny`, and `/proc/*/ns/user` stats
  with a non-init inode. systemd then skips units it can't run there
  (debugfs, tracing, fusectl, hugepages, binfmt_misc). `/proc/sys`
  `statfs` reports `ST_RDONLY`.
- Direct children of the init get `PR_SET_PDEATHSIG(SIGKILL)` (at fork
  and at exec, since systemd spawns through `posix_spawn`), so services
  die with the init instead of outliving it.
- Pid isolation: signals and `/proc` listings only reach the init and
  its descendants ([seccomp-filter.md](seccomp-filter.md)). A guest
  `kill(-1)` no longer reaches app processes.
- `unshare` returns `EPERM`, which systemd reads as "containerized" and
  skips service sandboxing.

## Mounts
Code: `src/mounts.c`. The session has one mount table, a flock-guarded
shared mmap at `<store>/mounts` (wiped at start, like
`<store>/mnt/`). Every process mirrors it into its bind table (binds
with `mount_id`) on the next path translation after the table's
generation moves, so there is no per-process mount namespace.
A `CLONE_VM` child (vfork, `posix_spawn`) shares its parent's bind
table but not its fds, so it never mirrors; it routes through the
parent's view until its exec builds its own.
- `tmpfs`/`ramfs`/`mqueue` mount a fresh backing dir `<store>/mnt/<id>`
  (`mode=` applies) over the target; `statfs` reports the fs magic.
  A missing target under an existing parent works (`/dev/mqueue` in a
  host-bound `/dev`).
- `MS_BIND` (files too), `MS_MOVE` and `MS_REMOUNT` (`ro` toggle) work
  on the table; `umount2` removes an entry (a tmpfs's backing dir is
  detached to `mnt/.dead-<id>`), and returns `EINVAL` for non-mounts so
  umount loops end. Propagation-only calls and mounts on API points
  (`/proc`, `/sys`, `/dev`, `/dev/shm`, `/dev/pts`, `/run`,
  `/sys/fs/cgroup`) return 0; other filesystem types `EPERM`.
- Targets given as `/proc/self/fd/N` (systemd's style) resolve through
  the fd.
- `/proc/mounts` and `/proc/<pid>/mountinfo|mounts` are synthesized from
  the command line binds plus the table. libmount's monitor epolls
  mountinfo; those fds sit above the reserved fd floor, where a stacked
  filter traps `epoll_ctl` and registers an inotify fd on
  `<store>/mounts.event` instead, so systemd sees mount changes.
- The new mount API (`fsopen`, `open_tree`, `move_mount`, ...,
  `mount_setattr`) and `statmount`/`listmount` are `ENOSYS`; systemd
  and util-linux fall back to `mount(2)`.

Gaps: no mount namespaces (a service's private mounts are global);
emulated mounts are invisible to processes outside the session.

## Fake cgroup v2 (`--cgroup`)
Code: `src/cgroup.c`. Requires `--init`-style startup; set up in main
before the guest starts.
- A real directory `<store>/cgroup` (store = `<rootfs parent>/tawcroot`)
  is wiped at start and bound at `/sys/fs/cgroup`. `mkdir` creates the
  interface files (`cgroup.procs`, `cgroup.threads`, `cgroup.controllers`
  (empty), `cgroup.subtree_control`, `cgroup.events`, `cgroup.type`,
  `cgroup.freeze`, `cgroup.kill`, ...). `rmdir` removes them, failing
  `EBUSY` while populated or with child cgroups.
- Membership lives in `<store>/cgroup.registry`: a flock-guarded shared
  mmap of pid → cgroup path, each entry checked against the pid's
  starttime so recycled pids drop out. Trapped fork/clone puts the
  child in its parent's cgroup; an exec that skipped a trapped fork
  (vfork) registers itself.
- Reading `cgroup.procs|threads|events` returns a memfd built from the
  registry. Write opens of `cgroup.procs|threads|kill` are moved to a
  reserved fd (≥1000) and a stacked fd-floor filter traps `write` on
  those fds only, so ordinary writes stay untrapped. Writing a pid moves
  it; `1` to `cgroup.kill` SIGKILLs the subtree.
- `/proc/<pid>/cgroup` is synthesized as `0::/<path>`; `statfs` under
  the tree reports `CGROUP2_SUPER_MAGIC`.

Gaps: no controllers or limits; `cgroup.events` is only refreshed on
moves, not on process exit; `name_to_handle_at` cgroup IDs are not
emulated (systemd ignores the error).

## Loader and MDWE
Units with `MemoryDenyWriteExecute=` get `PR_SET_MDWE`, which inherits
across exec. The loader therefore never maps text writable: like
binfmt_elf it only zeroes a segment's file tail when the segment has
BSS.

## Hostname, ports, peers
- Hostname (`src/uts.c`): `sethostname`/`setdomainname` write
  `<store>/uts`, shared by the session; `uname` and
  `/proc/sys/kernel/{hostname,domainname}` read it.
- Privileged ports (`src/ports.c`): guest port p < 1024 is host port
  p + 20000. Binds shift on any address; connect/sendto/sendmsg shift
  for loopback destinations; getsockname/getpeername/accept and
  recvfrom/recvmsg/recvmmsg sources shift back. Inet datagram sockets
  are moved above the reserved fd floor, where a stacked filter traps
  their receives. Not covered: `sendmmsg` destinations, and a real host
  service on 20000–21023 shows up as its unshifted port.
- Peer identity: SO_PEERCRED of a guest peer reports its virtual
  uid/gid. Init-session processes record their ids in
  `<store>/ids/<pid>` (checked against starttime) when they change;
  a peer without a record has its nearest recorded ancestor's ids.
  dbus-daemon needs this to let services own their bus names.
- `capget` answers libcap-ng's version probe like the kernel (writes
  the v3 version, `EINVAL`).

## Known boot failures

Debian sid's systemd 257 boots to `graphical.target` with no failed
units: journald, udevd, networkd, resolved, logind, dbus, hostnamed and
console-getty run, `busctl`/`hostnamectl`/`resolvectl` work, and
`tmp.mount`, `run-lock.mount`, `dev-mqueue.mount` mount.
- `systemd-firstboot` waits on the console when `/etc/machine-id` is
  unset; the rootfs needs its first-boot settings done or the unit masked.
- `/run/initctl` and `/run/systemd/journal/dev-log` symlinks into a
  host-bound `/dev` fail `EACCES` (harmless); `getty@tty1` can't open
  the host's `/dev/tty1`.
- Raising `RLIMIT_NOFILE` above the app's hard limit fails `EPERM`
  (services log it and continue).
