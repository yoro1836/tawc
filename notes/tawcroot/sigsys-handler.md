# tawcroot — SIGSYS handler

## Robust-list compatibility on Android

Native `set_robust_list` / `get_robust_list` remain kernel operations when allowed.
On Android `ENOSYS`, tawcroot retains the actual per-thread registration and
supports querying the calling thread (pid 0 or its tid). This lets clients inspect
glibc's list without depending on libc-private TLS offsets. Foreign-thread queries
remain unsupported. This mode does **not** implement automatic owner-death cleanup
or wakeups; a thread dying while holding such a lock can leave its peers blocked.
The bounded registry reclaims dead-thread entries on exhaustion. No exit syscall
trap is added, because musl can unmap its stack before issuing exit.

Test through an Android-spawned process: ADB/run-as does not inherit Zygote's app
seccomp policy and cannot establish compatibility with the production launch path.

## SIGSYS handler

The hot path. Called any time the seccomp filter returns
`SECCOMP_RET_TRAP`. Signature:

```c
static void sigsys_handler(int sig, siginfo_t *info, void *ucontext);
```

`info->si_code == SYS_SECCOMP` distinguishes seccomp-originated
SIGSYS from kernel-fault SIGSYS (we abort if it's not seccomp —
something is very wrong).

`info->si_syscall` and `info->si_arch` give the syscall number and
arch (we'll only ever see one arch per build, but assert it
matches). The arguments are in the saved register state inside
`ucontext`, which is `ucontext_t *`.

The handler structure:

```c
{
    syscall_no = info->si_syscall;
    args = read_args_from_ucontext(uc);   // arch-specific
    pc   = read_pc_from_ucontext(uc);

    rv = tawcroot_dispatch_call(args, uc); // table lookup + handler,
                                           // wrapped by the lazy
                                           // DAC-override rescue

    write_return_value_to_ucontext(uc, rv);
    // ucontext is restored by the kernel on sigreturn
}
```

`dispatch_table` is a fixed array of function pointers indexed by
syscall number. Most entries are `NULL`. For a NULL entry we
return `-ENOSYS` — **not** abort. Android's stacked
`untrusted_app` filter can TRAP syscalls we don't handle (TRAP
beats ALLOW across stacked filters; see §"Seccomp filter"), and
future Android versions may TRAP additional syscalls. Aborting on
an unexpected TRAP would make us fragile to those changes.
`-ENOSYS` is what the kernel returns for unsupported syscalls and
is the value programs check to fall back gracefully.

The call goes through `tawcroot_dispatch_call` (`rescue.c`) rather
than the table directly: it claims a per-thread rescue slot, runs the
handler, and — only when the handler returns `-EACCES` at virtual euid
0 — widens the app-owned inodes along the routes the translator
recorded and re-runs the handler once. See
notes/tawcroot/path-translation.md §"DAC override"; on every other
outcome it is the bare table lookup: routes are only recorded, by
re-running the handler once, after a first run returned `-EACCES`.

### Guest memory access

The guest's pointers are in our address space, but they are still
untrusted syscall arguments. A bad path pointer, unterminated string,
or invalid output buffer must return the same error the kernel would
return (`-EFAULT` or `-ENAMETOOLONG`), not crash tawcroot from inside
the SIGSYS handler.

All guest-pointer access therefore goes through tiny `copy_from_guest`,
`copy_string_from_guest`, and `copy_to_guest` helpers. They are
handler-safe and libc-free. Prefer raw `process_vm_readv` /
`process_vm_writev` against our own task id: the kernel validates the
guest address and returns `-EFAULT` without delivering SIGSEGV to
tawcroot. The helpers build small stack-local `iovec`s and issue the
syscalls through `tawcroot_raw_syscall()`. Never use `memcpy`/`strlen`
directly on guest pointers.

