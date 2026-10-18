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

## 2026-10-18 where app-domain workloads spend time (trap-stats.py)

S24, Arch in the dev app, `-DTAWCROOT_STATS` runtime built from e300080,
`trap-stats.py --cpu 9`, broker fixed cost (`empty`) subtracted. Handler
time excludes SIGSYS delivery/return (~0.9 µs per trap on this device).

| Workload | Wall | Traps | Handler | Inner syscalls/trap (main traps) | /proc/self/fd readlinks |
| --- | ---: | ---: | ---: | --- | ---: |
| `find /usr` | 65 ms | 10,083 | 44 ms | fstatat 6.1 | 4,187 (dirfd lift 4,168) |
| `ls -lR /usr/lib` | 35 ms | 4,713 | 23 ms | statx ~2.5 | 16 |
| `grep -r` | 171 ms | 17,590 | 96 ms | openat 3.4, fstatat 6.3, fstat 3.4 | 14,770 (lift 9,155, fstat nlink 5,596) |
| `tar cf` | 219 ms | 25,548 | 132 ms | fstatat 5.0, openat 3.4, fstat 3.3 | 23,242 (lift 13,352, fstat nlink 9,856) |
| `pacman -Qk` | 194 ms | 36,755 | 116 ms | fstatat 3.2 | ~0 (path cache 81% hits) |
| `true` ×100 | 156 ms | 3,500 | 56 ms | — | — |

Path cache hit rate is 98–99% except first-visit paths (pacman 81%).
Handler time is the larger share of tawcroot overhead in metadata-heavy
work (55–70% of wall), not SIGSYS delivery. Per trap the handler issues
3–6 syscalls of its own, each roughly 1 µs or more on this kernel:
`/proc/self/fd/N` readlinks (dirfd-relative path lifting and the fstat
link-object probe in `fd_object_nlink`), and `process_vm_readv/writev`
guest copies.

Per spawned process (`true` loop): --exec-child init ≈ 220 µs — bind
table rebuild 64 µs, guest binary open/map 33 µs, shm/link store/path
cache 30 µs, ld.so map 27 µs, root fd/usercopy 21 µs, the rest ~40 µs;
~52 minor faults. Exec prepare in the parent ≈ 100 µs.

## 2026-10-18 fd path cache + object inode set

`/proc/self/fd` readlinks per workload (`trap-stats.py`, stats build):
`tar` 23,242 → 1,774, `grep -r` 14,770 → 1,474, `find` 4,187 → 286.
Dirfd lifts are now served by `fdpath.h` (one fstat to revalidate) and
fstat's object probe is skipped for inodes `linkobjs.h` rules out.

Production runtimes, `macro.sh --runs 7 --cpu 9`, 3 interleaved rounds,
only libtawcroot swapped in the same dev APK; medians of round medians:

| Workload | Before | After | After/before |
| --- | ---: | ---: | ---: |
| `tar_usr_share` | 202 ms | 168 ms | 0.83 |
| `grep_tree` | 160 ms | 137 ms | 0.86 |
| `find_usr` | 64 ms | 57 ms | 0.89 |
| `stat_many` | 34 ms | 31 ms | 0.91 |
| `ls_lR_usr_lib` | 32 ms | 32 ms | 1.00 |
| `pacman_Qk` | 193 ms | 191 ms | 0.99 |
| `spawn_true_x100` | 204 ms | 206 ms | 1.01 |

## 2026-10-18 stat results written straight into the guest buffer

Hot stat paths (`fstat`, path and `AT_EMPTY_PATH` `newfstatat`/`statx`)
now let the kernel fill the guest's buffer and patch the faked fields in
place, dropping one `process_vm_writev` per call.
`run-macro-shell.sh --rounds 3 --runs 7 --cpu 9` (adb shell domain, full
ArchLinuxARM-aarch64-latest tarball as rootfs, so absolute times differ
from the app-distro runs above), per-round medians:

| Workload | Before | After | After/before |
| --- | ---: | ---: | ---: |
| `stat_many` | 272 ms | 247 ms | 0.91 |
| `tar_usr_share` | 1,417 ms | 1,306 ms | 0.92 |
| `grep_tree` | 1,088 ms | 1,014 ms | 0.93 |
| `ls_lR_usr_lib` | 468 ms | 452 ms | 0.97 |
| `find_usr` | 230 ms | 223 ms | 0.97 |

## 2026-10-18 native vs tawcroot on the same tree (shell domain)

