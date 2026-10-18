# Alternative execution techniques

Source research on 2026-10-18, after the compact exec-state experiment.
This records feasibility and hypotheses, not measured improvements or
an implementation plan. Current timings are in
[exec-performance.md](exec-performance.md) and
[perf/results.md](../../tawcroot/perf/results.md).

## Candidates by target

| Technique | Work it could avoid | Scope / assessment |
| --- | --- | --- |
| ARM64 syscall rewriting with SIGSYS fallback | Signal delivery and return at patched syscall sites | Most interesting new interception technique; affects repeated guest syscalls, not just exec |
| Pass a clean guest environment at initial launch | Loading `/usr/bin/env` and its subsequent internal re-exec | Relatively bounded top-level launch change; no effect on an existing shell's fork/exec loop |
| Single-thread native fork server | Repeated runtime/supervisor initialization for new app launches | Larger lifecycle/IPC change; cannot transparently replace exec in an existing guest PID |
| One shared per-exec arena | Repeated argv/env length scans, copies, serialization and remapping | Built and measured: −1.1% spawn median on the S24, rejected (results.md) |
| In-process guest exec | Kernel re-exec and runtime reconstruction | Large potential scope, but Linux exec semantics and shared-VM children make general use difficult |

## ARM64 rewriting

[svc-hook](https://github.com/retrage/svc-hook) replaces an ARM64 `svc`
instruction with a same-width `b` to a nearby trampoline (within about
128 MiB). Its stated targets include Android. The
[implementation](https://github.com/retrage/svc-hook/blob/main/main.c)
changes code-page protections, preserves context, handles clone/signal
special cases and flushes instruction caches. It is useful reference
material, not a library already compatible with tawcroot.

[gVisor's systrap description](https://gvisor.dev/blog/2023/04/28/systrap-release/)
also demonstrates replacing frequently trapped syscall sites with direct
user-space jumps; that article's patching implementation is x86-specific.
[ASC-Hook](https://arxiv.org/html/2412.05784v2) is another ARM design, but
its limitations include low-address trampoline placement, late-loaded
libraries and signal-handler conflicts. Published hook microbenchmarks
are not predictions of tawcroot workload speedups.

A tawcroot-specific hybrid could retain the existing seccomp/SIGSYS path
for unpatched sites and patch only verified hot sites. This avoids
requiring every DSO or JIT page to be rewritten immediately. The current
same-process dispatcher and fixed raw-syscall stub remain useful.
Prefer initially limiting direct dispatch to ordinary path/fd operations;
clone, exec and signal-return require their own context/stack handling.
Do not replace already-untrapped calls with slower generic dispatch.

The [controlled S24 probe](../../tawcroot/perf/results.md#2026-10-18-arm64-rewriting-feasibility-s24)
passed near trampoline allocation, code protection transitions,
GPR/SP/NZCV preservation, four-thread execution after patching and original
code restoration in the app domain. It returns a constant rather than
calling the real dispatcher, so it establishes feasibility, not an
application speedup. FP/SIMD preservation, guest-stack space, concurrent
publication, unmapping/remapping and late library loading still need
explicit treatment. Android execution policy must be checked as the app,
not only adb shell.
The current [mapper](../../tawcroot/src/loader_map.c) already preserves
execute permission while temporarily writing ELF tail pages. The
[Android 6.1 SELinux hook](https://android.googlesource.com/kernel/common/+/refs/heads/android14-6.1/security/selinux/hooks.c)
distinguishes adding execute permission to a modified private mapping;
a generic RW-to-RX recipe cannot simply be assumed to work.

## Launch changes

[TawcrootMethod.rootfsArgv](../../app/src/main/java/me/phie/tawc/install/TawcrootMethod.kt)
prepends `/usr/bin/env -i K=V...`; `prod_main` in
[main.c](../../tawcroot/src/main.c) loads it as the first guest.
[GNU env](https://github.com/coreutils/coreutils/blob/master/src/env.c)
then execs the requested command. A launcher which supplies the final
guest environment and loads that command directly could eliminate one
whole exec per top-level launch. This is a source-derived opportunity;
its elapsed-time benefit has not been measured.

The initial kernel exec must already receive the correct guest envp if
`/proc/<pid>/environ` is to remain truthful. Changing only the synthesized
ELF stack is insufficient. ProcessBuilder, PTY JNI and remote launches
need consistent environment replacement, including TERM/COLORTERM,
PATH, cwd and separation from host TMPDIR. The current PTY dependency
adds environment entries with `putenv`, rather than clearing inherited
ones, so this is more than deleting an argv prefix.

An [Android-style zygote](https://source.android.com/docs/core/runtime/zygote)
or [Chromium-style fork server](https://chromium.googlesource.com/chromium/src/+/main/docs/linux/zygote.md)
would instead keep a separately launched, single-thread native runtime
ready to fork. It must own stdio/PTY fd transfer, sessions, child exit
reporting and configuration/distro lifetime. Forking the Android JVM
process is not the proposed design. A server-created child cannot stand
in for an existing guest's exec without changing PID and parentage.
Without kernel exec, its kernel cmdline/environ also remain the server's;
per-request metadata needs a separate solution. The current proctitle
rewrite is bounded by the initial kernel argument region.

## Guest exec with and without kernel re-exec

With kernel re-exec retained, the per-exec arena could become the final
shared memfd mapping from collection through commit. Current
[syscalls_exec.c](../../tawcroot/src/syscalls_exec.c) collects strings,
[exec_handler.c](../../tawcroot/src/exec_handler.c) serializes them and
[exec_state.c](../../tawcroot/src/exec_state.c) repeats length checks.
Collection/serialization/commit account for roughly 49/51/25 µs in the
instrumented run, only some of which is redundant. Batched safe guest
reads and failed-exec cleanup must survive; each concurrent/shared-VM
exec needs independent ownership. Merely changing copy to shared mapping
targets the already measured small difference of about 6.3 µs.

Immutable supervisor descriptors could reuse normalized root/bind
configuration, but not blindly cache mutable symlinks, cwd, identity or
shm. Guest chroot/mount and external writes require appropriate versioning
or revalidation. Reusing root/bind/store fds repeats the rejected fd
inheritance experiment. The 106 µs supervisor stage is not all removable.

Eliminating kernel re-exec is a substantially different project. In
[glibc posix_spawn](https://github.com/bminor/glibc/blob/master/sysdeps/unix/sysv/linux/spawni.c),
the child shares VM with its parent using CLONE_VM|CLONE_VFORK. Linux's
[exec release path](https://github.com/torvalds/linux/blob/v6.1/kernel/fork.c)
wakes the waiting parent. Unmapping guest memory and jumping to another
ELF would instead alter the parent's VM and fail that release protocol.
Current tawcroot allows CLONE_VM directly and does not track ownership
well enough to select a safe special case. Kernel exec also handles
other threads, CLOEXEC/shared fd tables, signal dispositions/altstack,
TLS, robust futexes, brk and process metadata; see the
[execve contract](https://man7.org/linux/man-pages/man2/execve.2.html).
An ordinary-fork, private-VM, single-thread subset would need proof before
it could use a separate fast path.

## Lower priority or unavailable replacements

- **SUD:** Linux 6.1 ARM64 does not implement Syscall User Dispatch;
  kernel version 5.11+ alone is not sufficient. The
  [current manual](https://man7.org/linux/man-pages/man2/PR_SET_SYSCALL_USER_DISPATCH.2const.html)
  identifies x86 support and notes that settings do not survive
  fork/clone/exec. SUD also still delivers SIGSYS.
- **seccomp user notification:** the
  [Linux 6.1 interface](https://www.kernel.org/doc/html/v6.1/userspace-api/seccomp_filter.html)
  adds a listener and request/reply operations; it cannot relax the
  inherited Android filter. No evidence shows that replacing tawcroot's
  local handler with that RPC would improve its exec workload.
- **gVisor shared queues/spinning:** solve communication between Sentry
  and sandbox tasks. Tawcroot already handles calls in the issuing
  thread, without that IPC. Adding a queue alone removes no current cost.
- **io_uring/eBPF:** Android 17's
  [app-domain policy](https://android.googlesource.com/platform/system/sepolicy/+/android17-release/public/te_macros)
  blocks io_uring, and the
  [BPF policy](https://android.googlesource.com/platform/system/sepolicy/+/android17-release/private/bpfloader.te)
  restricts program loading to bpfloader. Neither is a drop-in rootless
  path-translating exec mechanism.
- **ELF plan caches/persistent pools:** pre-exec currently probes an ELF
  header, not a full reusable load plan. Post-exec ELF parse measured
  only 5.3 µs; identity, permissions and mutable file contents still need
  validation. A pool of reusable guest VM slots cannot survive kernel
  exec without structural change.

The useful split is interception cost versus guest exec cost versus
top-level app launch. A technique's success in one does not establish an
improvement in the others. The rewriting probe is isolated from the
production runtime and adds no runtime dependency.