`process_vm_*` against self always passes the kernel's access check
(`__ptrace_may_access` short-circuits for the same thread group before
Yama/SELinux run), and Android's app seccomp policy allows it, so
production needs no fallback; `supervisor_init` fails fast (exit 92)
if the probe fails. A `-EPERM` probe failure means a seccomp errno
filter on the *host* — e.g. a container sandbox without
`CAP_SYS_PTRACE` (Docker's default profile) — and the host test
harnesses detect this and bail with one clear message instead of
cascading `-EFAULT`s. A SIGSEGV-fixup fallback copy path was
considered and rejected: tawcroot only virtualizes SIGSYS, and taking
ownership of guest-owned SIGSEGV (used by JITs, e.g. Firefox wasm)
isn't worth it for a case production never hits. If a real fallback is
ever needed (sandboxed hosts), prefer the pipe trick — `write(pipe_wr,
guest_src, n)` gets kernel address validation with clean `-EFAULT` and
partial counts, reading back completes the copy — over signal games.

Cost: one syscall per copy. The pid passed to `process_vm_*` is the
tgid (any thread of the group addresses the shared VM), cached on a
`MADV_WIPEONFORK` page: a forked child reads 0 and falls back to a
per-call `gettid` until the trapped-clone child path refills it
(`tawc_usercopy_refresh_pid`). Never refill lazily on a 0 read — a
vfork child shares the page and would leave its own pid behind after
its execve. A zombie leader (main thread exited, siblings alive)
returns `-ESRCH`, which retries by tid. String reads take 512-byte
chunks split into two remote iovecs at the 4 KiB boundary, so a
string ending just before an unmapped page comes back as a partial
read instead of `-EFAULT` for the whole chunk.

These helpers are used for:

- path strings on every path-bearing syscall;
- `readlink`/`readlinkat`, `getcwd`, and other output buffers;
- `execve`/`execveat` argv and envp pointer arrays and strings.

Do not use plain `memcpy`, `strlen`, or ad-hoc pointer walking on
guest-provided addresses in handler code.

One deliberate exception: the hot stat paths (`fstat`, path and
`AT_EMPTY_PATH` `newfstatat`/`statx`) pass the guest's output buffer to
the kernel and then patch the faked fields (uid/gid, emulated-hardlink
count, gamepad nodes) in place (`finish_stat_direct`). The kernel
validates the pointer and EFAULTs exactly like a native call; the
handler only touches the buffer after that write succeeded, so it is
mapped and writable. The residual risk is a sibling guest thread
unmapping the buffer in between — a guest racing its own syscall. This
saves one `process_vm_writev` per stat. Shadow stats (shm, /proc) still
copy. Covered by `test_fs_handlers.c::hosted_stat_family_bad_out_pointer_efaults`.

### Reading and writing the saved registers

The siginfo/ucontext shapes are arch-specific. We isolate this in
`arch/<arch>.h` with two inline helpers:

```c
static inline void arch_read_args(ucontext_t *uc, syscall_args *out);
static inline void arch_write_return(ucontext_t *uc, long rv);
```

`x86_64` (`uc->uc_mcontext.gregs`):
- args: RDI, RSI, RDX, R10, R8, R9 → `REG_RDI, REG_RSI, REG_RDX, REG_R10, REG_R8, REG_R9`.
- return: RAX.
- pc: RIP.

`aarch64` (`uc->uc_mcontext.regs[]`):
- args: x0..x5.
- return: x0.
- pc: pc.

Both archs are needed (real device is aarch64, emulator is x86_64).
We do not need to handle 32-bit personalities — Android's lp32
filter exists but TAWC is lp64 across the board.

### Issuing host syscalls from the handler

We can't just call `syscall(2)` from glibc because (a) glibc's
`syscall` may go through a vDSO/PLT path the seccomp filter would
trap, and (b) we want a single, allowlistable instruction-pointer
range. Instead, a hand-written stub:

```asm
; arch/x86_64/stub.S
.global tawcroot_raw_syscall
.type   tawcroot_raw_syscall, @function
tawcroot_raw_syscall:
    mov     %rdi, %rax           # syscall number → RAX
    mov     %rsi, %rdi           # shift args left
    mov     %rdx, %rsi
    mov     %rcx, %rdx
    mov     %r8,  %r10
    mov     %r9,  %r8
    mov     8(%rsp), %r9
.global tawcroot_raw_syscall_insn
tawcroot_raw_syscall_insn:
    syscall
.global tawcroot_raw_syscall_ret
tawcroot_raw_syscall_ret:
    ret
```

There are three related addresses, and mixing them up breaks the
whole design:

- The label sitting **on** the SYSCALL/SVC instruction
  (`tawcroot_raw_syscall_insn`) is the address of the trapping
  instruction in the binary.
- `seccomp_data.instruction_pointer` and `siginfo->si_call_addr`
  identify the **post-syscall PC** — the address of the instruction
  immediately *after* the SYSCALL/SVC. Linux populates both fields
  from `pt_regs->ip` / `pt_regs->pc`, which the syscall-entry asm
  sets to the saved-return PC (the next instruction). Confirmed
  empirically on x86_64 and aarch64 — see the smoke driver in
  `tawcroot/src/main.c`. SYSCALL is 2 bytes on x86_64, SVC is 4
  bytes on aarch64; the post-syscall PC is the *insn label + insn
  size*, which is the same address as `tawcroot_raw_syscall_ret`.
- The saved program counter in `ucontext_t` is the resume PC; for
  `SECCOMP_RET_TRAP` it equals `instruction_pointer` (resuming after
  the handler continues at the instruction after `SYSCALL`/`SVC`).

The BPF filter therefore checks
`instruction_pointer == &tawcroot_raw_syscall_ret` (NOT `_insn`) and
`RET_ALLOW`s just that one address. Anything else executing a
SYSCALL/SVC is guest code and follows the normal trap table.

(On aarch64 the equivalent uses `svc #0`; same `_ret`-label pattern.)

The stub symbols (`tawcroot_raw_syscall`,
`tawcroot_raw_syscall_insn`, `tawcroot_raw_syscall_ret`) should be
hidden (`.hidden` directive in the asm) and the asm function placed
in `.text` with no special section attributes. Under static linking
there's no PLT to worry about, but hidden visibility still ensures
the symbols can't be interposed by code the guest later mmaps (some
debug/ptrace tooling does this).

