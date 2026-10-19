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
- Full fake capabilities (`capget`/`capset`), `PR_CAPBSET_READ/DROP`,
  `PR_CAP_AMBIENT`, `PR_SET_SECUREBITS` succeed against shadow state.
- Direct children of the init get `PR_SET_PDEATHSIG(SIGKILL)` (at fork
  and at exec, since systemd spawns through `posix_spawn`), so services
  die with the init instead of outliving it.
- `unshare` returns `EPERM`, which systemd reads as "containerized" and
  skips service sandboxing.
- `mount`: bind mounts keep the existing bind emulation; remounts,
  propagation changes and mounts on API points (`/proc`, `/sys`, `/dev`,
  `/dev/shm`, `/dev/pts`, `/run`, `/sys/fs/cgroup`) return 0 without
  doing anything; everything else (including `MS_MOVE`) is `EPERM`, so
  systemd takes its plain-directory fallbacks (credentials).
  `umount2` returns `EINVAL` (`ENOENT` for missing paths): systemd
  retries umount until `EINVAL`.

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

## Known boot failures

Debian sid's systemd 257 reaches `multi-user.target` (journald, udevd,
networkd, logind, dbus, getty run).
- `.mount` units for real filesystems (tmp, mqueue, hugepages, debugfs,
  fusectl) exit 32: `mount(8)` gets `EPERM`.
- binfmt_misc automount fails.
- `systemd-firstboot` waits on the console when `/etc/machine-id` is
  unset; the rootfs needs its first-boot settings done or the unit masked.
- `/run/initctl` and `/run/systemd/journal/dev-log` symlinks into a
  host-bound `/dev` fail `EACCES` (harmless).
- `systemd-networkd-persistent-storage` and `systemd-resolved` fail
  (not yet investigated).
- Hazard: no pid isolation, so a guest `kill(-1)` reaches app processes.
