/* Unit tests for the cBPF program builder
 * (tawcroot/src/filter_build.c).
 *
 * Approach: build the filter for a controlled trap set and stub
 * address, walk it with a tiny in-test BPF interpreter, and assert
 * the action for a battery of `(nr, ip, args[0], arch)` inputs.
 *
 * The cBPF dialect we generate is a strict subset:
 *
 *   BPF_LD | BPF_W | BPF_ABS  (k = byte offset into seccomp_data)
 *   BPF_JMP | BPF_JEQ/JGE/JSET | BPF_K (jt / jf are pc-relative skips)
 *   BPF_JMP | BPF_JA          (k = pc-relative skip)
 *   BPF_RET | BPF_K           (return action verbatim)
 *
 * The interpreter only knows these. Anything else returns a sentinel
 * value `BPF_ERR_UNKNOWN_OP` so a builder regression that emits a
 * different opcode lights up loudly.
 *
 * What this catches that end-to-end smoke doesn't:
 *   - off-by-one in the per-trap JEQ skip count (would mis-route the
 *     next trap entry to ALLOW or skip it entirely)
 *   - wrong arch check (KILL_PROCESS for the right arch)
 *   - IP allowlist truncation (missing high half → wrong-arch stub
 *     address sneaking through ALLOW)
 *   - close fast-path errors: wrong jf skip, wrong range-compare
 *     boundary, mis-ordered RET ALLOW vs RET TRAP slots
 *   - reload-of-nr after the close block (subsequent trap_nrs would
 *     compare against args[0] instead of nr)
 */

#include <cleat/test.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/sched.h>

#include "filter_build.h"
#include "sysnr.h"

/* Mirror the kernel's seccomp_data layout. We don't include
 * <linux/seccomp.h>'s `seccomp_data` directly so the test file is
 * self-documenting about which offsets matter. */
struct test_seccomp_data {
	int32_t  nr;
	uint32_t arch;
	uint64_t instruction_pointer;
	uint64_t args[6];
};

#define BPF_ERR_UNKNOWN_OP   0xfffffffeu
#define BPF_ERR_RAN_OFF_END  0xffffffffu

/* Bit k/4 set for every seccomp_data word the last run_filter loaded. */
static uint32_t g_loaded;

/* Tiny cBPF interpreter. Returns the program's RET k value, or one of
 * the sentinels above on error. */
static uint32_t run_filter(const struct sock_filter *prog, size_t len,
			   const struct test_seccomp_data *d)
{
	g_loaded = 0;
	uint32_t A = 0;
	size_t pc = 0;
	const uint8_t *bytes = (const uint8_t *)d;
	while (pc < len) {
		const struct sock_filter ins = prog[pc];
		switch (ins.code) {
		case BPF_LD | BPF_W | BPF_ABS: {
			/* Bounds-check the load — k must address a 32-bit
			 * slice within seccomp_data (64 bytes). */
			if (ins.k + 4 > sizeof *d) return BPF_ERR_UNKNOWN_OP;
			uint32_t v;
			memcpy(&v, bytes + ins.k, 4);
			A = v;
			g_loaded |= 1u << (ins.k / 4);
			pc++;
			break;
		}
		case BPF_JMP | BPF_JEQ | BPF_K: {
			pc += 1 + (size_t)(A == ins.k ? ins.jt : ins.jf);
			break;
		}
		case BPF_JMP | BPF_JGE | BPF_K: {
			pc += 1 + (size_t)(A >= ins.k ? ins.jt : ins.jf);
			break;
		}
		case BPF_JMP | BPF_JSET | BPF_K: {
			pc += 1 + (size_t)((A & ins.k) ? ins.jt : ins.jf);
			break;
		}
		case BPF_JMP | BPF_JA:
			pc += 1 + (size_t)ins.k;
			break;
		case BPF_RET | BPF_K:
			return ins.k;
		default:
			return BPF_ERR_UNKNOWN_OP;
		}
	}
	return BPF_ERR_RAN_OFF_END;
}

/* Convenience: build a filter and run it against one input. */
static uint32_t build_and_run(const int *trap_nrs, size_t n_traps,
			      uint64_t stub_addr,
			      int reserved_fd_floor,
			      uint32_t audit_arch,
			      const struct test_seccomp_data *d)
{
	struct sock_filter prog[4096];
	long n = tawcroot_build_filter(prog, sizeof prog / sizeof prog[0],
				       trap_nrs, n_traps,
				       stub_addr,
				       reserved_fd_floor,
				       audit_arch);
	if (n < 0) return BPF_ERR_UNKNOWN_OP;
	return run_filter(prog, (size_t)n, d);
}