**Must-test before phase 1:** a tiny Android smoke binary installs a
filter that allows exactly `&tawcroot_raw_syscall_ret`, traps a
normal syscall from elsewhere, and records both `si_call_addr` and
the saved resume PC from `ucontext_t` for x86_64 and aarch64. If
the observed filter address is not the post-syscall PC label,
stop and fix the stub/filter contract before implementing path
translation. (Status: passing on x86_64 emulator — see
`tawcroot/tests/handler/test_foundation_smoke.c`,
`tawcroot/tests/testhost/src/smoke.c`, and the comment in `filter.c`
where the `_ret`-vs-`_insn` discrepancy is documented.) The same smoke must issue every raw syscall the handler
or `--exec-child` bootstrap plans to use (`openat`, `read`, `write`,
`close`, `mmap`, `mprotect`, `munmap`, `getcwd`, `readlinkat`,
`fstatat`/`statx`, `fcntl`, `execveat`, state-fd creation, etc.).
Our IP allowlist bypasses only our own filter; it cannot override
Android's already-stacked zygote filter. If Android `TRAP`s or
`KILL`s one of these raw syscalls from the stub, pick a different
primitive before building on it.

### Never emit a syscall newer than the oldest Android policy

A nested SIGSYS is fatal, silently. Our `SIGSYS` handler is installed
with `sa_mask = ~0`, so SIGSYS is blocked while it runs; if a raw
syscall the handler issues is itself `RET_TRAP`ped by Android's
zygote filter, the kernel cannot deliver the second SIGSYS and
force-kills the process — no log, no exit code, no core. The guest
just vanishes.

The rule that follows: **a handler may only emit syscall numbers that
every Android policy we support already allows.** Android's app
filter is generated from bionic's syscall list at the API level the
device shipped with, so "old enough for bionic at `minSdk`" is the
real test — not "old enough for the kernel". seccomp runs *before*
the kernel's unknown-syscall check, so a new kernel does not rescue a
new NR from an old policy either.

Known instances, all in `src/syscalls_fd.c`:

- `close_range` (NR 436, bionic API 34): **emulated**, never
  re-issued. `handle_close_range` opens `/proc/self/fd`,
  `getdents64`-walks it, and closes (or sets `FD_CLOEXEC` on) each
  guest fd in range, skipping the reserved table, rescanning until a
  pass closes nothing. Falls back to a linear loop bounded by
  `prlimit64(RLIMIT_NOFILE)` when `/proc` can't be opened. This was
  wmww/tawc#14: glibc's pre-exec `closefrom` killed every
  gpg/gpg-agent/dirmngr spawn on an Android 11 device, surfacing as
  pacman's "GPGME error: Invalid crypto engine".
