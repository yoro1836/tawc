# systemd as pid 1 (`--init --cgroup`)

Goal: boot the guest's systemd as pid 1 without root, user namespaces
or `unshare`. Everything below is emulation inside tawcroot; nothing is
kernel isolation.

```
tawcroot -r <rootfs> -b /proc:/proc -b /sys:/sys -b /dev:/dev \
    --console <log> --init --cgroup -- /lib/systemd/systemd
```

Set `container=<name>` in the environment so systemd takes its
container paths. `/dev/console` must be a tty or systemd logs nothing;
`--console FILE` provides one (a pty copied into FILE), or bind a tty.

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
  (debugfs, tracing, fusectl, hugepages, binfmt_misc). `/proc/sys` and
  sysfs `statfs` report `ST_RDONLY` (with a writable `/sys`, systemd
  resolves `/dev/console` to the host's kernel console).
- Direct children of the init get `PR_SET_PDEATHSIG(SIGKILL)` (at fork
  and at exec, since systemd spawns through `posix_spawn`), so services
  die with the init instead of outliving it.
- Pid isolation: signals and `/proc` listings only reach the init and
  its descendants ([seccomp-filter.md](seccomp-filter.md)). A guest
  `kill(-1)` no longer reaches app processes.
- `unshare` returns `EPERM`, which systemd reads as "containerized" and
  skips service sandboxing.
- systemd 262 also needs `pidfd_open` (it takes a pidfd on itself in
  every spawned child), `faccessat2(fd, "", X_OK, AT_EMPTY_PATH)`, and
  `statx` of the `/dev/shm` mount reporting `STATX_ATTR_MOUNT_ROOT`.
- Kernel keyrings (`add_key`, `request_key`, `keyctl`) return ENOSYS.
  Android SELinux lets apps join a session keyring but not link the
  user keyring, which systemd treats as fatal (`status=237/KEYRING`).
- Android SELinux also denies apps chmod on their ptys and inotify
  watches on them. `fchmodat2` fakes success for root like `fchmodat`,
  and a denied watch on a char device watches `<store>/watch-stub`
  instead (systemd waits on `/dev/console` before tty services).

## Mounts
Code: `src/mounts.c`. The session has one mount table, a flock-guarded
shared mmap at `<store>/mounts` (wiped at start, like
`<store>/mnt/`). Every process mirrors it into its bind table (binds
with `mount_id`) on the next path translation after the table's
generation moves, so there is no per-process mount namespace.
A `CLONE_VM` child (vfork, `posix_spawn`) shares its parent's bind
table but not its fds, so it never mirrors; it routes through the
parent's view until its exec builds its own.
- `tmpfs`/`ramfs`/`mqueue`/`fusectl` mount a fresh backing dir `<store>/mnt/<id>`
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
- New mount API: `fsopen("tmpfs"|"ramfs")` returns an `O_PATH` fd on
  `<store>/mnt/.fsctx-<n>`, which holds the options (reads fail, so
  libmount's message drain ends; Android apps can't reopen memfds); `fsconfig` records `mode`/`source`/`ro`; `fsmount` creates the
  backing dir and returns an `O_PATH` fd on it; `move_mount` of that fd
  adds the entry. systemd 258+ mounts credentials this way and treats
  `ENOSYS` as fatal. Other types, `open_tree`, `fspick`,
  `mount_setattr` and `statmount`/`listmount` are `ENOSYS`, which sends
  util-linux back to `mount(2)` (it probes `mount_setattr` before using
  mount attributes like `nosuid`).

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
  `/proc/sys/kernel/{hostname,domainname}` read it. journald epolls the
  hostname fd; like mountinfo fds it registers an inotify watch on
  `<store>/uts` instead, drained by `uname`.
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

## Joining a session (`--join`)
`tawcroot -r <rootfs> [-b ...] --join -- CMD` runs CMD inside the
rootfs's running init session, like `nsenter`/`machinectl shell`. The
init publishes `<store>/init` (pid, starttime, session flags) before
exec; `--join` checks it is still alive, takes the same pid view,
capabilities, mount table, hostname and cgroup tree (the joiner sits in
the root cgroup), and records `<store>/joined/<pid>` so its process tree
counts as a guest for signals and `/proc` listings. With no live init
it exits 97. Joiners keep their own `-b` binds, and binds win over
emulated mounts, so don't pass a `/run` bind to a joiner. Orphans of a
joined tree reparent outside the session and drop out of it.

App: the per-distro "Boot with systemd" setting (tawcroot installs,
`Installation.systemdBoot`, `TawcrootMethod.ensureSystemd`) boots
`/usr/lib/systemd/systemd` on the first spawn and `--join`s every spawn
while it runs. The init gets the normal binds minus `/tmp/.X11-unix`
(boot-time tmpfiles empties that dir, Xwayland's socket included) and
`--console <rootfs>/var/log/tawc-console.log`: tawcroot opens a pty,
binds its slave as `/dev/console` and forks a helper that appends the
master's output to that file (systemd won't log to a non-tty console;
it exits 255 silently on a fatal boot error otherwise). tawcroot's own
errors and the init's exit status land there too. Turning the setting off sends SIGRTMIN+4 (poweroff). Not
yet run on a device.

## Debugging on a device
- Init sessions append guest SIGSEGV/SIGBUS details (pc, return address
  and their `/proc/self/maps` lines) to `<store>/fault.log`.
- If `<store>/eacces.log` exists, every trapped syscall failing with
  EACCES/EPERM appends `pid nr errno path` to it.
- `<store>` is `<rootfs parent>/tawcroot`. A phone test script that runs
  both over adb or rish lives in the project files
  (`tawcroot/phone-systemd-test.sh`).

## Known boot failures

Debian trixie's systemd 257 and sid's 262 boot to `graphical.target`
(on the host, x86_64) with no failed units besides `getty@tty1`: journald, udevd, networkd, resolved, logind, dbus, hostnamed and
console-getty run, `busctl`/`hostnamectl`/`resolvectl` work, and
`tmp.mount`, `run-lock.mount`, `dev-mqueue.mount` mount.
- `systemd-firstboot` waits on the console when `/etc/machine-id` is
  unset; the rootfs needs its first-boot settings done or the unit masked.
- `/run/initctl` and `/run/systemd/journal/dev-log` symlinks into a
  host-bound `/dev` fail `EACCES` (harmless); `getty@tty1` can't open
  the host's `/dev/tty1`.
- Raising `RLIMIT_NOFILE` above the app's hard limit fails `EPERM`
  (services log it and continue).
