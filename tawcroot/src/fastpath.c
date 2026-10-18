/* Experimental SIGSYS bypass by rewriting guest syscall sites
 * (include/fastpath.h). Built only with -DTAWCROOT_FASTPATH on aarch64.
 *
 * Patch protocol, run inside the SIGSYS handler (all signals blocked)
 * under one global flag:
 *   1. copy the stub template into an RWX region within branch range
 *      of the site, point its tail `b` at site+4, flush it;
 *   2. make the site's page RWX, store `b stub` over the `svc`, flush,
 *      restore r-x.
 * SVC and B are both on the architecture's list of instructions that
 * may be modified while another thread executes them, so a racing
 * thread sees either the old trap or the finished stub.
 *
 * Only sites inside a private file mapping that /proc/self/maps shows
 * as r-xp are patched (never JIT or writable code), and the page goes
 * back to r-x. Making an already-executable mapping writable needs
 * SELinux execmem, not execmod, as do the RWX stub regions. Stubs leak
 * with their library.
 */

#if defined(TAWCROOT_FASTPATH) && defined(__aarch64__)

#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "arch.h"
#include "chroot.h"
#include "fastpath.h"
#include "raw_sys.h"
#include "rescue.h"
#include "sysnr.h"
#include "usercopy.h"

#ifndef PR_SET_VMA
# define PR_SET_VMA           0x53564d41
# define PR_SET_VMA_ANON_NAME 0
#endif
#ifndef MAP_FIXED_NOREPLACE
# define MAP_FIXED_NOREPLACE 0x100000
#endif

#define FP_SITES       4096
#define FP_MIN_TRAPS   4
#define FP_REGION      (64 * 1024)
#define FP_REGIONS     64
#define FP_STUB        32
#define FP_REACH       (120L * 1024 * 1024)
#define FP_PAGE        4096UL

#define INSN_SVC0      0xd4000001u
#define FP_RANGES      256

extern const uint32_t tawcroot_fast_stub_tmpl[];
extern void tawcroot_fast_common(void);

struct fp_site {
	uintptr_t pc;      /* address of the svc */
	uint32_t  traps;
	uint32_t  done;    /* 1 patched, 2 rejected */
};

struct fp_region {
	uintptr_t base;
	uint32_t  used;
};

static struct fp_site   g_sites[FP_SITES];
static struct fp_region g_regions[FP_REGIONS];
static int              g_lock;

/* r-xp file-backed ranges from the last /proc/self/maps read. Guarded
 * by g_lock. */
static struct { uintptr_t lo, hi; } g_rx[FP_RANGES];
static int  g_nrx;
static char g_maps_buf[4096];

static uintptr_t parse_hex(const char **pp, const char *end)
{
	uintptr_t v = 0;
	const char *p = *pp;
	for (; p < end; p++) {
		char c = *p;
		unsigned d;
		if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
		else break;
		v = (v << 4) | d;
	}
	*pp = p;
	return v;
}

/* "lo-hi r-xp off dev inode ..." with a nonzero inode. */
static void maps_line(const char *p, const char *end)
{
	uintptr_t lo = parse_hex(&p, end);
	if (p >= end || *p++ != '-') return;
	uintptr_t hi = parse_hex(&p, end);
	if (end - p < 6 || p[0] != ' ' || p[1] != 'r' || p[2] != '-' ||
	    p[3] != 'x' || p[4] != 'p' || p[5] != ' ')
		return;
	p += 6;
	for (int field = 0; field < 2; field++) {   /* offset, dev */
		while (p < end && *p != ' ') p++;
		while (p < end && *p == ' ') p++;
	}
	if (p >= end || *p == '0') return;          /* inode 0: anonymous */
	if (g_nrx < FP_RANGES) {
		g_rx[g_nrx].lo = lo;
		g_rx[g_nrx].hi = hi;
		g_nrx++;
	}
}