- `dup2` → `dup3`, `poll` → `ppoll`, `select` → `pselect6`,
  `epoll_wait` → `epoll_pwait`, `pipe` → `pipe2`, and friends on
  x86_64: the legacy NR is lp32-only in bionic's allowlist, so the
  handler issues the modern sibling.
- `faccessat2` (NR 439): trapped and routed to `faccessat`.

Test coverage for the rule is the synthesized android filter
(`tawcroot/tests/handler/androidfilter/wrap.c`), which `RET_TRAP`s the
version-gated NRs — including 436 — so any handler that re-emits one
kills the testhost and fails the whole module. The prod-env layer
(`tests/integration/tests/tawcroot_prodenv.rs`) is the matching
real-policy check, but only on whatever API the target AVD or phone
runs.

### Why non-PIE

The IP-based allowlisting in the BPF filter bakes
`&tawcroot_raw_syscall_ret` as a fixed address literal into the
filter program at install time. The filter is kernel state, inherited
across `execve`, and **cannot be modified or removed** — only new
filters can be stacked on top. Seccomp filter stacking uses a
"most restrictive wins" rule: `TRAP` (0x00030000) has strictly
higher precedence than `ALLOW` (0x7fff0000). If any filter in the
stack says TRAP for a given syscall, the result is TRAP regardless
of what other filters say.

This creates a hard constraint for the re-exec architecture:

1. First tawcroot installs a filter with `IP == X → ALLOW` for
   the stub. After `execve` into the guest, this filter is
   inherited.
2. Guest execs something → handler re-execs tawcroot
   (`--exec-child`). The inherited filter is still active.
3. If tawcroot is PIE, ASLR places the stub at a new address Y.
   The inherited filter says `IP == X → ALLOW`, but the stub is
   at Y — no match — inherited filter returns TRAP. Even if
   `--exec-child` installs a new filter saying `IP == Y → ALLOW`,
   the inherited TRAP beats the new ALLOW. **The stub's own
   syscalls are TRAPped.** The handler recurses/dies.

**With non-PIE** (`-static -no-pie`), the binary is ET_EXEC. The
kernel loads it at the link-time-fixed address every time. The stub
is at the same virtual address across all re-execs. The inherited
filter's `IP == X` check matches the new process's stub. ALLOW.
No new filter needed.

Consequences:
- `--exec-child` **must NOT install a new seccomp filter.** The
  inherited filter is correct. It only reinstalls the SIGSYS
  handler (reset by `execve`) and re-opens the rootfs fd / bind
  table.
