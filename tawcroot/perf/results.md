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

## 2026-10-18 exec cost attribution, S24

`run-exec-cost.sh`, CPU 9, 4 KiB pages, SM-S921N, kernel
6.1.162-android14-11. Native and tawcroot ran as adb shell against the
same minimal rootfs and static glibc probe; the exec target only calls
`exit_group`, without libc startup. Five rounds × 1,000 operations,
five warm-ups per operation. Runtime: `bb332e1`, initially built with
NDK 29. These are overhead measurements, not a before/after gain.
Raw: `exec-cost-baseline-20261018.tsv` under `build/tawcroot-perf/`.

| Operation | Native median | tawcroot median |
| --- | ---: | ---: |
| libc fork + wait | 585.8 µs | 636.4 µs |
| raw fork + wait | 545.8 µs | 621.3 µs |
| fork + exec + wait | 1,135.3 µs | 1,724.9 µs |
| Child minor faults per spawn | 14.8 | 96.2 |

The spawn gap is 589.6 µs; subtracting the fork gap leaves roughly
539 µs as an investigation guide, not a causal decomposition. A separate
app-domain baseline with the installed NDK 27.2 runtime measured
1,701.0 µs/spawn (three rounds × 1,000); do not subtract shell-domain
native times from it. Raw: `exec-cost-app-baseline-20261018.txt`.

Isolated guest probes measured a 24 KiB state copy at 38.1 µs versus
31.8 µs for a shared mapping, only a 6.3 µs difference. Anonymous first
writes took 2.22 µs/page and COW writes 4.26 µs/page. Cached-file refaults
took 0.216 µs/page with 0.126 minor faults/page. These access patterns
cannot be reduced to a single price per minor fault. Native raw-fork
medians with low fds / fd 1000 / fd 1024 were 546.9 / 553.6 / 552.4 µs
(five rounds × 1,000); this did not identify a large high-fd penalty.
Raw: `exec-cost-fd-20261018.txt`.

### Separate stage profile

`build-exec-profile.py` instrumented a copied source tree, not the
production runtime. NDK 29, CPU 9, five warm-ups plus 50 measured child
execs. Markers cross re-exec through the copied state's shared mapping;
formatting happens after the final marker. Medians of individual stage
differences below do not necessarily add to the median total.
Raw: `exec-stages-baseline-20261018.txt`.

| Interval | Median |
| --- | ---: |
| Handler entry → internal exec | 222.8 µs |
| argv/env collection | 49.1 µs |
| ELF probe | 29.3 µs |
| Supervisor state snapshot | 30.7 µs |
| State serialization/write | 50.7 µs |
| Commit preparation | 25.0 µs |
| Internal exec → new runtime main | 609.1 µs |
| New runtime main → guest jump | 189.3 µs |
| Main → parsed state | 29.7 µs |
| Supervisor reconstruction | 106.2 µs |
| Restore exec state | 11.5 µs |
| ELF parse / map | 5.3 / 14.5 µs |
| Stack reserve / build | 6.1 / 11.9 µs |

The internal-exec interval includes startup before `main`; it is not
pure kernel time. The instrumented state layout, mappings and clock
calls also change costs. Use these intervals to locate work, then test
changes with normal binaries. CPU-wide `/proc/stat` deltas cannot
attribute kernel time to this process, and the probe's user/system CPU
split was too coarse to resolve these short child lifetimes reliably.

### Compact exec-state experiment: rejected

The candidate replaced fixed argv/env offset arrays with string-sequence
start offsets and existing counts. Header size fell from 22,412 to
1,940 bytes (experimental wire version 11). The acceptance condition was
a consistent roughly 2–3% or greater speedup, fewer faults and passing
correctness checks.

Both uninstrumented binaries used app-pinned NDK 27.2.12479018. The
baseline SHA-256 matches the installed dev runtime:
`3339bf55a58710c5e798642c4e8b305cdde33248d7852c2f0f868d38f337ed69`.
Candidate:
`54b39c2ea679029c26413d4ec34c7917ec0413b9ca723f182fa7a54e62190a67`.
Six rounds × 1,000 operations, CPU 9, native/baseline/candidate order
reversed on alternating rounds. Raw: `exec-cost-compact-ab-20261018.tsv`.

| Shell-domain metric | Baseline | Candidate |
| --- | ---: | ---: |
| Spawn median | 1,739.1 µs | 1,724.5 µs |
| Child minor faults per spawn | 100.23 | 95.22 |
| libc fork + wait | 637.1 µs | 639.2 µs |
| raw fork + wait | 612.9 µs | 611.6 µs |

Median spawn time improved only 0.84%. Paired round improvements ranged
from −1.64% to +9.47%; the median was −1.25% when the candidate ran later
and +3.09% when it ran first. About five fewer minor faults consistently
survived, but a consistent wall-time gain did not.

