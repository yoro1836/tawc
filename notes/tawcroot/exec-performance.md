# Exec performance investigation

The 2026-10-18 S24 investigation separated fork, state transfer, internal
re-exec and guest loading. Measurements and the rejected compact-state
experiment are in [perf/results.md](../../tawcroot/perf/results.md);
commands and probe limitations are in
[perf/README.md](../../tawcroot/perf/README.md#exec-cost-investigation).
The follow-up [execution-technique survey](execution-techniques.md)
compares ARM64 rewriting, launch fusion, fork servers and exec redesign.

The isolated stage profile put roughly 223 µs before internal exec and
189 µs between the new runtime's `main` and guest entry. Supervisor
reconstruction accounted for 106 µs of the latter. The 609 µs between
the exec marker and `main` includes early user-space startup as well as
kernel work. Profiling changes the state layout and adds clock calls;
these values locate work, rather than serving as a production baseline.

Shrinking the exec-state header from 22,412 to 1,940 bytes removed about
five minor faults per spawn, but improved the controlled shell median
by only 0.84%, with a strong ordering effect. It was rejected and
production remains on wire version 10. Lower syscall or fault counts
alone do not justify a more complicated state handoff.

## Attribution constraints

- Keep the compiler, runtime, rootfs, CPU affinity and execution domain
  the same for A/B comparisons. Standalone `build.sh` can select a newer
  NDK than the app; set `ANDROID_NDK_HOME` explicitly. Repacking only
  the runtime in one dev APK keeps the rest of the app constant.
- Minor faults cover demand-zero, cached-file and COW paths. Linux's
  [file fault-around implementation](https://github.com/torvalds/linux/blob/v6.1/mm/memory.c#L4176)
  can map neighboring pages, so touched pages and fault counts differ.
  Measure actual fault deltas and access-path timings separately.
- [`/proc/stat`](https://docs.kernel.org/6.1/filesystems/proc.html#miscellaneous-kernel-statistics-in-proc-stat)
  reports system/CPU totals. Its changes cannot establish the kernel
  time of one child process. Do not repeat the earlier unproven
  attribution of roughly 500 µs to process-specific kernel work.
- Compare uninstrumented runtimes for adoption, alternate run order,
  and confirm microbenchmark results through the app's exec broker.

## Loader source research

The current mapper clears a segment's last partial file page even when
`p_memsz == p_filesz`; a read-only page then needs temporary write
permission. The [ELF ABI](https://refspecs.linuxfoundation.org/elf/gabi4%2B/ch5.pheader.html)
requires zero-fill for the extra `p_memsz > p_filesz` region.
[glibc](https://github.com/bminor/glibc/blob/master/elf/dl-map-segments.h#L136)
conditions tail clearing on that extra region. Linux 6.1's
[main ELF loader](https://github.com/torvalds/linux/blob/v6.1/fs/binfmt_elf.c#L1169)
and interpreter path differ, so they do not support a claim that every
native loader handles tail pages identically. No loader change was
included in this experiment; its measured mapping stage was only
about 14.5 µs.
