# tawcroot as a container runtime

Turn tawcroot from "a rootfs runner that can boot systemd" into a
software container runtime for stock (unrooted) Android. The kernel
gives an app no namespaces, no real root and no cgroups. tawcroot
emulates each of these in its seccomp trap layer, and the app stays
the front end.

**Status: plan, not started.** Base: PR #4 (`--init --cgroup --join
--console`). Sid's systemd 262 reaches `running` with 0 failed units
on an arm64 phone, booted through the app. Current mechanics are in
[notes/tawcroot/systemd.md](../notes/tawcroot/systemd.md).

## Principles

- **One layer per namespace.** Each emulated namespace owns its
  translation in one module. Every syscall and `/proc` surface that
  exposes the namespace's ids routes through that module. Today PID 1
  is a set of special cases spread over the getdents filter, stat
  rewrites, path translation and signals. The `ps` bug from 2026-10-20
  (`openat(procfd, "1/stat")` bypassing the rewrite) came from one of
  those gaps.
- **Session state lives in the store** (`<rootfs parent>/tawcroot`).
  It is shared by every process of the container, including `--join`ed
  ones, and is checked against starttime so recycled pids drop out.
  The cgroup registry already works this way.
- **Not a security boundary.** Isolation means the guest can't
  *accidentally* see or hit the host (app processes, other containers).
  Hostile guests are out of scope.
- **Every phase lands with a host test and an arm64 CI check.** Android
  SELinux surprises are verified on the phone with
  `/mnt/project-files/tawcroot/phone-systemd-test.sh`.

## Phase 1: PID namespace (next)

Give every guest process a virtual pid, not just the init.

- **Table:** `<store>/pids`, a flock-guarded mmap of
  `vpid ↔ (real pid, starttime)`, same shape as `cgroup.registry`.
  - The init is 1, and new vpids count up from 2.
  - Trapped `clone`/`fork`/`vfork` allocate the child's vpid in the
    parent after the kernel returns.
  - `--join` registers its tree the same way.
- **Translate in one place** (`pidns.c`). Inputs map vpid→real, outputs
  map real→vpid, and non-guest pids are `ESRCH`. Covered calls:
  - `getpid`, `getppid`, `gettid`, `set_tid_address` return.
  - `kill`, `tgkill`, `tkill`, `rt_(tg)sigqueueinfo`, `pidfd_open`,
    `waitid(P_PID)`.
  - `wait4`/`waitid` results, and `si_pid` in SIGCHLD siginfo (via
    `signal_shadow`).
  - `SO_PEERCRED`, `SCM_CREDENTIALS`, `F_GETOWN`/`F_SETOWN`,
    `sched_*`/`prlimit`/`getpriority` pid args.
- **`/proc` view**, driven by the same table:
  - Directory listing shows vpids.
  - `/proc/<vpid>/...` resolves to the real pid. This covers absolute
    paths and fd-relative opens through a single resolver, and replaces
    `proc_one_to_init` and `proc_relative_to_abs`.
  - `/proc/self` and `/proc/thread-self` readlinks.
  - `stat`, `status` (`NSpid` etc.) and `task/` are rewritten.
  - `cgroup.procs` lists vpids.
- **Remove the init special cases** once the table covers them.
- **Done when:** `ps`, `top`, `pgrep`, `systemctl status` and `kill`
  from a joined shell all agree on vpids on host, arm64 CI and the
  phone.

## Phase 2: mount namespace cleanup

- Make the synthesized mount table (`mounts.c`) the single source of
  truth for path translation. Guest mounts become translation rules,
  not side tables next to the bind list.
- Keep mounts per container (per store), so two running containers
  don't see each other's mounts.
- `umount` of everything systemd mounts, so shutdown is clean.

## Phase 3: lifecycle and app integration

- **Stop/reboot:**
  - The app sends `SIGRTMIN+3` (halt) or `SIGINT` (reboot) to the init
    and waits for it to exit.
  - Emulate `reboot(2)` so `systemctl poweroff/reboot` end or restart
    the session.
- **App screen:** container status (`is-system-running`, failed units)
  and a console view of the `--console` pty, not just the log file.
- **`/run/systemd/container`:** write it, and pass `container=tawc` in
  every launch path.
- **Exec into a running container:** the app terminal uses `--join` by
  default when a session is up.

## Phase 4: resource limits (fake cgroup controllers)

- `pids.max`: enforced at trapped fork.
- `memory.max`: no kernel help. Poll RSS of the cgroup's processes and
  apply `cgroup.kill` past the limit, reported as an OOM in
  `memory.events`.
- `cpu.max`: probably skip, or map to nice. Decide after phase 1.

## Phase 5: network and IPC

- **Network:** stay "host network" (like `docker --network host`), with
  per-container hostname (done) and port remap (done). Real isolation
  isn't reachable without root, so document it as a non-goal.
- **IPC:** scope SysV shm/sem keys and abstract unix sockets per
  container, so two containers don't collide.

## Later

- **Images:** pull OCI images straight into a rootfs, beside the
  existing distro installers.
- **Several containers at once:** separate stores, plus app UI to list
  and stop them.

## Order

PID comes first because the other phases depend on pid identity:
cgroup membership, peer credentials, signals, lifecycle and limits all
name processes by pid. Mounts come next because shutdown and multiple
containers need them. Limits and IPC matter only once more than one
container runs at a time.

Status table: `/mnt/project-files/tawcroot/container-requirements.md`
(2026-10-19 snapshot, partly outdated).