`native-vs-shell.sh` on the device after `run-macro-shell.sh` extracted
the full ArchLinuxARM tarball: the same GNU binaries run natively through
the rootfs `ld.so` (absolute paths prefixed with the rootfs, slightly
longer walks for native) and under the b258a30 runtime, alternating, 3
rounds × 7 runs on cpu9, per-round medians:

| Workload | Native | tawcroot | Ratio |
| --- | ---: | ---: | ---: |
| `grep -r` | 715 ms | 1,060 ms | 1.48 |
| `tar cf` | 732 ms | 1,304 ms | 1.78 |
| `ls -lR` | 217 ms | 449 ms | 2.07 |
| `find -type f` | 104 ms | 221 ms | 2.12 |

Metadata-only walks pay the most; workloads with real I/O and CPU work
dilute the per-trap cost. Spawn-heavy work and pacman are not in this
comparison.

## 2026-10-18 syscall-site rewriting prototype (`TAWCROOT_FASTPATH`)

Design in `notes/tawcroot/sigsys-handler.md` ("Experimental site
rewriting"). `run-macro-shell.sh`, shell domain, cpu9, full ArchLinuxARM
tarball. Baseline = same tree without the flag.

| Workload | Base | `=1` (signals blocked) | Ratio | `=2` (unsafe ceiling) |
| --- | ---: | ---: | ---: | ---: |
| `stat_many` | 181 ms | 163 ms | 0.90 | 0.85 |
| `tar_usr_share` | 954 ms | 878 ms | 0.92 | 0.87 |
| `find_usr` | 161 ms | 151 ms | 0.94 | 0.89 |
| `ls_lR_usr_lib` | 338 ms | 319 ms | 0.94 | 0.90 |
| `grep_tree` | 743 ms | 712 ms | 0.96 | 0.95 |
| `spawn_true_x100` | — | — | 1.00 | 1.00 |
| `bash_startup_x50` | — | — | ~1.0 (noisy) | 1.02 |

`=1` is 5 rounds × 7 runs; `=2` 3 rounds × 7 runs (separate session, so
compare its ratio, not its times). Signal delivery is only ~0.9 µs of a
path trap; most of the remaining cost is in the handlers themselves.

## 2026-10-19 incremental path-cache hashing

A `simpleperf record -e cpu-clock:u` profile of `find`/`ls -lR`/`tar`
under the fastpath build (shell domain; kernel samples are not
allowed without root) put 67% of user time in tawcroot, and
`hash_suffix` alone at 21% of that: the resolver probes every prefix,
and each probe rehashed and `strlen`ed the whole prefix. The oracle now
carries hash and length across a pass. Default build vs previous
commit, `run-macro-shell.sh --rounds 5 --runs 7 --cpu 9`:

| Workload | Before | After | After/before |
| --- | ---: | ---: | ---: |
| `ls_lR_usr_lib` | 401 ms | 371 ms | 0.93 |
| `stat_many` | 221 ms | 209 ms | 0.95 |
| `find_usr` | 186 ms | 183 ms | 0.98 |
| `grep_tree` | 1,016 ms | 1,005 ms | 0.99 |
| `tar_usr_share` | 1,249 ms | 1,236 ms | 0.99 |

Absolute times run higher than the 2026-10-18 tables (different
session and device state); compare ratios.

## 2026-10-19 dirfd leaf shortcut

`trap-stats.py --shell` (default build) showed handler time at 29–68%
of wall in find/ls/grep/tar, dominated by dirfd-relative `fstatat` and
`openat` (8–9 µs/trap): each single-name lookup was rewritten to the
whole root-relative path, re-checked prefix by prefix in userland and
re-walked by the kernel from the root. Plain read lookups now go to
`(dirfd, name)` (`notes/tawcroot/path-translation.md`, "Leaf
shortcut"). `run-macro-shell.sh --rounds 5 --runs 7 --cpu 9`:

| Workload | Before | After | After/before |
| --- | ---: | ---: | ---: |
| `tar_usr_share` | 1,223 ms | 1,152 ms | 0.94 |
| `grep_tree` | 989 ms | 938 ms | 0.95 |
| `find_usr` | 183 ms | 174 ms | 0.95 |
| `stat_many` | 210 ms | 203 ms | 0.97 |
| `ls_lR_usr_lib` | 368 ms | 370 ms | 1.01 |

`ls -lR` names files relative to the cwd, not a dirfd, so it is
unaffected; its largest cost is `llistxattr` (8.5 µs/trap, 30% of wall),
routed through `/proc/self/fd/<rootfs>/…`.

## 2026-10-19 fcntl filter leaf, xattr host path

`fcntl` trapped on every call (12,488 traps in `find_usr`, mostly
gnulib's `F_DUPFD_CLOEXEC`/`F_GETFD`), though the handler only acts on
reserved fds and the emulated shm seal ops; the filter now traps only
those, like `close`. Rootfs `*xattr` targets go to the kernel as the
root's host path instead of `/proc/self/fd/<root>/…`.

The phone was dozing (cpu9 capped at 1.63 GHz) and alternated fast and
3× slow rounds, so `run-macro-shell.sh` now also reports the ratio of
the fastest runs (5 rounds × 9 runs, both changes vs `bdfa524`):

| Workload | Min ratio |
| --- | ---: |
| `find_usr` | 0.865 |
| `stat_many` | 0.914 |
| `grep_tree` | 0.958 |
| `ls_lR_usr_lib` | 0.991 |
| `tar_usr_share` | 1.003 |

The xattr change alone, from `trap-stats.py --shell` on `ls_lR_usr_lib`
alternating three times: `llistxattr` 8.18/8.25/9.08 → 7.48/7.23/7.35
µs/trap, with `statx` steady at 6.0–6.1 µs as the control (about
14 ms of a ~390 ms run).

## 2026-10-19 getdents64 classified by device

Every `getdents64` batch read `/proc/self/fd/N` to tell rootfs, bind and
procfs listings apart. An `fstat` device check now decides; only procfs
listings still read the link. 5 rounds × 9 runs vs `3fb9a88` (one or two
dozing rounds per side; medians shown):

| Workload | Before | After | After/before |
| --- | ---: | ---: | ---: |
| `find_usr` | 154 ms | 147 ms | 0.955 |
| `ls_lR_usr_lib` | 356 ms | 350 ms | 0.98 |
| `grep_tree` | 956 ms | 947 ms | 0.99 |
| `tar_usr_share` | 1,173 ms | 1,157 ms | 0.99 |

## 2026-10-19 prefix matching without strlen; instruction counts

`route_through_binds` (up to three calls per translation) and
`apply_memo` took `strlen` of the whole path before comparing against
each bind/memo, though nearly every miss is decided by the first byte;
`path_scratch_acquire` CASed every busy slot. With the phone dozing,
wall-time A/B could not resolve this, so the comparison is
`simpleperf stat -e instructions:u` over the whole run (guest +
tawcroot user mode; kernel work not included), two identical rounds:

| Workload | Before | After | Change |
| --- | ---: | ---: | ---: |
| `ls -lR /usr/lib` | 327.65 M | 303.89 M | −7.3% |
| `find /usr -type f` | 82.35 M | 82.34 M | 0 |
| `tar cf - /usr/share` | 186.94 M | 187.10 M | +0.1% |

find and tar mostly take the dirfd leaf shortcut, which skips the
orchestrator. Instruction counts repeat to ~0.01% between runs, which
makes them the better gate for small user-space changes on a busy or
dozing phone.

## 2026-10-19 fastpath: mask signals only once a handler exists

`TAWCROOT_FASTPATH=1` builds, `run-macro-shell.sh --rounds 5 --runs 9
--cpu 9`, always-mask vs mask-after-first-handler (stable rounds):

| Workload | Always mask | Conditional | Ratio |
| --- | ---: | ---: | ---: |
| `stat_many` | 176 ms | 158 ms | 0.90 |
| `tar_usr_share` | 1,086 ms | 995 ms | 0.92 |
| `find_usr` | 138 ms | 128 ms | 0.93 |
| `ls_lR_usr_lib` | 345 ms | 320 ms | 0.93 |
| `grep_tree` | 922 ms | 916 ms | 0.99 |

The same session's default-build vs always-mask run was noisy (two fast
rounds per side); its fastest-run ratios were 0.88–0.96. Combined, the
fastpath build is roughly 0.82–0.88 of the default build on these
workloads.

## 2026-10-19 native vs tawcroot, after the 10-18/19 changes

`native-vs-shell.sh` (same tree, 3 rounds × 7 runs, cpu9, phone dozing
for part of it), medians of per-round medians. "Before" is the
2026-10-18 table above (`b258a30`).

| Workload | Before | Default build (`d745d0e`) | `TAWCROOT_FASTPATH=1` (`2573c40`) |
| --- | ---: | ---: | ---: |
| `find -type f` | 2.12× | 1.71× | 1.32× |
| `ls -lR` | 2.07× | 1.76× | 1.51× |
| `tar cf` | 1.78× | 1.57× | 1.35× |
| `grep -r` | 1.48× | 1.34× | 1.36× |
