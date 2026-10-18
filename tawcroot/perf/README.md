# tawcroot perf harness

Standalone microbenchmarks for comparing:

- `native` host execution against a rootfs path prefix
- `tawcroot -r ROOTFS -- CMD`
- `proot -r ROOTFS CMD`
- real `chroot ROOTFS CMD`
- gVisor `runsc` with the rootfs as an OCI bundle root

This is not part of `tawcroot/test.sh` or the integration suite. It is
for local performance runs only.

Past results are recorded in [results.md](results.md); compare new runs
against those baselines and append significant runs there.

## Quick start

Build host tawcroot if needed:

```sh
tawcroot/build.sh --abi=host
```

Run the available backends:

```sh
tawcroot/perf/run-perf.sh --rootfs /path/to/rootfs
```

Run a focused comparison:

```sh
tawcroot/perf/run-perf.sh \
  --rootfs /path/to/rootfs \
  --modes native,tawcroot,proot \
  --iterations 50000 \
  --rounds 5
```

The runner writes TSV to `build/tawcroot-perf/results-*.tsv` and prints
mean/median/min/max `ns_per_op` summary rows.

## On a device

`run-perf-device.sh` runs native vs tawcroot on an arm64 phone
(`TAWC_TARGET` / `ANDROID_SERIAL`), from the adb `shell` domain against
a minimal rootfs in the scratch dir, with interleaved rounds:

```sh
tawcroot/build.sh --abi=aarch64
TAWC_TARGET=physical tawcroot/perf/run-perf-device.sh \
  --rounds 10 --iterations 20000 --cpu 200
```

`--cpu` pins with `taskset` (hex mask; pick the prime core — unpinned
runs swing 2x with core migration). The shell domain has no app
seccomp filter or `untrusted_app` SELinux policy, so syscalls Android
traps for apps (e.g. `openat2` on 16+) still work here; confirm
anything policy-sensitive in the app. The bench needs a static-capable
`aarch64-linux-gnu-gcc` (or `--cc` / `--bench-bin`).

### Exec cost investigation

Separate fork from exec before attributing process-launch overhead:

```sh
TAWC_TARGET=physical tawcroot/perf/run-exec-cost.sh --rounds 5 --cpu 9
```

This compares native and tawcroot in the shell domain using the same
static glibc probe and a guest with no libc startup. It records wall
time, parent/child CPU time and actual minor/major fault deltas. The
kernel probes cover mappings, memfd state transfer and page faults.
`--cpu` takes a CPU list; the runner converts it to Android's hex mask.
Raw TSVs and the runtime hash are saved under `build/tawcroot-perf/`.
Use `--tawcroot BASELINE --compare CANDIDATE` for an interleaved comparison.
App-domain performance still needs a separate broker/real-workload A/B.
Build both runtimes with the same `ANDROID_NDK_HOME`; use the app's
pinned NDK when comparing against an APK.

For internal exec stages, `python3 tawcroot/perf/build-exec-profile.py`
builds an isolated instrumented copy under `build/tawcroot-perf/` using
the NDK flags from `build.sh`. It does not stage into the APK. Launch
the resulting runtime directly from the shell; its fixed syscall-stub
address belongs to that binary. Each successful load prints one
`TAWC_EXEC_PROFILE` row before entering the guest. `kind=child` rows
contain pre/post absolute monotonic timestamps across the re-exec.
Output formatting occurs after `post.jump`; use stage differences for
attribution and the normal binary for performance comparisons.

The profiler extends the copied exec-state header and transports markers
through a shared mapping. Its overhead prevents treating the instrumented
spawn time as the production baseline. Minor faults also include demand
zero, cached-file faults and COW; file fault-around means touched pages
and fault counts differ. CPU-wide `/proc/stat` changes cannot identify a
particular process's kernel time.

For app A/B without rebuilding unrelated APK components, use
`repack-runtime-apk.sh DEV_APK ARM64_RUNTIME OUTPUT_APK`. It changes one
runtime entry, aligns/signs the APK, and checks the original debug
certificate. Use the same input APK for both builds. `macro.sh --cpu 9
--workloads spawn_true_x100,bash_startup_x50,stat_many` narrows the real-app
comparison to process launch and a filesystem control.
Workload failures or missing timing rows make the macro run fail before
writing a result file.

### ARM64 rewriting feasibility

```sh
TAWC_TARGET=physical tawcroot/perf/run-rewrite-probe.sh --cpu 9
```

This builds a static probe and runs the same binary in the native shell
and the installed distro through the app broker. It records the app
SELinux context and installed runtime hash, and cleans both scratch
copies. Select the install with `TAWC_INSTALL_ID` if needed.

The probe patches one controlled `getuid` SVC to a nearby fixed-return
trampoline. It checks RX/RWX transitions, instruction-cache flushes,
GPR/SP/NZCV preservation, execution by four threads after patching, and
restoring the original instruction. Patching occurs before thread creation;
concurrent patch publication and FP/SIMD context are outside this probe.
Exit 77 means unavailable, 1 means a failed check, and 0 means success.

`patched_fixed_return` measures a tiny branch sequence, not real TAWC
dispatch, argument validation or path translation. Its timing cannot be
reported as an application speedup. This uses the same optional static
aarch64 glibc toolchain as the exec-cost probe; no production dependency
or runtime patching is enabled.

## Real workloads without the app

`macro.sh` and `trap-stats.py` go through the app's exec broker, which
starts the app's activity on the phone (and swapping APKs relaunches it
every round). For runtime-vs-runtime A/B without touching the app or the
screen, `run-macro-shell.sh` extracts a distro tarball into the device
scratch dir once and runs both runtimes from the adb `shell` domain:

```sh
TAWC_TARGET=physical tawcroot/perf/run-macro-shell.sh \
  --rootfs-tar build/tawcroot-perf/ArchLinuxARM-aarch64-latest.tar.gz \
  --tawcroot BASE.so --compare CANDIDATE.so --rounds 3 --cpu 9
tawcroot/perf/run-macro-shell.sh --cleanup   # remove the rootfs
```

The rootfs stays on the device between runs (pass `--rootfs-tar` only
the first time). Shell domain lacks the app's SELinux/seccomp context,
so compare runtimes with it, not absolute app numbers.

## Where real workloads spend their time

`trap-stats.py` attributes app-domain workload time to tawcroot. Build a
measurement runtime with `EXTRA_CFLAGS=-DTAWCROOT_STATS tawcroot/build.sh
--abi=aarch64` (production builds compile the hooks out; see
`include/stats.h`), swap it in with `repack-runtime-apk.sh`, then:

```sh
TAWC_TARGET=physical tawcroot/perf/trap-stats.py --cpu 9 [--workloads a,b]
```

Per workload it prints wall time, trap counts with handler time and the
syscalls tawcroot itself issued per trap, path cache hit rate, the
sources of `/proc/self/fd` readlinks, and `--exec-child` init split into
phases with minor faults. Handler time excludes SIGSYS delivery and
return. Reinstall the production runtime afterwards.

## Local competitor tools

These tools are only for local perf runs. Do not add them to the normal
app build.

For x86_64 hosts, install local `proot` and `runsc` binaries under
`build/tawcroot-perf/tools/`:

```sh
mkdir -p build/tawcroot-perf/tools
cd build/tawcroot-perf/tools

gvisor_base="https://storage.googleapis.com/gvisor/releases/release/latest/$(uname -m)"
curl -fsSLO "$gvisor_base/runsc"
curl -fsSLO "$gvisor_base/runsc.sha512"
sha512sum -c runsc.sha512
chmod 0755 runsc

curl -fL -o proot https://proot.gitlab.io/proot/bin/proot
chmod 0755 proot
```

Then pass them explicitly:

```sh
tawcroot/perf/run-perf.sh \
  --rootfs /path/to/rootfs \
  --modes native,tawcroot,proot,gvisor \
  --proot build/tawcroot-perf/tools/proot \
  --runsc build/tawcroot-perf/tools/runsc \
  --iterations 50000 \
  --rounds 20 \
  --interleave
```

The older GitHub `proot-static-build` v5.1.1 binaries segfaulted on the
current dev host during testing. Use the current upstream PRoot binary
unless you specifically want to compare against that old build.

## Requirements

- A Linux rootfs matching the machine architecture.
- A static C compiler path, usually `musl-gcc` or a host `cc` with
  static libc available. Alternatively pass `--bench-bin PATH`.
- `build/tawcroot-host/tawcroot` for the `tawcroot` backend, or pass
  `--tawcroot PATH`.
- Optional: `proot` for the `proot` backend.
- Optional: `runsc` for the `gvisor` backend.
- Optional: root, or `--sudo-chroot`, for the `chroot` backend.

For rootless gVisor, the rootfs path must be traversable by the current
user. A disposable test rootfs should normally have at least:

```sh
chmod 0755 /path/to/rootfs
chmod 1777 /path/to/rootfs/tmp
```

The benchmark binary is copied into:

```text
<rootfs>/tmp/tawcroot-perf/tawcroot-perf
```

The workload creates files under:

```text
<rootfs>/tmp/tawcroot-perf/work
<rootfs>/tmp/tawcroot-perf/create
```

## Workloads

- `getpid`: baseline syscall loop.
- `stat`: repeated path metadata lookups.
- `open_read_close`: repeated path open/read/close.
- `readdir_entries`: repeated directory scans.
- `create_write_unlink`: create/write/delete cycle.
- `fork_exec_wait`: small fork/exec/wait loop.
- `gettid`: untrapped syscall (filter cost only).
- `getuid`: trapped, no path work (the SIGSYS round-trip floor).
- `fstat`, `write_read_pipe`: hot fd-only syscalls.
- `stat_deep`, `stat_leaf_symlink`, `stat_relative`, `fstatat_dirfd`:
  path shapes that stress the resolver (8 components, a relative leaf
  symlink, cwd-relative, dirfd-relative).
- `signal_raise`: tgkill + delivery + trapped `rt_sigreturn`.

These are microbenchmarks, not application benchmarks. Use them to spot
large syscall-path regressions, then validate with real workloads such
as package manager operations or browser startup.

## Reading results

Prefer `median` over `mean` when comparing backends. `min`/`max` are
printed so scheduler and CPU-frequency noise is visible.

Use at least `--rounds 20` for numbers worth discussing. If a result
looks surprising, rerun with a different backend order, for example:

```sh
tawcroot/perf/run-perf.sh \
  --rootfs /path/to/rootfs \
  --modes proot,tawcroot,native,gvisor \
  --proot build/tawcroot-perf/tools/proot \
  --runsc build/tawcroot-perf/tools/runsc \
  --iterations 50000 \
  --rounds 20
```

Treat `getpid` as a sanity check, not a rootfs benchmark. Some backends
may fast-path it without exercising path translation.

The runner always executes all rounds for one backend before moving to
the next backend unless `--interleave` is passed. Backend-major order is
simple and reproducible, but it can preserve cache and CPU-frequency
ordering effects. For careful comparisons, use `--interleave`, compare
at least two backend orders, or inspect the raw TSV.
