/* Runtime-control syscall handlers — guest-side denials and shadow
 * virtualization for operations that would otherwise compromise
 * tawcroot's own invariants.
 *
 * Surface (notes/tawcroot/sigsys-handler.md §"Guest signal/seccomp control"):
 *   - Guest seccomp filters: accept without installing another filter.
 *     This compatibility runtime provides no guest sandbox isolation.
 *   - `rt_sigaction(SIGSYS, ...)`: virtualize. The guest's intended
 *     disposition lives in a shadow buffer; reads/writes of SIGSYS
 *     hit the shadow and never the kernel. The real kernel disposition
 *     stays our SIGSYS handler.
 *   - `rt_sigprocmask`: SIGSYS is reserved and cannot be blocked.
 *     Strip it from requested masks and report the actual kernel mask.
 *     No per-thread shadow or thread-exit bookkeeping is needed.
 *   - `sigaltstack`: native; loader auxv advertises the stack budget.
 *   - Other signals are unaffected.
 */

#include <stddef.h>
#include <stdint.h>
#include <ucontext.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "io.h"
#include "path.h"
#include "mounts.h"
#include "namespace.h"
#include "path_scratch.h"

#include "dispatch.h"
#include "fastpath.h"
#include "errno_neg.h"
#include "raw_sys.h"
#include "signal_shadow.h"
#include "syscalls_control.h"
#include "sysnr.h"
#include "usercopy.h"
#include "tawc_string.h"
#include "linkstore.h"
#include <signal.h>

#ifndef PR_GET_SECCOMP
# define PR_GET_SECCOMP 21
#endif
#ifndef PR_SET_SECCOMP
# define PR_SET_SECCOMP 22
#endif

#ifndef SIGSYS
# define SIGSYS 31
#endif
/* sigset bit position is (signo - 1). */
#define SIGSYS_BIT (1ULL << (SIGSYS - 1))

#ifndef SIG_BLOCK
# define SIG_BLOCK   0
# define SIG_UNBLOCK 1
# define SIG_SETMASK 2
#endif

/* Compatibility only: never stack guest BPF over our SIGSYS routing.
 * Validate readable storage, not BPF semantics. Android's filter remains
 * active; no application filesystem/network policy is enforced here. */
static long accept_guest_filter(long pointer)
{
	struct sock_fprog program;
	if (tawc_copy_from_guest(&program, sizeof program, (void *)(uintptr_t)pointer) < 0)
		return TAWC_EFAULT;
	if (!program.len || program.len > 4096) return TAWC_EINVAL;
	struct sock_filter instruction;
	for (unsigned i = 0; i < program.len; i++)
		if (tawc_copy_from_guest(&instruction, sizeof instruction, program.filter + i) < 0)
			return TAWC_EFAULT;
	return 0;
}

static long handle_seccomp(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	if (args->a == SECCOMP_SET_MODE_FILTER) {
		/* NULL is a capability probe, not a filter installation. We do
		 * not provide BPF enforcement; do not advertise that capability. */
		if (!args->c) return TAWC_ENOSYS;
		/* NEW_LISTENER requires a real notification fd, which we cannot supply. */
		if (args->b & ~SECCOMP_FILTER_FLAG_TSYNC) return TAWC_EINVAL;
		return accept_guest_filter(args->c);
	}
	return TAWC_ENOSYS;
}

static long handle_prctl(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	if ((int)args->a == PR_SET_SECCOMP)
		return args->b == SECCOMP_MODE_FILTER && args->c ? accept_guest_filter(args->c) : TAWC_EINVAL;
	long ns = tawcroot_namespace_prctl((int)args->a, args->b);
	if (ns != TAWC_ENOSYS) return ns;
	return TAWC_RAW(TAWC_SYS_prctl, args->a, args->b, args->c,
			args->d, args->e, 0);
}

/* Android omits robust-list syscalls from the app allowlist. Keep the guest's
 * actual registration for libraries which inspect their own pthread list.
 * This is registration/query compatibility, NOT kernel owner-death recovery.
 * Native kernels retain their complete implementation. No libc TLS layout or
 * fabricated list is involved; exit(2) stays untrapped for musl stack safety. */
#define ROBUST_SLOTS 4096
static struct { uint64_t owner; uintptr_t head; } robust_lists[ROBUST_SLOTS];

