/* Pure cBPF builder. See filter_build.h for the API contract.
 *
 * Lives in PROD_C_FOR_TESTS so the cleat orchestrator can build the
 * program in its own address space (without touching seccomp(2)) and
 * walk it through a tiny BPF interpreter for table-driven testing.
 *
 * This file MUST stay free of `raw_sys.h`, `fdtab.h`, or any header
 * that pulls in our process-globals — otherwise the hosted glibc test
 * build clashes with `_start`/raw-syscall expectations. The dispatch-
 * table source-of-truth is upstream of this builder; trap_nrs comes
 * in as a plain array.
 */

#include <stddef.h>
#include <stdint.h>

#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/sched.h>

#include "errno_neg.h"
#include "filter_build.h"
#include "sysnr.h"

/* sock_filter constructors. Plain brace initializers (not compound
 * literals with an explicit cast — clang rejects those in array-
 * initializer position with `-Werror -Wmissing-braces`).
 *
 * sock_filter layout: { __u16 code, __u8 jt, __u8 jf, __u32 k }.
 *
 * <linux/filter.h> reuses `BPF_A` as an accumulator-source flag,
 * hence the TAWC_ prefix on these. */
#define TAWC_BPF_S(_code, _k)            { (_code), 0, 0, (_k) }
#define TAWC_BPF_J(_code, _k, _jt, _jf)  { (_code), (_jt), (_jf), (_k) }

static uint32_t stub_lo_of(uint64_t a) { return (uint32_t)(a & 0xffffffffu); }
static uint32_t stub_hi_of(uint64_t a) { return (uint32_t)(a >> 32); }

/* Leaf actions of the NR search tree. */
enum leaf_kind { LEAF_PLAIN, LEAF_CLOSE, LEAF_CLONE, LEAF_SIGMASK };

struct build_ctx {
	struct sock_filter *prog;
	size_t cap;
	size_t i;
	const int *nrs;          /* sorted, unique */
	int close_floor;
	size_t ipcheck_at;       /* filled in after the tree is emitted */
	size_t n_ja;             /* JA-to-ipcheck sites to patch */
	size_t ja_at[2048];
};

/* Search-tree leaves holding at most this many NRs are a JEQ chain. */
#define LEAF_RUN 4

static enum leaf_kind leaf_kind(const struct build_ctx *c, int nr)
{
	if (nr == TAWC_SYS_clone) return LEAF_CLONE;
	if (nr == TAWC_SYS_close && c->close_floor > 0) return LEAF_CLOSE;
	if (nr == TAWC_SYS_rt_sigprocmask) return LEAF_SIGMASK;
	return LEAF_PLAIN;
}

/* Instructions an NR's action takes, between its JEQ and the next. */
static size_t action_len(enum leaf_kind k)
{
	switch (k) {
	case LEAF_CLOSE: return 4;  /* LD a0, JGE, JA ipcheck, RET ALLOW */
	case LEAF_CLONE: return 4;  /* LD a0, JSET, RET ALLOW, JA ipcheck */
	case LEAF_SIGMASK: return 8;
	default:         return 1;  /* JA ipcheck */
	}
}

static size_t tree_len(const struct build_ctx *c, size_t lo, size_t hi)
{
	if (hi - lo <= LEAF_RUN) {
		size_t n = 1;  /* trailing RET ALLOW */
		for (size_t k = lo; k < hi; k++)
			n += 1 + action_len(leaf_kind(c, c->nrs[k]));
		return n;
	}
	size_t mid = lo + (hi - lo) / 2;
	return 2 + tree_len(c, lo, mid) + tree_len(c, mid, hi);
}

static int emit(struct build_ctx *c, struct sock_filter ins)
{
	if (c->i >= c->cap) return 0;
	c->prog[c->i++] = ins;
	return 1;
}

static int emit_ja_ipcheck(struct build_ctx *c)
{
	if (c->n_ja >= sizeof c->ja_at / sizeof c->ja_at[0]) return 0;
	c->ja_at[c->n_ja++] = c->i;
	return emit(c, (struct sock_filter)TAWC_BPF_S(BPF_JMP | BPF_JA, 0));
}

/* Emit the subtree for nrs[lo, hi). A holds the syscall NR on entry
 * and on every path out of an action that falls through. */