The app A/B replaced only `lib/arm64-v8a/libtawcroot.so` in the same
signed dev APK. CPU 9, four baseline/candidate pairs, alternating order,
nine measured runs plus a warm-up per workload. The first macro sample
also showed no useful gain (`true` 165 → 164 ms; bash 177.5 → 187.5 ms).
Review then found that the old harness ignored workload failures.
After adding failure/row validation and replacing `seq` spawn loops
with Bash arithmetic loops, all four pairs were repeated successfully:

| App workload | Baseline median | Candidate median | Candidate/baseline |
| --- | ---: | ---: | ---: |
| `spawn_true_x100` | 161 ms | 164 ms | 1.019 |
| `bash_startup_x50` | 172.5 ms | 173 ms | 1.003 |
| `stat_many` (control) | 57 ms | 54.5 ms | 0.956 |

These are medians of the four per-round medians. Spawn pair changes
ranged from 1.28% faster to 3.75% slower; bash baseline medians ranged
from 121 to 176 ms. The app run confirms no consistent process-launch
gain. Raw: `compact-app-strict-{baseline,candidate}-{1..4}.tsv` and
`compact-app-strict-20261018.log`. The script restored the original APK
afterwards. The compact production change was rejected and reverted;
probe tools and corrected test fixtures remain.

Full candidate correctness runs reproduced baseline's existing failures:
host 2,215 passed / 13 failed; device 1,691 passed / 10 failed (the
candidate added five tests). The failures concern stale guest seccomp
smoke expectations, since fixed (commit 2833049).
After reverting the candidate, all 19 exec-state tests pass on host and
device. Four malformed-state tests now start from a successfully written
state instead of an undersized buffer, and the capacity test exercises
the actual argv/env limits. Production retains wire version 10.

## 2026-10-18 ARM64 rewriting feasibility, S24

`run-rewrite-probe.sh --cpu 9 --iterations 200000 --rounds 7`, same
Galaxy S24 and baseline runtime SHA-256 `3339bf55…`. App execution was
through the installed Arch distro, with Android context
`u:r:untrusted_app_34:…`; native execution used `u:r:shell:s0`.
Raw log: `build/tawcroot-perf/rewrite-probe-s24-20261018.log`.

Both contexts passed near trampoline allocation (about 1 MiB from the
site), anonymous RW→RX, file-code RX→RWX→RX, instruction-cache flushes,
GPR/x30/SP/NZCV checks, four threads × 10,000 executions after patching,
and restoration of the original SVC. This confirms the basic technique
works in the S24 app context, not just adb shell.

| Controlled loop | Native median | App/tawcroot median |
| --- | ---: | ---: |
| Constant-return assembly call | 0.719 ns | 0.720 ns |
| Original getuid SVC | 149.656 ns | 1,002.263 ns |
| Patched fixed-return branch | 1.437 ns | 1.438 ns |

The patched path returns a marker instead of calling a real dispatcher or
the kernel. These are different operations; this table is **not a TAWC
speedup comparison**. Real path translation, argument checks, context
management and patch setup cost remain unmeasured. Concurrent publication,
late library/JIT mappings and FP/SIMD handling also remain untested.
The probe restored its code and the runner removed its device copies.

## 2026-10-18 shared exec arena experiment (rejected)

Candidate: argv/env collected straight into the final `MAP_SHARED`
state mapping, metadata appended in place, one mapping from collection
to commit (no intermediate copy or re-serialization). Same NDK 27.2 for
both builds (baseline runtime SHA-256 `3339bf55…`, candidate
`e749e0cc…`), `run-exec-cost.sh --rounds 8 --iterations 1500 --cpu 9`,
shell domain, interleaved order. Patch kept outside the tree at
`build/tawcroot-perf/exec-arena-rejected.patch`.

| Round | Baseline spawn | Arena spawn | Change |
| ---: | ---: | ---: | ---: |
| 1 | 1,719.9 µs | 1,702.8 µs | −1.0% |
| 2 | 1,715.7 µs | 1,715.4 µs | −0.0% |
| 3 | 1,719.4 µs | 1,700.1 µs | −1.1% |
| 4 | 1,786.0 µs | 1,672.6 µs | −6.4% |
| 5 | 1,734.4 µs | 1,731.9 µs | −0.1% |
| 6 | 1,709.8 µs | 1,705.0 µs | −0.3% |
| 7 | 1,727.7 µs | 1,659.7 µs | −3.9% |
| 8 | 1,726.6 µs | 1,715.7 µs | −0.6% |

Never slower, but the typical round moved under 1% (overall medians
1,723.2 → 1,703.9 µs, −1.1%), the same order as the rejected compact
state. ~700 changed lines in the exec path are not justified by it.

Its tests surfaced a real interaction instead: with lazy rescue
recording, an `EACCES` from execve's terminal re-exec re-ran the whole
handler twice more. exec is now excluded from the dispatch-level rescue
and its guest-path opens rescue individually (see
notes/tawcroot/path-translation.md §"DAC override").