static uint64_t robust_owner(void)
{
	uint64_t pid = (uint32_t)TAWC_RAW(TAWC_SYS_getpid, 0, 0, 0, 0, 0, 0);
	uint64_t tid = (uint32_t)TAWC_RAW(TAWC_SYS_gettid, 0, 0, 0, 0, 0, 0);
	return (pid << 32) | tid;
}

static long handle_set_robust_list(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	if (args->b != 3 * (long)sizeof(uintptr_t)) return TAWC_EINVAL;
	long result = TAWC_RAW(TAWC_SYS_set_robust_list, args->a, args->b, 0, 0, 0, 0);
	if (result != TAWC_ENOSYS) return result;
	uint64_t owner = robust_owner();
	/* A registration overwrites any previous registration for this thread,
	 * including a reused tid. Reap dead registrations only on exhaustion. */
	for (int pass = 0; pass < 2; pass++) {
		for (unsigned i = 0; i < ROBUST_SLOTS; i++) {
			unsigned slot = ((uint32_t)owner + i) % ROBUST_SLOTS;
			uint64_t found = __atomic_load_n(&robust_lists[slot].owner, __ATOMIC_ACQUIRE);
			if (found != owner) {
				if (pass && found && found != UINT64_MAX &&
				    TAWC_RAW(TAWC_SYS_tgkill, found >> 32, (uint32_t)found,
				             0, 0, 0, 0) == TAWC_ESRCH) {
					__atomic_compare_exchange_n(&robust_lists[slot].owner, &found,
					                            0, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
					found = __atomic_load_n(&robust_lists[slot].owner, __ATOMIC_ACQUIRE);
				}
				if (found || !__atomic_compare_exchange_n(&robust_lists[slot].owner,
				        &found, UINT64_MAX, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) continue;
			}
			__atomic_store_n(&robust_lists[slot].head, (uintptr_t)args->a, __ATOMIC_RELAXED);
			__atomic_store_n(&robust_lists[slot].owner, owner, __ATOMIC_RELEASE);
			return 0;
		}
	}
	return TAWC_ENOMEM;
}

static long handle_get_robust_list(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	long result = TAWC_RAW(TAWC_SYS_get_robust_list, args->a, args->b, args->c, 0, 0, 0);
	if (result != TAWC_ENOSYS) return result;
	uint64_t owner = robust_owner();
	if ((int)args->a && (uint32_t)args->a != (uint32_t)owner) return TAWC_EPERM;
	uintptr_t head = 0;
	size_t size = 3 * sizeof(uintptr_t);
	for (unsigned i = 0; i < ROBUST_SLOTS; i++) {
		unsigned slot = ((uint32_t)owner + i) % ROBUST_SLOTS;
		if (__atomic_load_n(&robust_lists[slot].owner, __ATOMIC_ACQUIRE) == owner) {
			head = __atomic_load_n(&robust_lists[slot].head, __ATOMIC_RELAXED);
			break;
		}
	}
	if (tawc_copy_to_guest((void *)args->c, &size, sizeof size) < 0 ||
	    tawc_copy_to_guest((void *)args->b, &head, sizeof head) < 0) return TAWC_EFAULT;
	return 0;
}

/* Init sessions: a guest SIGSEGV/SIGBUS handler is fronted by one that
 * first writes the fault (address, pc, and the /proc/self/maps lines
 * of pc and the return address) to stderr and to <store>/fault.log,
 * which survives a console helper killed along with the session.
 * It then restores the guest's action and returns, so the fault
 * re-raises into the guest's handler. systemd's crash handler alone
 * only says "Caught <SEGV>". */
static uint64_t g_fault_action[2][TAWC_KERN_SIGACTION_SIZE / sizeof(uint64_t)];
static int g_fault_wrapped[2];
static char g_maps[65536];
static char g_fault_path[4200];

static int fault_slot(int sig) { return sig == SIGSEGV ? 0 : sig == SIGBUS ? 1 : -1; }

static void put_hex(char *buf, size_t cap, size_t *n, unsigned long v)
{
	char tmp[19] = "0x";
	int len = 2;
	for (int shift = 60; shift >= 0; shift -= 4) {
		int d = (int)((v >> shift) & 15);
		if (d || len > 2 || !shift) tmp[len++] = "0123456789abcdef"[d];
	}
	tmp[len] = 0;
	(void)tawc_str_append(buf, cap, n, tmp);
}

static unsigned long parse_hex(const char **p)
{
	unsigned long v = 0;
	for (;; ++*p) {
		char c = **p;
		if (c >= '0' && c <= '9') v = v * 16 + (unsigned long)(c - '0');
		else if (c >= 'a' && c <= 'f') v = v * 16 + (unsigned long)(c - 'a' + 10);
		else return v;
	}
}

static void fault_emit(long lf, const char *p, long n)
{
	(void)TAWC_RAW(TAWC_SYS_write, 2, (long)p, n, 0, 0, 0);
	if (lf >= 0) (void)TAWC_RAW(TAWC_SYS_write, lf, (long)p, n, 0, 0, 0);
}

static void write_map_line(long lf, long len, unsigned long addr)
{
	for (long i = 0; i < len;) {
		long end = i;
		while (end < len && g_maps[end] != '\n') ++end;
		const char *p = &g_maps[i];
		unsigned long lo = parse_hex(&p);
		unsigned long hi = (*p == '-') ? (++p, parse_hex(&p)) : 0;
		if (addr >= lo && addr < hi) {
			fault_emit(lf, &g_maps[i], end - i + (end < len));
			return;
		}
		i = end + 1;
	}
}

static void fault_log(int sig, siginfo_t *info, void *ucv)
{
	ucontext_t *uc = ucv;
	int slot = fault_slot(sig);
#if defined(__aarch64__)
	unsigned long pc = uc->uc_mcontext.pc, ra = uc->uc_mcontext.regs[30];
	unsigned long sp = uc->uc_mcontext.sp;
#else
	unsigned long pc = (unsigned long)uc->uc_mcontext.gregs[REG_RIP];
	unsigned long sp = (unsigned long)uc->uc_mcontext.gregs[REG_RSP];
	unsigned long ra = sp ? *(unsigned long *)sp : 0;   /* best effort */
#endif
	char line[160];
	size_t n = 0;
	(void)tawc_str_append(line, sizeof line, &n, "tawcroot: pid ");
	(void)tawc_str_append_dec(line, sizeof line, &n, tawc_getpid());
	(void)tawc_str_append(line, sizeof line, &n, sig == SIGSEGV ? " SIGSEGV" : " SIGBUS");
	(void)tawc_str_append(line, sizeof line, &n, " code ");
	(void)tawc_str_append_dec(line, sizeof line, &n, info->si_code);
	(void)tawc_str_append(line, sizeof line, &n, " addr ");
	put_hex(line, sizeof line, &n, (unsigned long)info->si_addr);
	(void)tawc_str_append(line, sizeof line, &n, " pc ");
	put_hex(line, sizeof line, &n, pc);
	(void)tawc_str_append(line, sizeof line, &n, " ra ");
	put_hex(line, sizeof line, &n, ra);
	(void)tawc_str_append(line, sizeof line, &n, " sp ");
	put_hex(line, sizeof line, &n, sp);
	(void)tawc_str_append(line, sizeof line, &n, "\n");
	size_t k = 0;
	long lf = -1;
	if (tawcroot_store_host_path_len &&
	    !tawc_str_append(g_fault_path, sizeof g_fault_path, &k, tawcroot_store_host_path) &&
	    !tawc_str_append(g_fault_path, sizeof g_fault_path, &k, "/fault.log"))
		lf = tawc_openat(AT_FDCWD, g_fault_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	fault_emit(lf, line, (long)n);
	long fd = tawc_openat(AT_FDCWD, "/proc/self/maps", O_RDONLY | O_CLOEXEC, 0);
	if (fd >= 0) {
		long len = 0, r;
		while (len < (long)sizeof g_maps &&
		       (r = tawc_read((int)fd, g_maps + len, sizeof g_maps - (size_t)len)) > 0)
			len += r;
		tawc_close((int)fd);
		write_map_line(lf, len, pc);
		write_map_line(lf, len, ra);
	}
	if (lf >= 0) tawc_close((int)lf);
	if (slot >= 0) {
		g_fault_wrapped[slot] = 0;
		(void)TAWC_RAW(TAWC_SYS_rt_sigaction, sig, (long)g_fault_action[slot], 0, 8, 0, 0);
	}
}

/* rt_sigaction for SIGSEGV/SIGBUS in an init session; see fault_log. */
static long fault_sigaction(int slot, const uint64_t *act, void *oldact)
{
	if (oldact) {
		uint64_t cur[TAWC_KERN_SIGACTION_SIZE / sizeof(uint64_t)];
		if (g_fault_wrapped[slot]) memcpy(cur, g_fault_action[slot], sizeof cur);
		else if (TAWC_RAW(TAWC_SYS_rt_sigaction, slot ? SIGBUS : SIGSEGV, 0, (long)cur, 8, 0, 0))
			return TAWC_EINVAL;
		if (tawc_copy_to_guest(oldact, cur, sizeof cur) < 0) return TAWC_EFAULT;
	}
	if (!act) return 0;
	int sig = slot ? SIGBUS : SIGSEGV;
	memcpy(g_fault_action[slot], act, sizeof g_fault_action[slot]);
	if (act[0] <= 1 /* SIG_DFL, SIG_IGN */) {
		g_fault_wrapped[slot] = 0;
		return TAWC_RAW(TAWC_SYS_rt_sigaction, sig, (long)act, 0, 8, 0, 0);
	}
	uint64_t ours[TAWC_KERN_SIGACTION_SIZE / sizeof(uint64_t)];
	memcpy(ours, act, sizeof ours);
	ours[0] = (uint64_t)(uintptr_t)fault_log;
	ours[1] = (act[1] | SA_SIGINFO) & ~(uint64_t)SA_RESETHAND;
	g_fault_wrapped[slot] = 1;
	return TAWC_RAW(TAWC_SYS_rt_sigaction, sig, (long)ours, 0, 8, 0, 0);
}

static long handle_rt_sigaction(const tawcroot_syscall_args *args,
				ucontext_t *uc)
{
	(void)uc;
	int    sig         = (int)args->a;
	const void *act    = (const void *)(uintptr_t)args->b;
	void  *oldact      = (void *)(uintptr_t)args->c;
	size_t sigsetsize  = (size_t)args->d;

	if (sig != SIGSYS) {
		/* A handler may invoke translated syscalls, including async-signal-safe
		 * write(). Its sa_mask must not block our reserved trap signal. This
		 * is separate from rt_sigprocmask's thread-mask virtualization. */
		uint64_t action[TAWC_KERN_SIGACTION_SIZE / sizeof(uint64_t)];
		if (act && sigsetsize == 8) {
			long e = tawc_copy_from_guest(action, sizeof action, act);
			if (e < 0) return e;
			action[3] &= ~SIGSYS_BIT;
			act = action;
		}
		int slot = fault_slot(sig);
		if (slot >= 0 && sigsetsize == 8 &&
		    (tawcroot_namespace.session & TAWC_NS_INIT_SESSION))
			return fault_sigaction(slot, act ? action : 0, oldact);
		long rv = TAWC_RAW(TAWC_SYS_rt_sigaction, args->a, (long)act,
				   args->c, args->d, 0, 0);
		if (rv == 0 && act) tawcroot_fastpath_note_handler(action[0]);
		return rv;
	}

	if (sigsetsize != 8) return TAWC_EINVAL;

	/* Read the guest's new action into a stack-local buffer FIRST so
	 * we know whether the call would have succeeded before exposing
	 * stale shadow contents to a faulting oldact write. */
	unsigned char incoming[TAWC_KERN_SIGACTION_SIZE];
	int have_incoming = 0;
	if (act) {
		long e = tawc_copy_from_guest(incoming, sizeof incoming, act);
		if (e < 0) return TAWC_EFAULT;
		have_incoming = 1;
	}

	if (oldact) {
		unsigned char snap[TAWC_KERN_SIGACTION_SIZE];
		tawc_sigshadow_action_get(snap);
		long e = tawc_copy_to_guest(oldact, snap, sizeof snap);
		if (e < 0) return TAWC_EFAULT;
	}

	if (have_incoming)
		tawc_sigshadow_action_set(incoming);
	return 0;
}

/* Locate the 8-byte kernel sigset embedded in ucontext_t->uc_sigmask.
 * Bionic's <ucontext.h> exposes it as `sigset64_t` whose first member
 * is a `__bionic_sigset_t __bits` of 8 bytes; we treat it as a u64.
 * Modifying *here* is what makes the change persist across sigreturn —
 * a kernel-level rt_sigprocmask issued during a SIGSYS handler is
 * *undone* by sigreturn restoring task->blocked from this field. */
static uint64_t *uc_sigmask_word(ucontext_t *uc)
{
	return (uint64_t *)&uc->uc_sigmask;
}

/* Update the saved kernel mask so sigreturn preserves the change.
 * Copy input before output for aliasing, and roll back on output EFAULT. */
static long handle_rt_sigprocmask(const tawcroot_syscall_args *args,
				  ucontext_t *uc)
{
	int    how         = (int)args->a;
	const void *guest_set    = (const void *)(uintptr_t)args->b;
	void  *guest_oldset      = (void *)(uintptr_t)args->c;
	size_t sigsetsize  = (size_t)args->d;

	if (sigsetsize != 8) return TAWC_EINVAL;
	/* Kernel no-op semantics also ignore how when set is NULL. */
	if (!guest_set && !guest_oldset) return 0;

	uint64_t set_val = 0;
	int have_set = 0;
	if (guest_set) {
		long e = tawc_copy_from_guest(&set_val, 8, guest_set);
		if (e < 0) return TAWC_EFAULT;
		have_set = 1;
	}

	uint64_t *kmask = uc_sigmask_word(uc);
	uint64_t  cur_kmask = *kmask & ~SIGSYS_BIT;

	if (have_set) {
		uint64_t kernel_set = set_val & ~SIGSYS_BIT;

		switch (how) {
		case SIG_BLOCK:
			*kmask = cur_kmask | kernel_set;
			break;
		case SIG_UNBLOCK:
			*kmask = cur_kmask & ~kernel_set;
			break;
		case SIG_SETMASK:
			*kmask = kernel_set;
			break;
		default:
			return TAWC_EINVAL;
		}
	}

	if (guest_oldset) {
		uint64_t old = cur_kmask;
		long e = tawc_copy_to_guest(guest_oldset, &old, 8);
		if (e < 0) {
			/* A failed output copy must not change the mask. */
			if (have_set) *kmask = cur_kmask;
			return TAWC_EFAULT;
		}
	}
	return 0;
}

/* Syscalls that swap in a caller-supplied mask while they wait. A guest
 * signal handler that runs during the wait inherits that mask, and its
 * first trapped syscall (at least its rt_sigreturn) would then hit a
 * blocked SIGSYS, which the kernel turns into a kill. JavaScriptCore's
 * thread suspension sigsuspend()s with everything but its own signal
 * blocked. Re-issue with SIGSYS removed; a signal during the wait runs
 * its guest handler on top of this one, as the wait was not ours. */
static long strip_mask(long *guest, uint64_t *buf)
{
	if (!*guest) return 0;
	if (tawc_copy_from_guest(buf, 8, (const void *)(uintptr_t)*guest) < 0)
		return TAWC_EFAULT;
	*buf &= ~SIGSYS_BIT;
	*guest = (long)buf;
	return 0;
}

static long handle_rt_sigsuspend(const tawcroot_syscall_args *args,
				 ucontext_t *uc)
{
	(void)uc;
	uint64_t m;
	long set = args->a;
	if (args->b != 8) return TAWC_EINVAL;
	long e = strip_mask(&set, &m);
	if (e) return e;
	return TAWC_RAW(TAWC_SYS_rt_sigsuspend, set, 8, 0, 0, 0, 0);
}

/* ppoll(fds, nfds, tsp, mask, size). The filter traps these four only
 * with a mask (pselect6: with its mask/size pair). */
static long handle_ppoll(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	uint64_t m;
	long set = args->d;
	long e = strip_mask(&set, &m);
	if (e) return e;
	return TAWC_RAW(TAWC_SYS_ppoll, args->a, args->b, args->c, set, args->e, 0);
}

/* epoll_pwait{,2}(epfd, events, max, timeout, mask, size). */
static long handle_epoll_pwait(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	uint64_t m;
	long set = args->e;
	long e = strip_mask(&set, &m);
	if (e) return e;
	return TAWC_RAW(args->nr, args->a, args->b, args->c, args->d, set, args->f);
}

/* pselect6's sixth argument is { const sigset_t *mask; size_t size }. */
static long handle_pselect6(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	struct { long mask; long size; } data;
	uint64_t m;
	if (!args->f)
		return TAWC_RAW(TAWC_SYS_pselect6, args->a, args->b, args->c,
				args->d, args->e, 0);
	if (tawc_copy_from_guest(&data, sizeof data, (const void *)(uintptr_t)args->f) < 0)
		return TAWC_EFAULT;
	long e = strip_mask(&data.mask, &m);
	if (e) return e;
	return TAWC_RAW(TAWC_SYS_pselect6, args->a, args->b, args->c, args->d,
			args->e, (long)&data);
}

#if defined(__x86_64__)
/* x86_64 glibc's getpgrp(3) issues the legacy getpgrp syscall, which
 * Android's untrusted_app filter RET_TRAPs (bionic only ever calls
 * getpgid). Without this handler the -ENOSYS fallthrough reaches
 * bash's job-control init as a garbage process group and interactive
 * shells on a pty print "initialize_job_control: no job control in
 * background" and run with job control off. aarch64 never allocated a
 * getpgrp number — glibc wraps getpgid(0) there — so this is
 * emulator-only. */
static long handle_getpgrp(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)args;
	(void)uc;
	return TAWC_RAW(TAWC_SYS_getpgid, 0, 0, 0, 0, 0, 0);
}

/* time(tloc) → clock_gettime(CLOCK_REALTIME). Legacy time(2) is
 * RET_TRAPped by the real emulator filter (empirical audit:
 * notes/tawcroot/status.md); clock_gettime is allowlisted. Return
 * tv_sec, and mirror it into *tloc when non-NULL (via the guarded
 * copy — tloc is an untrusted guest pointer). */
static long handle_time(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	struct { long tv_sec; long tv_nsec; } ts;
	long r = TAWC_RAW(TAWC_SYS_clock_gettime, 0 /*CLOCK_REALTIME*/,
	                  (long)&ts, 0, 0, 0, 0);
	if (r < 0) return r;
	if (args->a != 0) {
		long cr = tawc_copy_to_guest((void *)args->a, &ts.tv_sec,
		                             sizeof ts.tv_sec);
		if (cr < 0) return cr;
	}
	return ts.tv_sec;
}

/* alarm(seconds) → setitimer(ITIMER_REAL). glibc's alarm(3) already
 * routes through setitimer so this only fires for programs issuing the
 * raw legacy NR, but the real filter RET_TRAPs it, so without this it
 * -ENOSYSes. Contract: arm a one-shot ITIMER_REAL for `seconds` (0
 * disarms) and return the whole seconds left on the previous timer,
 * rounding a partial second up as the man page specifies. */
static long handle_alarm(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	struct itv { long sec; long usec; };
	/* seconds is unsigned int in the kernel ABI — only the low 32
	 * register bits are meaningful. */
	struct { struct itv interval; struct itv value; } nv = {
		{ 0, 0 }, { (long)(unsigned int)args->a, 0 },
	}, ov;
	long r = TAWC_RAW(TAWC_SYS_setitimer, 0 /*ITIMER_REAL*/,
	                  (long)&nv, (long)&ov, 0, 0, 0);
	if (r < 0) return r;
	long rem = ov.value.sec;
	if (ov.value.usec != 0) rem++;
	return rem;
}
#endif

/* Guest-absolute name of the directory guest `path` resolves to (magic
 * links such as /proc/self/fd/N included), into `out` (4096 bytes). */
static long guest_dir(const char *path, char *out)
{
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *name = scratch->buf[0], *suffix = scratch->buf[1];
	long e = tawc_copy_string_from_guest(name, 4096, path);
	if (e < 0) return e;
	tawcroot_path_result r = tawcroot_path_translate(name, suffix, 4096,
		TAWCROOT_PATH_FOLLOW, TAWCROOT_PATH_INTENT_READ);
	if (r.err) return r.err;
	long fd = tawc_openat(r.base_fd, *suffix ? suffix : ".",
			      O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
	if (fd < 0) return fd;
	e = tawcroot_fd_to_guest_abs((int)fd, out, 4096);
	tawc_close((int)fd);
	return e < 0 ? e : 0;
}

/* API filesystems an init mounts at boot and the runtime already
 * provides (as binds or real directories). */
static long handle_mount(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	/* A bounded path-view operation, not a kernel mount or security boundary. */
	unsigned long flags = (unsigned long)args->d;
	if (!(tawcroot_namespace.session & TAWC_NS_INIT_SESSION))
		return flags == MS_BIND ? tawcroot_path_bind_guest((const char *)args->a,
			(const char *)args->b) : TAWC_EPERM;
	/* An --init session boots a real init: tmpfs and bind mounts go
	 * through the session's mount table (mounts.c); propagation
	 * changes are accepted and ignored. */
	if (!(flags & ~(MS_SHARED | MS_PRIVATE | MS_SLAVE | MS_UNBINDABLE | MS_REC)) &&
	    (flags & (MS_SHARED | MS_PRIVATE | MS_SLAVE | MS_UNBINDABLE)))
		return 0;
	return tawcroot_mounts_mount((const char *)args->a, (const char *)args->b,
		(const char *)args->c, flags, (const char *)args->e);
}

static long handle_umount(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	if (!(tawcroot_namespace.session & TAWC_NS_INIT_SESSION)) return TAWC_EPERM;
	long r = tawcroot_mounts_umount((const char *)args->a, (int)args->b);
	if (r != TAWC_ENOSYS) return r;
	/* Not an emulated mount: nothing else is a mount point. Success
	 * here would spin "umount until EINVAL" loops forever. */
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	long e = guest_dir((const char *)args->a, scratch->buf[0]);
	return e == TAWC_ENOENT ? e : TAWC_EINVAL;
}

void tawcroot_control_register(void)
{
	tawcroot_dispatch_install(TAWC_SYS_set_robust_list, handle_set_robust_list);
	tawcroot_dispatch_install(TAWC_SYS_get_robust_list, handle_get_robust_list);
	/* Some Android 4.19 kernels backport pidfd_open but not P_PIDFD
	 * waitid. GLib then selects pidfds and loses child exit status.
	 * Probe an invalid fd: supported kernels return EBADF, incomplete
	 * kernels EINVAL. Not -1: kernels reject a negative id with EINVAL.
	 * Do not emulate waits using a racy fd-to-PID lookup.
	 * On incomplete kernels expose pidfd_open as unavailable so callers
	 * use their ordinary SIGCHLD/waitpid path. New kernels stay native. */
	if (TAWC_RAW(TAWC_SYS_waitid, 3 /* P_PIDFD */, 0x7fffffff, 0,
	             5 /* WEXITED | WNOHANG */, 0, 0) == TAWC_EINVAL)
		tawcroot_dispatch_install(TAWC_SYS_pidfd_open, tawcroot_deny_enosys);
	/* The new mount API and statmount/listmount would act on host
	 * paths and mounts; ENOSYS sends callers (systemd, libmount) back
	 * to mount(2) and /proc/self/mountinfo, which we emulate. Init
	 * sessions emulate fsopen..move_mount for tmpfs (credentials). */
	static const int new_mount_api[] = { 428, 433, 442, 457, 458 };
	for (size_t i = 0; i < sizeof new_mount_api / sizeof new_mount_api[0]; i++)
		tawcroot_dispatch_install(new_mount_api[i], tawcroot_deny_enosys);
	static const int emulated_mount_api[] = { 429, 430, 431, 432 };
	for (size_t i = 0; i < sizeof emulated_mount_api / sizeof emulated_mount_api[0]; i++)
		tawcroot_dispatch_install(emulated_mount_api[i], tawcroot_mounts_new_api);
	tawcroot_dispatch_install(TAWC_SYS_seccomp,         handle_seccomp);
	tawcroot_dispatch_install(TAWC_SYS_prctl,           handle_prctl);
	tawcroot_dispatch_install(TAWC_SYS_rt_sigaction,    handle_rt_sigaction);
	tawcroot_dispatch_install(TAWC_SYS_rt_sigprocmask,  handle_rt_sigprocmask);
	tawcroot_dispatch_install(TAWC_SYS_rt_sigsuspend,   handle_rt_sigsuspend);
	tawcroot_dispatch_install(TAWC_SYS_ppoll,           handle_ppoll);
	tawcroot_dispatch_install(TAWC_SYS_pselect6,        handle_pselect6);
	tawcroot_dispatch_install(TAWC_SYS_epoll_pwait,     handle_epoll_pwait);
	tawcroot_dispatch_install(TAWC_SYS_epoll_pwait2,    handle_epoll_pwait);
	/* Keep sigaltstack native: callers may unmap their stack before
	 * disabling it. Trapping that disable would use the unmapped stack. */
	/* io_uring_setup: deny with -ENOSYS so guest libraries fall back to
	 * syscall-based I/O which we can translate. The plan
	 * (notes/tawcroot/path-translation.md "Open questions" #1) classifies a passed-through
	 * io_uring as a *correctness* hazard, not just a missing feature: the
	 * kernel reads SQEs from app-shared memory, sees host-relative paths,
	 * and silently opens host files — bypassing every translation rule.
	 * Programs that probe with -ENOSYS fall back to non-uring paths
	 * cleanly. (Review finding D4.)
	 *
	 * io_uring_register and io_uring_enter trap with the same -ENOSYS for
	 * defense-in-depth: with io_uring_setup denied the guest can't create
	 * a ring fd, but a ring fd inherited from a non-tawcroot parent across
	 * exec would otherwise sail past us untranslated. Trapping the post-
	 * setup syscalls makes "no io_uring traffic ever escapes" enforceable
	 * independently of the stacked Android filter. See notes/tawcroot/path-translation.md
	 * "io_uring MVP behavior". */
	tawcroot_dispatch_install(TAWC_SYS_io_uring_setup,    tawcroot_deny_enosys);
	tawcroot_dispatch_install(TAWC_SYS_io_uring_enter,    tawcroot_deny_enosys);
	tawcroot_dispatch_install(TAWC_SYS_io_uring_register, tawcroot_deny_enosys);

	/* clone3: deny with -ENOSYS so glibc's __clone falls back to the
	 * older clone(2) syscall (NR 220 aarch64 / NR 56 x86_64). All stacked
	 * seccomp filters are evaluated at syscall entry and the most
	 * restrictive action wins, so this can't shield the guest from an
	 * Android RET_KILL on clone3 — it works only because Android's policy
	 * is empirically not KILL: on Android 14 our trap fires ([sigsys]
	 * nr=435), i.e. Android allows-or-traps clone3. Returning -ENOSYS
	 * causes glibc to set its "clone3 missing" flag and use clone()
	 * going forward, keeping the guest off the risky syscall entirely. */
	tawcroot_dispatch_install(TAWC_SYS_clone3,          tawcroot_deny_enosys);

	/* Kernel keyrings: Android SELinux denies apps KEYCTL_LINK but not
	 * JOIN, which systemd treats as fatal (status=237/KEYRING). ENOSYS
	 * makes systemd and other callers skip keyrings entirely. */
	tawcroot_dispatch_install(TAWC_SYS_add_key,         tawcroot_deny_enosys);
	tawcroot_dispatch_install(TAWC_SYS_request_key,     tawcroot_deny_enosys);
	tawcroot_dispatch_install(TAWC_SYS_keyctl,          tawcroot_deny_enosys);

	/* Never trap exit(2): musl unmaps a dying thread's stack before the
	 * syscall. SIGSYS delivery then has no stack and kills the process. */

#if defined(__x86_64__)
	tawcroot_dispatch_install(TAWC_SYS_getpgrp,         handle_getpgrp);
	tawcroot_dispatch_install(TAWC_SYS_time,            handle_time);
	tawcroot_dispatch_install(TAWC_SYS_alarm,           handle_alarm);
#endif

	/* Defense-in-depth denials. Trapped so the guest can't mutate kernel
	 * state our path-translation layer assumes is fixed: pivot_root would
	 * desync our root-relative bookkeeping (we don't model mounts, so the
	 * "pivot the rootfs onto a sibling mount" semantics have nothing to
	 * pivot to); kernel mount/umount2 would tear down our setup binds (/dev/shm,
	 * /proc, libhybris stage); unshare/setns would hand the guest a
	 * namespace where our fd-relative /proc walks no longer name what we
	 * think they name. Lying with -EPERM is the same posture proot takes.
	 *
	 * chroot is NOT in this list — it has its own handler in chroot.c
	 * that swaps the root-view bookkeeping. mount only accepts the bounded
	 * process-local bind operation; real mounts stay denied, except that an
	 * --init session's mount/umount2 succeed as no-ops (handle_mount). */
	tawcroot_dispatch_install(TAWC_SYS_pivot_root,      tawcroot_deny_eperm);
	tawcroot_dispatch_install(TAWC_SYS_mount,           handle_mount);
	tawcroot_dispatch_install(TAWC_SYS_umount2,         handle_umount);
	tawcroot_dispatch_install(TAWC_SYS_setns,           tawcroot_deny_eperm);
}