/* Use the build-target arch's audit constant for inputs that should
 * pass the arch gate. */
#if defined(__x86_64__)
#  define TEST_AUDIT_ARCH AUDIT_ARCH_X86_64
#  define WRONG_AUDIT_ARCH AUDIT_ARCH_AARCH64
#elif defined(__aarch64__)
#  define TEST_AUDIT_ARCH AUDIT_ARCH_AARCH64
#  define WRONG_AUDIT_ARCH AUDIT_ARCH_X86_64
#else
#  error "unsupported test arch"
#endif

#define STUB_ADDR  0x40006789abcdef00ULL
#define OUT_OF_STUB 0x4000000000001000ULL

/* ---------------- prologue: arch + IP allowlist ---------------- */

test(filter_guest_sigreturn_traps_but_runtime_return_does_not)
{
	struct test_seccomp_data d = {
		.nr = TAWC_SYS_rt_sigreturn, .arch = TEST_AUDIT_ARCH,
		.instruction_pointer = OUT_OF_STUB,
	};
	test_int_eq(build_and_run(NULL, 0, STUB_ADDR, 0, TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_TRAP);
	d.instruction_pointer = STUB_ADDR;
	test_int_eq(build_and_run(NULL, 0, STUB_ADDR, 0, TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_ALLOW);
}

test(filter_wrong_arch_returns_kill_process)
{
	int traps[] = { 1 };
	struct test_seccomp_data d = {
		.nr = 99, .arch = WRONG_AUDIT_ARCH,
		.instruction_pointer = OUT_OF_STUB,
	};
	test_int_eq(build_and_run(traps, 1, STUB_ADDR, 0, TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_KILL_PROCESS);
}

test(filter_stub_ip_allows_any_trapped_nr)
{
	/* Even a syscall in the trap set is ALLOWed when issued from
	 * the stub. This is the IP-allowlist contract that the SIGSYS
	 * handler depends on to issue host syscalls without re-entering
	 * itself. */
	int traps[] = { 99 };
	struct test_seccomp_data d = {
		.nr = 99, .arch = TEST_AUDIT_ARCH,
		.instruction_pointer = STUB_ADDR,
	};
	test_int_eq(build_and_run(traps, 1, STUB_ADDR, 0, TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_ALLOW);
}

test(filter_allowlist_compares_full_64bit)
{
	/* IP differs in the HIGH 32 bits only — must NOT match the stub
	 * allowlist. A regression that only compared the low half would
	 * accept any address with the same low-32 bits as the stub. */
	int traps[] = { 99 };
	struct test_seccomp_data d = {
		.nr = 99, .arch = TEST_AUDIT_ARCH,
		.instruction_pointer = (STUB_ADDR & 0xffffffffULL) | 0x1234567800000000ULL,
	};
	test_int_eq(build_and_run(traps, 1, STUB_ADDR, 0, TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_TRAP);
}

/* ---------------- trap-list dispatch ---------------- */

test(filter_traps_listed_nr)
{
	int traps[] = { 100, 200, 300 };
	struct test_seccomp_data d = {
		.nr = 200, .arch = TEST_AUDIT_ARCH,
		.instruction_pointer = OUT_OF_STUB,
	};
	test_int_eq(build_and_run(traps, 3, STUB_ADDR, 0, TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_TRAP);
}

test(filter_allows_unlisted_nr)
{
	int traps[] = { 100, 200, 300 };
	struct test_seccomp_data d = {
		.nr = 250, .arch = TEST_AUDIT_ARCH,
		.instruction_pointer = OUT_OF_STUB,
	};
	test_int_eq(build_and_run(traps, 3, STUB_ADDR, 0, TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_ALLOW);
}

test(filter_traps_first_and_last_nr)
{
	/* Catches off-by-one at either end of the iteration. */
	int traps[] = { 100, 200, 300 };
	struct test_seccomp_data d = {
		.nr = 100, .arch = TEST_AUDIT_ARCH,
		.instruction_pointer = OUT_OF_STUB,
	};
	test_int_eq(build_and_run(traps, 3, STUB_ADDR, 0, TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_TRAP);
	d.nr = 300;
	test_int_eq(build_and_run(traps, 3, STUB_ADDR, 0, TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_TRAP);
}

test(filter_no_traps_default_allows)
{
	struct test_seccomp_data d = {
		.nr = 0, .arch = TEST_AUDIT_ARCH,
		.instruction_pointer = OUT_OF_STUB,
	};
	test_int_eq(build_and_run(NULL, 0, STUB_ADDR, 0, TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_ALLOW);
}

/* ---------------- close fast path ---------------- */

test(filter_close_with_unreserved_fd_allows)
{
	/* The pacman/gpgme closefrom dance: close(fd) for fd not in our
	 * reserved range must ALLOW inline (no SIGSYS round-trip). */
	int traps[] = { TAWC_SYS_close };
	struct test_seccomp_data d = {
		.nr = TAWC_SYS_close, .arch = TEST_AUDIT_ARCH,
		.instruction_pointer = OUT_OF_STUB,
		.args = { 7, 0, 0, 0, 0, 0 },   /* fd 7 — kernel-allocated */
	};
	test_int_eq(build_and_run(traps, 1, STUB_ADDR, 1000,
				  TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_ALLOW);
}

test(filter_close_with_reserved_fd_traps)
{
	int traps[] = { TAWC_SYS_close };
	struct test_seccomp_data d = {
		.nr = TAWC_SYS_close, .arch = TEST_AUDIT_ARCH,
		.instruction_pointer = OUT_OF_STUB,
		.args = { 1001, 0, 0, 0, 0, 0 },  /* above the floor */
	};
	test_int_eq(build_and_run(traps, 1, STUB_ADDR, 1000,
				  TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_TRAP);

	/* Exactly at the floor (JGE is inclusive) and far above — both
	 * trap. The far-above case is the key win over the old per-fd
	 * list: a runtime-reserved fd (shm/chroot) the filter never saw
	 * at install time is still covered by the range. */
	d.args[0] = 1000;
	test_int_eq(build_and_run(traps, 1, STUB_ADDR, 1000,
				  TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_TRAP);
	d.args[0] = 1000000;
	test_int_eq(build_and_run(traps, 1, STUB_ADDR, 1000,
				  TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_TRAP);

	/* Just below the floor — ALLOW inline (the closefrom fast path). */
	d.args[0] = 999;
	test_int_eq(build_and_run(traps, 1, STUB_ADDR, 1000,
				  TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_ALLOW);
}

test(filter_close_block_does_not_eat_following_traps)
{
	/* The not-close jf skip must land AFTER the close block's tail
	 * `LD nr` (so A holds nr again), making the following trap_nr's
	 * JEQ compare against nr, not args[0]. If the skip landed inside
	 * the block (post-`LD args[0]`), the close fd value would leak
	 * into the next comparison and (mis)match a trap_nr
	 * coincidentally equal to a reserved fd. Drive that exact
	 * collision: reserved fd 200 + a follow-up trap_nr 200 — the
	 * close-block ALLOW for fd 7 must not cause nr 200 to fall
	 * through to ALLOW. */
	int traps[] = { TAWC_SYS_close, 200 };
	struct test_seccomp_data d = {
		.nr = 200, .arch = TEST_AUDIT_ARCH,
		.instruction_pointer = OUT_OF_STUB,
		.args = { 7, 0, 0, 0, 0, 0 },
	};
	test_int_eq(build_and_run(traps, 2, STUB_ADDR, 1000,
				  TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_TRAP);
}

test(filter_close_with_zero_floor_falls_through_to_default)
{
	/* floor 0 disables the fast path: close is treated like any normal
	 * trapped NR (TRAPs unconditionally). Production only passes 0 when
	 * nothing is reserved yet, which never happens after rootfs_fd is
	 * reserved pre-install. */
	int traps[] = { TAWC_SYS_close };
	struct test_seccomp_data d = {
		.nr = TAWC_SYS_close, .arch = TEST_AUDIT_ARCH,
		.instruction_pointer = OUT_OF_STUB,
		.args = { 7, 0, 0, 0, 0, 0 },
	};
	test_int_eq(build_and_run(traps, 1, STUB_ADDR, 0,
				  TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_TRAP);
}

/* ---------------- mixed: realistic-ish trap set ---------------- */

test(filter_mixed_set_each_nr_routes_correctly)
{
	int traps[] = {
		TAWC_SYS_openat,
		TAWC_SYS_close,
		TAWC_SYS_fstatat,
		TAWC_SYS_readlinkat,
	};

	struct test_seccomp_data d = {
		.arch = TEST_AUDIT_ARCH,
		.instruction_pointer = OUT_OF_STUB,
	};

	d.nr = TAWC_SYS_openat;
	test_int_eq(build_and_run(traps, 4, STUB_ADDR, 1000,
				  TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_TRAP);

	d.nr = TAWC_SYS_close;
	d.args[0] = 7;
	test_int_eq(build_and_run(traps, 4, STUB_ADDR, 1000,
				  TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_ALLOW);
	d.args[0] = 1000;
	test_int_eq(build_and_run(traps, 4, STUB_ADDR, 1000,
				  TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_TRAP);

	d.nr = TAWC_SYS_fstatat;
	d.args[0] = 0;
	test_int_eq(build_and_run(traps, 4, STUB_ADDR, 1000,
				  TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_TRAP);

	d.nr = TAWC_SYS_readlinkat;
	test_int_eq(build_and_run(traps, 4, STUB_ADDR, 1000,
				  TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_TRAP);

	/* Untrapped NR that's adjacent to a trapped one — catches a
	 * different-by-one bug. */
	d.nr = TAWC_SYS_openat + 1;
	test_int_eq(build_and_run(traps, 4, STUB_ADDR, 1000,
				  TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_ALLOW);
}

/* ---------------- builder-side validation ---------------- */

test(filter_too_many_traps_is_e2big)
{
	/* The builder caps n_traps at 1900 (each trap_nr is two
	 * instructions, plus prologue and tail; 4096 BPF_MAXINSNS). */
	struct sock_filter prog[4];   /* deliberately tiny */
	long rv = tawcroot_build_filter(prog, 4, NULL, 9999,
					STUB_ADDR, 0, TEST_AUDIT_ARCH);
	test_int_eq(rv, -7);  /* E2BIG */
}

test(filter_capacity_overflow_is_e2big)
{
	/* prog_cap of 1 is not enough for the 11-instruction prologue. */
	int traps[] = { 100 };
	struct sock_filter prog[1];
	long rv = tawcroot_build_filter(prog, 1, traps, 1,
					STUB_ADDR, 0, TEST_AUDIT_ARCH);
	test_int_eq(rv, -7);
}

test(filter_close_range_block_is_fixed_size)
{
	/* The range-compare close block is a fixed 6 instructions
	 * regardless of how many fds are reserved, so the old per-fd u8
	 * jump-offset overflow can't happen. Confirm a huge floor value
	 * still builds (no list to encode) and traps a matching close. */
	int traps[] = { TAWC_SYS_close };
	struct test_seccomp_data d = {
		.nr = TAWC_SYS_close, .arch = TEST_AUDIT_ARCH,
		.instruction_pointer = OUT_OF_STUB,
		.args = { 2000000000, 0, 0, 0, 0, 0 },
	};
	test_int_eq(build_and_run(traps, 1, STUB_ADDR, 1000,
				  TEST_AUDIT_ARCH, &d),
		    SECCOMP_RET_TRAP);
}

test(filter_fork_traps_but_shared_vm_stack_switch_does_not)
{
	int traps[] = { TAWC_SYS_clone, TAWC_SYS_openat };
	struct test_seccomp_data d = {
		.nr = TAWC_SYS_clone, .arch = TEST_AUDIT_ARCH,
		.instruction_pointer = OUT_OF_STUB,
	};
	test_int_eq(build_and_run(traps, 2, STUB_ADDR, 0, TEST_AUDIT_ARCH, &d), SECCOMP_RET_TRAP);
	d.args[0] = CLONE_FS;
	test_int_eq(build_and_run(traps, 2, STUB_ADDR, 0, TEST_AUDIT_ARCH, &d), SECCOMP_RET_TRAP);
	d.args[0] = CLONE_VM | CLONE_FS | CLONE_THREAD;
	test_int_eq(build_and_run(traps, 2, STUB_ADDR, 0, TEST_AUDIT_ARCH, &d), SECCOMP_RET_ALLOW);
	d.nr = TAWC_SYS_openat;
	test_int_eq(build_and_run(traps, 2, STUB_ADDR, 0, TEST_AUDIT_ARCH, &d), SECCOMP_RET_TRAP);
}

/* ---------------- search tree at production scale ---------------- */

test(filter_large_set_routes_every_nr)
{
	/* 160 scattered NRs, given out of order: every one traps from
	 * guest code and is allowed from the stub, and every NR in between
	 * is allowed. Covers the JA-linked binary search at a size where
	 * plain jt/jf offsets would overflow. */
	int traps[160];
	for (int k = 0; k < 160; k++) traps[k] = (k * 37) % 160 * 3 + 1;
	static struct sock_filter prog[4096];
	long n = tawcroot_build_filter(prog, 4096, traps, 160, STUB_ADDR, 0,
				       TEST_AUDIT_ARCH);
	test_true(n > 0);
	for (int nr = 0; nr < 500; nr++) {
		int trapped = nr % 3 == 1 && nr < 480;
		if (nr == TAWC_SYS_rt_sigreturn) trapped = 1;
		struct test_seccomp_data d = {
			.nr = nr, .arch = TEST_AUDIT_ARCH,
			.instruction_pointer = OUT_OF_STUB,
		};
		uint32_t want = trapped ? SECCOMP_RET_TRAP : SECCOMP_RET_ALLOW;
		test_int_eq(run_filter(prog, (size_t)n, &d), want);
		d.instruction_pointer = STUB_ADDR;
		test_int_eq(run_filter(prog, (size_t)n, &d), SECCOMP_RET_ALLOW);
	}
}

test(filter_untrapped_nr_reads_only_arch_and_nr)
{
	/* The kernel's seccomp action cache can only mark an NR always-
	 * ALLOW when the filter decides it from arch + nr alone. Loading
	 * the IP (offsets 8/12) or args before the NR test defeats it. */
	int traps[] = { TAWC_SYS_openat, TAWC_SYS_close, TAWC_SYS_clone,
			TAWC_SYS_fstatat };
	static struct sock_filter prog[4096];
	long n = tawcroot_build_filter(prog, 4096, traps, 4, STUB_ADDR, 1000,
				       TEST_AUDIT_ARCH);
	test_true(n > 0);
	struct test_seccomp_data d = {
		.nr = 400, .arch = TEST_AUDIT_ARCH,   /* in none of the sets */
		.instruction_pointer = OUT_OF_STUB,
	};
	test_int_eq(run_filter(prog, (size_t)n, &d), SECCOMP_RET_ALLOW);
	test_int_eq(g_loaded, 0x3u);  /* words 0 (nr) and 1 (arch) */
}

test(filter_sigprocmask_traps_only_mask_growth)
{
	int traps[] = { TAWC_SYS_rt_sigprocmask, TAWC_SYS_openat };
	struct test_seccomp_data d = {
		.nr = TAWC_SYS_rt_sigprocmask, .arch = TEST_AUDIT_ARCH,
		.instruction_pointer = OUT_OF_STUB,
	};
	/* SIG_BLOCK / SIG_SETMASK with a set: trap. */
	d.args[0] = 0; d.args[1] = 0x7ffc00001000ULL;
	test_int_eq(build_and_run(traps, 2, STUB_ADDR, 0, TEST_AUDIT_ARCH, &d), SECCOMP_RET_TRAP);
	d.args[0] = 2;
	test_int_eq(build_and_run(traps, 2, STUB_ADDR, 0, TEST_AUDIT_ARCH, &d), SECCOMP_RET_TRAP);
	/* A set pointer whose low word is 0 is still a set. */
	d.args[1] = 0x100000000ULL;
	test_int_eq(build_and_run(traps, 2, STUB_ADDR, 0, TEST_AUDIT_ARCH, &d), SECCOMP_RET_TRAP);
	/* SIG_UNBLOCK, and queries, run natively. */
	d.args[0] = 1; d.args[1] = 0x7ffc00001000ULL;
	test_int_eq(build_and_run(traps, 2, STUB_ADDR, 0, TEST_AUDIT_ARCH, &d), SECCOMP_RET_ALLOW);
	d.args[0] = 0; d.args[1] = 0;
	test_int_eq(build_and_run(traps, 2, STUB_ADDR, 0, TEST_AUDIT_ARCH, &d), SECCOMP_RET_ALLOW);
	/* The following NR still routes. */
	d.nr = TAWC_SYS_openat;
	test_int_eq(build_and_run(traps, 2, STUB_ADDR, 0, TEST_AUDIT_ARCH, &d), SECCOMP_RET_TRAP);
}