static int emit_tree(struct build_ctx *c, size_t lo, size_t hi)
{
	if (hi - lo <= LEAF_RUN) {
		for (size_t k = lo; k < hi; k++) {
			enum leaf_kind kind = leaf_kind(c, c->nrs[k]);
			if (!emit(c, (struct sock_filter)TAWC_BPF_J(
					BPF_JMP | BPF_JEQ | BPF_K,
					(uint32_t)c->nrs[k], 0,
					(uint8_t)action_len(kind))))
				return 0;
			switch (kind) {
			case LEAF_CLOSE:
				/* TRAP-candidate only for fd >= floor. */
				if (!emit(c, (struct sock_filter)TAWC_BPF_S(BPF_LD | BPF_W | BPF_ABS, 16)) ||
				    !emit(c, (struct sock_filter)TAWC_BPF_J(BPF_JMP | BPF_JGE | BPF_K,
						(uint32_t)c->close_floor, 0, 1)) ||
				    !emit_ja_ipcheck(c) ||
				    !emit(c, (struct sock_filter)TAWC_BPF_S(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)))
					return 0;
				break;
			case LEAF_CLONE:
				/* Never route shared-VM thread stack switches
				 * through a C handler. */
				if (!emit(c, (struct sock_filter)TAWC_BPF_S(BPF_LD | BPF_W | BPF_ABS, 16)) ||
				    !emit(c, (struct sock_filter)TAWC_BPF_J(BPF_JMP | BPF_JSET | BPF_K,
						CLONE_VM, 0, 1)) ||
				    !emit(c, (struct sock_filter)TAWC_BPF_S(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)) ||
				    !emit_ja_ipcheck(c))
					return 0;
				break;
			case LEAF_SIGMASK:
				/* Only a call that can ADD to the mask could
				 * block SIGSYS: SIG_UNBLOCK and set == NULL
				 * (query) run natively. */
				if (!emit(c, (struct sock_filter)TAWC_BPF_S(BPF_LD | BPF_W | BPF_ABS, 16)) ||
				    !emit(c, (struct sock_filter)TAWC_BPF_J(BPF_JMP | BPF_JEQ | BPF_K,
						1 /*SIG_UNBLOCK*/, 5, 0)) ||
				    !emit(c, (struct sock_filter)TAWC_BPF_S(BPF_LD | BPF_W | BPF_ABS, 24)) ||
				    !emit(c, (struct sock_filter)TAWC_BPF_J(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 2)) ||
				    !emit(c, (struct sock_filter)TAWC_BPF_S(BPF_LD | BPF_W | BPF_ABS, 28)) ||
				    !emit(c, (struct sock_filter)TAWC_BPF_J(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0)) ||
				    !emit_ja_ipcheck(c) ||
				    !emit(c, (struct sock_filter)TAWC_BPF_S(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)))
					return 0;
				break;
			default:
				if (!emit_ja_ipcheck(c)) return 0;
				break;
			}
		}
		return emit(c, (struct sock_filter)TAWC_BPF_S(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
	}

	/* A >= nrs[mid] → right half (jt skips the JA); else JA left. JA
	 * carries a 32-bit offset, so no subtree size limits the tree. */
	size_t mid = lo + (hi - lo) / 2;
	size_t right = tree_len(c, mid, hi);
	if (!emit(c, (struct sock_filter)TAWC_BPF_J(BPF_JMP | BPF_JGE | BPF_K,
			(uint32_t)c->nrs[mid], 1, 0)) ||
	    !emit(c, (struct sock_filter)TAWC_BPF_S(BPF_JMP | BPF_JA, (uint32_t)right)))
		return 0;
	return emit_tree(c, mid, hi) && emit_tree(c, lo, mid);
}

/* Sorted, de-duplicated trap set, plus rt_sigreturn. Init-only and
 * single-threaded, so static storage is fine. */
static int g_sorted[2048];

long tawcroot_build_filter(struct sock_filter *prog, size_t prog_cap,
			   const int *trap_nrs, size_t n_traps,
			   uint64_t stub_ret_addr,
			   int reserved_fd_floor,
			   uint32_t audit_arch)
{
	if (!prog || prog_cap == 0) return TAWC_EINVAL;
	if (n_traps > 1900) return TAWC_E2BIG;  /* kernel cap is 4096 */

	/* Guest signal handlers can edit ucontext directly, bypassing
	 * sigprocmask, so rt_sigreturn always traps (our own returns use
	 * the allowlisted raw stub and do not). */
	size_t n = 0;
	for (size_t t = 0; t <= n_traps; t++) {
		int nr = t < n_traps ? trap_nrs[t] : TAWC_SYS_rt_sigreturn;
		size_t k = n;
		while (k > 0 && g_sorted[k - 1] > nr) k--;
		if (k > 0 && g_sorted[k - 1] == nr) continue;
		for (size_t m = n; m > k; m--) g_sorted[m] = g_sorted[m - 1];
		g_sorted[k] = nr;
		n++;
	}

	/* Static: the JA patch table is too big for an init stack frame
	 * under -Wframe-larger-than, and the builder never runs
	 * concurrently. */
	static struct build_ctx c;
	c.prog = prog;
	c.cap = prog_cap;
	c.i = 0;
	c.nrs = g_sorted;
	c.close_floor = reserved_fd_floor;
	c.n_ja = 0;

	/* Shape (NR first, so the kernel's seccomp action cache can mark
	 * every untrapped NR constant-ALLOW and skip the filter for it):
	 *
	 *   arch != ours            → KILL_PROCESS
	 *   nr search tree:
	 *     not in trap set       → ALLOW
	 *     close, fd < floor     → ALLOW   (closefrom fast path)
	 *     clone with CLONE_VM   → ALLOW   (threads, vfork)
 *     rt_sigprocmask that can't add to the mask
 *                           → ALLOW   (SIG_UNBLOCK, set == NULL)
	 *     otherwise             → ipcheck
	 *   ipcheck: ip == stub     → ALLOW   (tawcroot's own syscalls)
	 *            else           → TRAP
	 *
	 * `close` uses a range compare: TRAP only when args[0] >=
	 * reserved_fd_floor. The range is a deliberate superset of the
	 * reserved table — the handler makes the exact membership decision
	 * and forwards the real close for anything else — so fds reserved
	 * AFTER filter install (shm_open, post-chroot root fd) are
	 * protected too. args[0] is compared on its low 32 bits only (a
	 * negative fd's low word is >= floor → traps → kernel EBADF).
	 *
	 * cBPF is 32-bit, so the 64-bit stub address is compared in two
	 * halves; on aarch64 with base 0x2000000000 the high half is
	 * non-zero and a low-only compare would mis-ALLOW guest code. */
#define EMIT_OR_FAIL(_ins) do {                                            \
		if (!emit(&c, (struct sock_filter)_ins)) return TAWC_E2BIG;  \
	} while (0)

	EMIT_OR_FAIL(TAWC_BPF_S(BPF_LD | BPF_W | BPF_ABS, 4));
	EMIT_OR_FAIL(TAWC_BPF_J(BPF_JMP | BPF_JEQ | BPF_K, audit_arch, 1, 0));
	EMIT_OR_FAIL(TAWC_BPF_S(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
	EMIT_OR_FAIL(TAWC_BPF_S(BPF_LD | BPF_W | BPF_ABS, 0));
	if (!emit_tree(&c, 0, n)) return TAWC_E2BIG;

	c.ipcheck_at = c.i;
	EMIT_OR_FAIL(TAWC_BPF_S(BPF_LD | BPF_W | BPF_ABS, 8));
	EMIT_OR_FAIL(TAWC_BPF_J(BPF_JMP | BPF_JEQ | BPF_K, stub_lo_of(stub_ret_addr), 0, 3));
	EMIT_OR_FAIL(TAWC_BPF_S(BPF_LD | BPF_W | BPF_ABS, 12));
	EMIT_OR_FAIL(TAWC_BPF_J(BPF_JMP | BPF_JEQ | BPF_K, stub_hi_of(stub_ret_addr), 0, 1));
	EMIT_OR_FAIL(TAWC_BPF_S(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
	EMIT_OR_FAIL(TAWC_BPF_S(BPF_RET | BPF_K, SECCOMP_RET_TRAP));

#undef EMIT_OR_FAIL
	for (size_t k = 0; k < c.n_ja; k++)
		prog[c.ja_at[k]].k = (uint32_t)(c.ipcheck_at - c.ja_at[k] - 1);
	return (long)c.i;
}