- Pick base addresses that avoid the guest's normal mapping
  region. Use `-Wl,--image-base=<addr>` (NDK lld does **not**
  support `-Ttext-segment` — it errors out and explicitly
  recommends `--image-base`). Bases:
  **0x2000000000** for aarch64 (matches proot's `LOADER_ADDRESS`),
  **0x40000000** for x86_64. The aarch64 number can be high
  because `R_AARCH64_CALL26` and friends are PC-relative and the
  binary is internally self-consistent. The x86_64 number is
  forced down to ~1 GB by static-bionic's `libc.a`: it contains
  `R_X86_64_PLT32` references to weak-undefined symbols (e.g.
  `__loader_add_thread_local_dtor` from `__cxa_thread_atexit_impl.o`,
  `__scudo_default_options` from scudo's `flags.o`) that the static
  link resolves to address 0. The PLT32 displacement from our `.text`
  to 0 must fit in a signed 32-bit immediate, so any base above ~2 GB
  fails the link with "relocation R_X86_64_PLT32 out of range." 1 GB
  comfortably clears the typical ET_EXEC default base (0x400000) and
  sits far below where Linux/glibc ld.so loads (~0x7f00_0000_0000),
  while leaving 1 GB of headroom under the 2 GB PLT32 ceiling.
- No ASLR for tawcroot itself. Acceptable — tawcroot is not a
  security boundary (§"What it explicitly is not"), and the guest
  can read `/proc/self/maps` to find our address anyway.

`PR_SET_SYSCALL_USER_DISPATCH` defines an always-allowed IP range
separately from seccomp, but Linux 5.11 introduced it on x86.
Linux 6.1 ARM64 lacks that support, so a newer kernel version alone
does not make it available on the S24. It still uses SIGSYS and its
settings do not survive fork/clone/exec. See the
[manual](https://man7.org/linux/man-pages/man2/PR_SET_SYSCALL_USER_DISPATCH.2const.html)
and [technique survey](execution-techniques.md); it is not a current
ARM64 alternative to the fixed-address BPF path.

### Threading and `vfork` invariants

The handler runs in the calling thread's context. With multiple
guest threads, multiple SIGSYS handlers can run concurrently in
their own stacks. This is safe by construction because:

- Core tawcroot globals (rootfs_fd, bind table, well-known-symlink
  memo cache, the rootfs host-path string) are written exactly twice
  in a process's lifetime: once at supervisor init, and again only by
  the `chroot(2)` handler when the guest issues a chroot. The MVP
  deliberately does **not** maintain a mutable guest-cwd string;
  `AT_FDCWD` resolution derives from the kernel's current cwd via
  raw `getcwd` + reverse translation. The fd-provenance table and
  guest-visible `SIGSYS` shadow are explicit exceptions: fixed-size,
  handler-safe, and updated only through trapped control syscalls.
- All per-call state is stack-local.
- We never call any libc function with hidden mutable state from
  the handler (no `getenv`, no `errno` read, no stdio).

The chroot path-mutation race. `chroot` is the one syscall that
re-writes core globals after init. The writes are sequenced (zero
the length, then bytes, then republish length; deactivate-then-clear
for binds; write the new fd last) so concurrent readers fall into
one of two states: they see the OLD coherent view, or they see a
"length zero" / "no binds" intermediate that every reader interprets
as "path outside the rootfs view → -ENOENT". The latter is a
transient ENOENT the guest retries past in microseconds. Without
explicit memory barriers this isn't a hard guarantee on aarch64, but
the failure mode is bounded: a concurrent reader can at worst observe
a transient ENOENT or wrong-route on a single path-bearing syscall.
For the workloads we target (pacman 6.x's chroot is single-threaded
post-startup; container runtimes that chroot from a worker thread
don't run on Android) this is acceptable. If a future workload needs
strict multi-threaded chroot safety, switch to a seqlock-snapshot
protocol (writer increments seq; readers retry on odd-seen or
mismatch).

`vfork` shares VM with the parent until `execve`. A `vfork`'d child
running our handler must not mutate ordinary globals that the parent
depends on. Any mutable process-wide state outside the chroot/init
exception (fd provenance, guest `SIGSYS` shadow, future namespace
tracking) must use an explicitly designed lock-free snapshot scheme
that is safe for concurrent signal handlers (for example, fixed-size
double-buffered state plus an atomic sequence counter), or it must be
re-derived from kernel state per call. Do not add a plain mutable C
string/global that handlers read while another thread can update it
without the bounded-failure analysis the chroot path has.

### Guest signal/seccomp control

tawcroot owns `SIGSYS`. The seccomp filter is permanent kernel state,
and the whole architecture assumes a live `SA_SIGINFO` handler for
seccomp-originated traps. A guest that installs its own `SIGSYS`
handler, blocks `SIGSYS`, resets the disposition to `SIG_DFL`, or
stacks another seccomp filter can break tawcroot even though it is
"just" making normal Linux syscalls.

Therefore these syscalls are part of the runtime surface, not optional
hardening:

- `rt_sigaction`: virtualize `SIGSYS`. Guest attempts to read or write
  the `SIGSYS` disposition see a guest-shadow value, while the real
  kernel disposition remains tawcroot's handler. Other signals pass
  through. If the guest asks for `SIG_DFL`/`SIG_IGN` on `SIGSYS`, store
  that in the shadow table but do not apply it to the kernel.
- `rt_sigprocmask` / `sigprocmask`: reserve `SIGSYS` for translation.
  Clear it from requested masks and report it unblocked on queries.
  Update the saved kernel mask in `ucontext` so sigreturn preserves changes
  to other signals. There is no per-thread mask shadow or TID table.
  Programs requiring a blockable SIGSYS are not supported; pretending it
  was blocked cannot provide that behavior.
- `sigaltstack`: native; the loader advertises the signal-stack budget.

- `seccomp(SECCOMP_SET_MODE_FILTER)` and
  `prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, ...)`: accept without
  installing a filter. Check nonempty length (maximum 4096 instructions)
  and readable storage; this is not a BPF verifier. Seccomp syscall flags
  are limited to zero or TSYNC. Other flags, including NEW_LISTENER,
  return EINVAL: no notification fd can be supplied. Other seccomp
  operations return ENOSYS. PR_GET_SECCOMP reports the host's real state.
  NULL filter capability probes report unsupported (seccomp: ENOSYS;
  prctl: EINVAL), rather than advertising BPF enforcement we do not provide.
  Applications that can degrade must not enter enforcement-dependent paths.
  Guest filters can otherwise kill the raw syscall stub or bypass pathname
  translation. Consequently this runtime does not enforce guest sandboxes;
  only the Android app boundary remains, not per-project/instance isolation.

The guest-visible `SIGSYS` shadow state is tiny but still mutable
process state. Store it in fixed-size atomics or a snapshot structure
compatible with the threading rules above; do not protect it with
malloc-backed containers or libc locks from inside the handler.

Tests must cover at least: guest `sigaction(SIGSYS, SIG_DFL)`, guest
blocking `SIGSYS`, guest `prctl(PR_SET_SECCOMP)` / `seccomp(2)` (both
must observe success on filter installation without actually installing the
filter), and a path syscall after each attempt. The expected result
is that path translation still works.

### Why the handler is async-signal-safe

Strictly speaking, an `SA_SIGINFO` handler is required to use only
async-signal-safe APIs. Our handler:

- Reads immutable static globals (the rootfs fd, the bind table) and
  fixed-size atomic/snapshot runtime tables (fd provenance, guest
  `SIGSYS` shadow) — fine, as long as those tables follow the
  threading rules above.
- Issues raw syscalls via the stub — fine, `syscall(2)` is
  async-signal-safe by definition.
- Walks a small stack-local string buffer after the path has been
  copied through the guarded guest-copy helpers.

What we **avoid**: malloc, stdio, locale, anything that takes a
lock. Path buffers come from the `path_scratch.c` static pool, not
the handler stack (see §"Handler stack budget"). The bind table is
built once at init and immutable thereafter; the few mutable runtime
tables must be lock-free and handler-safe.

Because guest glibc/ld.so is manually loaded into the same address
space as a statically linked bionic tawcroot, the handler/runtime must
also be treated as a small freestanding runtime. Compile handler,
guest-copy, path, syscall-dispatch, and arch-stub objects with flags
that prevent compiler-inserted runtime dependencies from leaking into
the hot path (`-fno-stack-protector`, no sanitizers, no profiling,
no exceptions/unwind assumptions). Do not read bionic `errno` in the
handler; raw syscalls return negative errno values directly. Any libc
helper used outside the handler but before the manual-load jump must
be explicitly audited under static bionic and inherited seccomp.

We deliberately leave `SA_NODEFER` *off*: the kernel masks SIGSYS
for the duration of the handler. With NODEFER set, a stub-syscall
that erroneously trapped (e.g. if the IP-allowlist predicate
regressed) would re-enter the handler synchronously and stack-
overflow under recursion; without it, the second SIGSYS sits
pending until the outer handler returns, at which point it fires
once and we either succeed or crash cleanly with a single nested
frame. Either knob is defensible, but "non-recursive by default"
is the safer one given our handler state is immutable-or-snapshot
only and the handler has no genuine reason to re-enter itself.

### Handler stack budget

Where the handler runs is two-tier, because SIGSYS is registered
`SA_ONSTACK`:

- **Thread has a `sigaltstack`** → the kernel puts the signal frame, and
  so our whole handler chain, there. This is what makes Go work at all:
  goroutine stacks start at 2 KiB and the runtime issues syscalls
  straight from them, but every M gets a 32 KiB altstack in
  `runtime.minit`. Without `SA_ONSTACK` each trapped syscall wrote
  ~6 KiB below a 2 KiB stack into the neighbouring goroutine stacks
  (Go packs them, so nothing faults) and the process died later inside
  the GC — `micro`, `fzf`, every Go program (GitHub issue #6).
- **No altstack** → the kernel silently falls back to the trapping
  thread's own stack, whatever size the guest's runtime chose. musl's
  default thread stack is 128 KiB; the supported floor is the 16 KiB
  pinned by the `static_small_stack_open_argv1` fixture (a clone child
  with an explicit 16 KiB stack doing a path-bearing open). We can never
  *rely* on an altstack — it is per-thread state the guest owns — so
  this floor and the frame cap below still stand.

Measured cost on the OnePlus 9 (no SVE; poison 1 MiB, clone a child
mid-region, one syscall, scan for the lowest clobbered word): a trapped
`getpid` writes ~4.7 KiB below SP, a trapped path-resolving `openat`
~6.2 KiB — about 4.5 KiB of kernel `rt_sigframe` plus ~1.4 KiB of
handler chain. The kernel half grows with SVE (and xsave on x86_64).

`sigaltstack` runs natively. The loader advertises the kernel's
`AT_MINSIGSTKSZ` plus 16 KiB of handler budget (a 16 KiB kernel-frame
allowance is used when older kernels omit that auxiliary value). Applications
must provide enough stack space; fixed undersized stacks are not substituted.
This preserves stack addresses and metadata and lets callers disable a stack
immediately after unmapping it without a SIGSYS frame landing on freed memory.
Pinned by `static_sigaltstack{,_small}_open_argv1` and
`static_sigaltstack_swap_argv1` (replace, then disable + munmap).

The budget is enforced mechanically, not by convention: every
production object compiles with `-Wframe-larger-than=1024 -Werror`
(Makefile `TAWC_CFLAGS`, build.sh `COMMON_CFLAGS`), so a handler frame
over 1 KiB is a build error. The rules, also stated in
`include/stack_budget.h`:

- Buffers sized by `PATH_MAX` or by a guest-controlled length come
  from the `path_scratch.c` pool (128 slots × 4 × 4096 B in BSS,
  lock-free CAS acquire), never the frame.
- Fixed buffers under ~256 bytes are fine on the stack.
- Recursive functions must keep per-level frames minimal — the cap
  multiplies by depth. The deepest chain is exec's shebang
  classification (`classify_loadable`, depth ≤ `TAWC_SHEBANG_MAX_DEPTH`
  = 4); before the 2026-08 fix its ~2 KiB per-level frame times five
  levels could alone approach a small guest stack.
- Larger staging serialized by a lock goes in statics — the exec path
  stages through exec_lock-guarded static buffers (`exec_handler.c`).
- Init and `--exec-child` bootstrap code that provably never runs on a
  guest stack (supervisor init, filter install, the loader below the
  jump) opts out per file/region with `TAWCROOT_FRAME_CAP_EXEMPT`.
  Never exempt anything reachable from the dispatch table.

The testhost build compiles the same production sources plus smoke
drivers that legitimately use big frames on the process's own stack, so
it passes `-Wno-frame-larger-than`; the prod objects are the gate.

### Handler coding conventions

Refactored conventions every fs-handler follows (June 2026 cleanup):

- **Single guest fetch.** A handler copies the guest path string at
  most once (`fetch_guest_path` into the scratch pool), classifies
  intercepts (`classify_shm`, `tawcroot_proc_shadow_open`,
  `tawcroot_is_proc_self_exe`) against that local copy, and then
  translates the *same bytes* via `translate_local`. Never re-read a
  guest pointer after classifying it — a racing guest thread could
  swap the string between the peek and the translate (double-fetch).
  Handlers without intercepts use `translate_at`, which is fetch +
  `translate_local` in one call.
- **`struct fs_path`** is the unit of translation output: `{fd,
  is_root, path}` — pass `fd`/`path` straight to the `*at` syscall;
  `is_root` means the path resolved to the base directory itself and
  each syscall picks its own semantics ("." for most, AT_EMPTY_PATH,
  or a direct errno).
- **Legacy x86_64 syscalls are `via_at` shims** — rebuild the register
  frame and re-enter the modern *at handler (nr is preserved for
  nr-sensitive handlers). Don't write bespoke legacy bodies.
- **Bounded string building** goes through `tawc_str_append` /
  `tawc_str_append_dec` / `tawc_str_copy` / `tawc_proc_fd_path`
  (strings.c, unit-tested) — no hand-rolled bounded copy loops.
- **Trapped-and-denied syscalls** register `tawcroot_deny_enosys` /
  `tawcroot_deny_eperm` (dispatch.c) with the rationale comment at the
  registration site.
- **/proc shadow synthesis** lives in `proc_shadow.c`; adding a shadow
  file means one classifier kind, one content synthesizer and one line
  in `tawcroot_proc_shadow_open`. The stat surface comes free: one
  classifier (`tawcroot_proc_shadow_classify`) feeds all four handlers
  (open, stat, statx, access), and the metadata answers are synthesized
  from the kind, never from the memfd — building content on every stat
  would re-read and rewrite the whole maps file for an
  `ls -l /proc/self`. **Lockstep rule:** every kind that opens must
  also stat. Handlers reach the classifier through
  `proc_shadow_classify_at` (syscalls_fs.c), which also retries via
  fd-relative composition; `hosted_proc_shadow_open_stat_lockstep`
  fails CI if a new kind lands on only one surface.

### Syscall-site rewriting (`TAWCROOT_FASTPATH`)

On by default for aarch64 (`include/fastpath_config.h`); build with
`EXTRA_CFLAGS=-DTAWCROOT_FASTPATH=0` to turn it off. Stats builds and the
in-process tests orchestrator build it off. `src/fastpath.c` skips signal
delivery for hot guest syscall sites:

- After the SIGSYS handler dispatches a trap whose nr is on a uc-free
  allowlist (identity getters, `openat`, `close`, the stat family,
  `readlinkat`, `faccessat{,2}`, `getdents64`, `getcwd`), it counts the
  site. On the 64th trap it rewrites the `svc #0` into `b stub` when the
  instruction before it is `mov {w,x}8, #nr` with the same nr. The nr at
  a patched site is then static, so `clone`, `rt_sigreturn` and the like
  can never be routed through it.
- Only sites inside r-xp file-backed mappings (per `/proc/self/maps`,
  cached) are patched; the page goes RWX for the store and back to r-x.
  Because the mapping is already executable this needs SELinux execmem,
  not execmod. Stubs live in RWX anonymous regions named
  `[anon:tawcroot-fastpath]`, within branch range of the site.
- A stub (`src/arch/aarch64_fastpath.S`) saves every register the
  kernel would preserve (x1–x30, NZCV, caller-saved SIMD), calls
  `tawcroot_fast_entry`, which runs `tawcroot_dispatch_call(args, NULL)`
  and returns to site+4.
- Signals: the SIGSYS handler's `sa_mask` blocks everything, so no
  guest handler can interrupt (or `siglongjmp` out of) a translation
  holding scratch slots or seqlock writes. The direct call blocks the
  same way, but only once the process has installed a guest handler
  (`handle_rt_sigaction` reports every non-default, non-ignore
  handler; the flag is sticky so a vfork child's reset cannot clear
  what the parent still has, and exec starts a fresh runtime). Before
  that every disposition is default or ignore and no guest code can
  run mid-call, so the two `rt_sigprocmask` calls are skipped — the
  common case for CLI tools.
- SVC and B are both architecturally safe to replace while another
  thread executes them; the stub is written and flushed first.

S24, shell domain: with the unconditional mask, `stat_many` 0.90,
`tar` 0.92, `find`/`ls -lR` 0.94, `grep -r` 0.96 against the default
build; skipping the mask until a handler exists takes a further
0.90–0.93 off stat/find/ls/tar (`tawcroot/perf/results.md`).
Spawn-heavy loops are unchanged (short processes end before sites reach
the threshold). `TAWCROOT_FASTPATH=2` never masks: the ceiling, and not
reentrancy-safe once a guest handler exists.

The threshold is 64 because every process pays a first patch (a
`/proc/self/maps` read, two `mprotect`s, a COW page, a stub region):
at 4, ld.so's library `openat` site was patched in every short-lived
process and `true` ×100 / bash startup ran 4–6% slower; at 64 they are
unchanged and long walks keep the gain.

Verified in the app (`untrusted_app_34`, the app seccomp filter): sites
patch, results match the non-rewriting build, no AVC denials, and a
bash `trap` handler still runs. `prod_rootfs_trapped_svc_preserves_registers`
checks register preservation across the rewrite (256 calls from one
site; it catches a lost NZCV; getuid's path does not touch SIMD, so it
cannot catch lost q regs).
