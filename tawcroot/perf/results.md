# Perf baselines

Recorded runs of `run-perf.sh` so future runs can be compared against
something. Rerun with the same config and append a new section; flag
anything that moves a lot (say >25% on a median) without a known cause.
Raw TSVs live in `build/tawcroot-perf/` (not committed).

Medians are the number to compare; see README.md "Reading results".

## 2026-06-10 host x86_64

- Host: AMD Ryzen AI 9 HX 370, dev laptop.
- Config: `--iterations 50000 --rounds 20 --interleave`, minimal
  rootfs (empty dir + `tmp/`), static musl bench binary.
- tawcroot: host build @ git 164f80b (+ working tree).
- proot v5.3.1-99a84175 (seccomp accelerator on), runsc
  release-20260520.0 rootless, both from `build/tawcroot-perf/tools/`.
- chroot mode skipped (no root).

Median ns_per_op:

| benchmark           | native  | tawcroot | proot   | gvisor    |
|---------------------|--------:|---------:|--------:|----------:|
| getpid              |      73 |      126 |      80 |       958 |
| stat                |     352 |    2,285 |  17,109 |     5,268 |
| open_read_close     |     552 |    2,300 |   7,879 |    13,814 |
| readdir_entries     |      43 |       60 |      90 |       238 |
| create_write_unlink |   2,880 |    6,454 |  16,138 |    27,893 |
| fork_exec_wait      | 165,430 |  407,786 | 379,614 | 1,476,076 |

Expected shape, for spotting regressions:

- Path syscalls (`stat`, `open_read_close`, `create_write_unlink`):
  tawcroot ~2-7x native and 2.5-7x faster than proot. This is the
  core win of SIGSYS dispatch over ptrace; if tawcroot drifts toward
  proot here, something in the handler hot path regressed.
- `fork_exec_wait`: tawcroot ~2.5x native, slightly (~7%) behind
  proot. Known structural cost: each guest execve re-execs tawcroot
  itself and rebuilds runtime state in the child, vs proot's
  long-lived tracer + tiny loader stub. Was 476µs in a May 2026 run
  with the same config; improvements here are real, big increases
  are regressions in the exec/state-handoff path.
- `getpid`: untrapped; measures BPF filter evaluation only
  (~50ns over native from the IP-allowlist prelude + linear JEQ
  chain). Sanity check, not a comparison.
- `readdir_entries`: near-native for everyone; tawcroot pays a
  little for dirent filtering.

## 2026-10-18 device arm64 (first on-device baseline)

- Device: Galaxy S24 (SM-S921N), Android 17, kernel 6.1.162,
  pinned to cpu9 (3.2 GHz prime core, `--cpu 200`).
- Config: `run-perf-device.sh --rounds 10 --iterations 20000`,
  adb `shell` domain (not untrusted_app), minimal rootfs in
  `/data/local/tmp/tawc-dev/perf/`, static glibc bench.
- tawcroot: arm64 NDK build @ git bee5ae2. `write`/`getpid` trapped.

Median ns_per_op:

| benchmark           | native  | tawcroot  | ratio |
|---------------------|--------:|----------:|------:|
| gettid (untrapped)  |     402 |       501 |  1.2x |
| getuid (trap floor) |     350 |     1,923 |  5.5x |
| getpid              |     321 |     1,578 |  4.9x |
| fstat               |     348 |     2,696 |  7.7x |
| write_read_pipe     |   1,140 |     2,454 |  2.2x |
| stat                |   1,653 |     7,569 |  4.6x |
| stat_deep (8 comp)  |   1,644 |    12,471 |  7.6x |
| stat_leaf_symlink   |   1,328 |    10,102 |  7.6x |
| stat_relative       |     771 |     8,037 | 10.4x |
| fstatat_dirfd       |     769 |     9,420 | 12.2x |
| open_read_close     |   3,244 |     9,151 |  2.8x |
| readdir_entries     |     178 |       224 |  1.3x |
| create_write_unlink |  20,779 |    33,665 |  1.6x |
| fork_exec_wait      | 875,250 | 1,204,921 |  1.4x |
| signal_raise        |   1,334 |     6,290 |  4.7x |

Shape: one SIGSYS round trip is ~1.5 µs (getuid − native). Path
syscalls pay 4–9 µs on top of that from in-handler host syscalls
(per-component `readlinkat`, `getcwd`, `/proc/self/fd` readlink,
`gettid` + `process_vm_*` per copy), so cutting host-syscall count
beats cutting the trap itself until those are gone.

## 2026-10-18 device arm64, app domain

Same S24 / cpu9 pin, bench run under `libtawcroot.so` from the dev
APK's nativeLibraryDir through the exec broker (`untrusted_app_34`),
rootfs in the app's files dir. 6 rounds × 20000 iterations, after the
filter rework, lazy namespace traps, lazy rescue recording, cached
usercopy pid, and the shared path cache. (No native column: the app
domain cannot execve app-data files.)

| benchmark           | tawcroot (app) | tawcroot (shell) | native (shell) |
|---------------------|---------------:|-----------------:|---------------:|
| gettid (untrapped)  |            188 |              461 |            416 |
| getuid (trap floor) |          1,011 |            1,719 |            373 |
| fstat               |          1,904 |            2,012 |            316 |
| stat                |          4,010 |            3,978 |          1,679 |
| stat_deep           |          4,586 |            4,555 |          1,637 |
| stat_leaf_symlink   |          4,859 |            4,790 |          1,312 |
| fstatat_dirfd       |          5,951 |            5,832 |            770 |
| open_read_close     |          5,406 |            5,422 |          3,223 |
| fork_exec_wait      |      1,150,754 |        1,674,783 |        989,321 |

Path-heavy rows agree across domains; the trap floor and bare syscall
are cheaper in the app domain on this device.

## 2026-10-18 real workloads, app domain (macro.sh)

Arch Linux ARM in the dev app, S24, pinned to cpu9 (`--cpu 9`), wall
ms per workload (median of 5, medians across 2–3 interleaved rounds),
before = bee5ae2 libtawcroot.so repacked into the same APK, after =
a4a9abc. Unpinned runs swung 2x and even flipped direction, so pin.

| workload         | before | after | after/before |
|------------------|-------:|------:|-------------:|
| spawn_true_x100  |    187 |   191 |         1.02 |
| find_usr         |     88 |    67 |         0.76 |
| ls_lR_usr_lib    |     49 |    32 |         0.66 |
| stat_many        |     59 |    35 |         0.60 |
| grep_tree        |    244 |   178 |         0.73 |
| tar_usr_share    |    330 |   225 |         0.68 |
| pacman_Ql        |     39 |    32 |         0.83 |
| pacman_Qk        |    330 |   210 |         0.64 |
| bash_startup_x50 |    155 |   127 |         0.82 |
| sed_awk_pipe     |     61 |    57 |         0.93 |

Filesystem-heavy work is 17–40% faster; process-spawn work (≈1.9 ms per
fork+exec of `true`, 2.5 ms per bash) did not move: the exec path is
now the largest unchanged cost. One unpinned `find_usr` run took 323 s
(median of its 5 runs was normal); not reproduced in 30 + 6 further
runs, cause unknown.