static void maps_reload(void)
{
	g_nrx = 0;
	long fd = TAWC_RAW(TAWC_SYS_openat, AT_FDCWD, (long)"/proc/self/maps",
			   O_RDONLY | O_CLOEXEC, 0, 0, 0);
	if (fd < 0) return;
	size_t have = 0;
	for (;;) {
		long n = TAWC_RAW(TAWC_SYS_read, fd, (long)(g_maps_buf + have),
				  (long)(sizeof g_maps_buf - have), 0, 0, 0);
		if (n <= 0) break;
		have += (size_t)n;
		size_t start = 0;
		for (size_t i = 0; i < have; i++) {
			if (g_maps_buf[i] != '\n') continue;
			maps_line(g_maps_buf + start, g_maps_buf + i);
			start = i + 1;
		}
		/* Carry a partial line; drop one longer than the buffer. */
		have -= start;
		for (size_t i = 0; i < have; i++)
			g_maps_buf[i] = g_maps_buf[start + i];
		if (have == sizeof g_maps_buf) have = 0;
	}
	TAWC_RAW(TAWC_SYS_close, fd, 0, 0, 0, 0, 0);
}

static int rx_has(uintptr_t a)
{
	for (int i = 0; i < g_nrx; i++)
		if (a >= g_rx[i].lo && a < g_rx[i].hi) return 1;
	return 0;
}

/* Is `site` in r-xp file text? Rereads maps once on a miss. */
static int site_is_text(uintptr_t site)
{
	if (rx_has(site)) return 1;
	maps_reload();
	return rx_has(site);
}

static int eligible(long nr)
{
	switch (nr) {
	case TAWC_SYS_getuid: case TAWC_SYS_geteuid:
	case TAWC_SYS_getgid: case TAWC_SYS_getegid:
	case TAWC_SYS_openat: case TAWC_SYS_close:
	case TAWC_SYS_fstatat: case TAWC_SYS_fstat: case TAWC_SYS_statx:
	case TAWC_SYS_readlinkat: case TAWC_SYS_faccessat:
	case TAWC_SYS_faccessat2: case TAWC_SYS_getdents64:
	case TAWC_SYS_getcwd:
		return 1;
	}
	return 0;
}

static struct fp_site *site_slot(uintptr_t pc)
{
	size_t h = (size_t)((pc >> 2) * 0x9e3779b97f4a7c15ULL) >> 52;
	for (size_t i = 0; i < 16; i++) {
		struct fp_site *s = &g_sites[(h + i) & (FP_SITES - 1)];
		uintptr_t cur = __atomic_load_n(&s->pc, __ATOMIC_RELAXED);
		if (cur == pc) return s;
		if (cur == 0 &&
		    __atomic_compare_exchange_n(&s->pc, &cur, pc, 0,
						__ATOMIC_RELAXED, __ATOMIC_RELAXED))
			return s;
		if (cur == pc) return s;
	}
	return NULL;
}

static void flush_code(uintptr_t lo, uintptr_t hi)
{
	uint64_t ctr;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	uintptr_t dl = 4UL << ((ctr >> 16) & 0xf);
	uintptr_t il = 4UL << (ctr & 0xf);
	for (uintptr_t p = lo & ~(dl - 1); p < hi; p += dl)
		__asm__ volatile("dc cvau, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb ish" ::: "memory");
	for (uintptr_t p = lo & ~(il - 1); p < hi; p += il)
		__asm__ volatile("ic ivau, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb ish\n\tisb" ::: "memory");
}

static int in_reach(uintptr_t a, uintptr_t b)
{
	long d = (long)(a - b);
	return d > -FP_REACH && d < FP_REACH;
}

static uintptr_t stub_alloc(uintptr_t site)
{
	for (int i = 0; i < FP_REGIONS; i++) {
		struct fp_region *r = &g_regions[i];
		if (!r->base) {
			/* Search outward from the site in region-sized steps. */
			uintptr_t near = site & ~(uintptr_t)(FP_REGION - 1);
			for (long k = 1; k < 1800; k++) {
				uintptr_t want = (k & 1)
					? near + (uintptr_t)((k + 1) / 2) * FP_REGION
					: near - (uintptr_t)(k / 2) * FP_REGION;
				long p = TAWC_RAW(TAWC_SYS_mmap, (long)want, FP_REGION,
						  PROT_READ | PROT_WRITE | PROT_EXEC,
						  MAP_PRIVATE | MAP_ANONYMOUS |
						  MAP_FIXED_NOREPLACE, -1, 0);
				if (p == (long)want) {
					TAWC_RAW(TAWC_SYS_prctl, PR_SET_VMA,
						 PR_SET_VMA_ANON_NAME, want,
						 FP_REGION,
						 (long)"tawcroot-fastpath", 0);
					r->base = want;
					r->used = 0;
					break;
				}
				if (p >= 0)
					TAWC_RAW(TAWC_SYS_munmap, p, FP_REGION,
						 0, 0, 0, 0);
				else if (p != -17 /* EEXIST */)
					return 0;
			}
			if (!r->base) return 0;
		}
		if (in_reach(r->base, site) && r->used + FP_STUB <= FP_REGION) {
			uintptr_t s = r->base + r->used;
			r->used += FP_STUB;
			return s;
		}
	}
	return 0;
}

static uint32_t enc_b(uintptr_t from, uintptr_t to)
{
	return 0x14000000u | (uint32_t)(((long)(to - from) >> 2) & 0x3ffffff);
}

static int movz_x8(uint32_t insn, long nr)
{
	uint32_t base = insn & ~(0xffffu << 5);
	return (base == 0x52800008u || base == 0xd2800008u) &&
	       ((insn >> 5) & 0xffff) == (uint32_t)nr;
}

static void patch(struct fp_site *s, uintptr_t site, long nr)
{
	uint32_t insn[2];
	if (tawc_copy_from_guest(insn, sizeof insn, (void *)(site - 4)) < 0 ||
	    insn[1] != INSN_SVC0 || !movz_x8(insn[0], nr)) {
		s->done = 2;
		return;
	}
	if (!site_is_text(site)) { s->done = 2; return; }
	uintptr_t stub = stub_alloc(site);
	if (!stub) { s->done = 2; return; }

	uint32_t *w = (uint32_t *)stub;
	for (int i = 0; i < FP_STUB / 4; i++) w[i] = tawcroot_fast_stub_tmpl[i];
	w[4] = enc_b(stub + 16, site + 4);
	*(uint64_t *)(stub + 24) = (uint64_t)(uintptr_t)tawcroot_fast_common;
	flush_code(stub, stub + FP_STUB);

	uintptr_t page = site & ~(FP_PAGE - 1);
	if (TAWC_RAW(TAWC_SYS_mprotect, (long)page, FP_PAGE,
		     PROT_READ | PROT_WRITE | PROT_EXEC, 0, 0, 0) != 0) {
		s->done = 2;
		return;
	}
	__atomic_store_n((uint32_t *)site, enc_b(site, stub), __ATOMIC_RELEASE);
	flush_code(site, site + 4);
	TAWC_RAW(TAWC_SYS_mprotect, (long)page, FP_PAGE,
		 PROT_READ | PROT_EXEC, 0, 0, 0);
	s->done = 1;
}

void tawcroot_fastpath_note(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	if (!eligible(args->nr)) return;
	uintptr_t site = tawcroot_arch_resume_pc(uc) - 4;
	struct fp_site *s = site_slot(site);
	if (!s || s->done) return;
	if (__atomic_add_fetch(&s->traps, 1, __ATOMIC_RELAXED) < FP_MIN_TRAPS)
		return;
	int expected = 0;
	if (!__atomic_compare_exchange_n(&g_lock, &expected, 1, 0,
					 __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return;
	if (!s->done) patch(s, site, args->nr);
	__atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE);
}

/* Called by tawcroot_fast_common with the guest's x0..x5 and x8. */
long tawcroot_fast_entry(long a, long b, long c, long d, long e, long f,
			 long nr);
long tawcroot_fast_entry(long a, long b, long c, long d, long e, long f,
			 long nr)
{
#if TAWCROOT_FASTPATH == 1
	uint64_t all = ~((uint64_t)1 << (SIGSYS - 1)), old;
	TAWC_RAW(TAWC_SYS_rt_sigprocmask, SIG_SETMASK, (long)&all, (long)&old,
		 8, 0, 0);
#endif
	tawcroot_syscall_args args = {
		.nr = nr, .a = a, .b = b, .c = c, .d = d, .e = e, .f = f,
	};
	long rv = tawcroot_fs_sync();
	if (!rv) rv = tawcroot_dispatch_call(&args, NULL);
#if TAWCROOT_FASTPATH == 1
	TAWC_RAW(TAWC_SYS_rt_sigprocmask, SIG_SETMASK, (long)&old, 0, 8, 0, 0);
#endif
	return rv;
}

#endif
