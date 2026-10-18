/* See include/stats.h. Compiled to nothing unless TAWCROOT_STATS. */

#include "stats.h"

#if defined(TAWCROOT_STATS) && defined(__aarch64__)

#include <stddef.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "io.h"
#include "path_scratch.h"
#include "raw_sys.h"
#include "sysnr.h"

static struct tawcroot_stats_file *g_stats;

static void add(uint64_t *p, uint64_t v)
{
	__atomic_add_fetch(p, v, __ATOMIC_RELAXED);
}
static uint64_t g_main_entered;
static uint64_t g_mark_ticks, g_mark_faults;
static uint64_t g_phase_ticks[TAWCROOT_PHASES], g_phase_faults[TAWCROOT_PHASES];

/* Process-local running count of tawcroot's own syscalls; the handler
 * attributes the difference across one trap to that trap's nr. Threads
 * share it, so concurrent traps blur the split, never the totals. */
static uint64_t g_inner_seen;

void tawcroot_stats_inner(long nr)
{
	__atomic_add_fetch(&g_inner_seen, 1, __ATOMIC_RELAXED);
	if (g_stats && nr >= 0 && nr < TAWCROOT_STATS_NR)
		add(&g_stats->inner[nr], 1);
}

uint64_t tawcroot_stats_inner_seen(void)
{
	return __atomic_load_n(&g_inner_seen, __ATOMIC_RELAXED);
}

void tawcroot_stats_event(int ev)
{
	if (g_stats && ev >= 0 && ev < 6) add(&g_stats->events[ev], 1);
}

void tawcroot_stats_pathcache(int hit)
{
	if (!g_stats) return;
	add(&g_stats->pc_lookups, 1);
	if (hit) add(&g_stats->pc_hits, 1);
}

static uint64_t minflt(void)
{
	long ru[18];   /* struct rusage: 2 timevals, then 14 longs */
	if (TAWC_RAW(TAWC_SYS_getrusage, 0 /*RUSAGE_SELF*/, (long)ru, 0, 0, 0, 0) < 0)
		return 0;
	return (uint64_t)ru[8];   /* ru_minflt, after two timevals and maxrss..isrss */
}

void tawcroot_stats_init(const char *store_host_path, int top_level)
{
	g_stats = 0;
	if (!store_host_path || store_host_path[0] != '/') return;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path = scratch->buf[0];
	size_t pos = 0;
	if (tawc_str_append(path, TAWCROOT_PATH_SCRATCH_SIZE, &pos, store_host_path) ||
	    tawc_str_append(path, TAWCROOT_PATH_SCRATCH_SIZE, &pos, "-stats"))
		return;
	long fd = tawc_openat(AT_FDCWD, path,
			      O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) return;
	struct stat st;
	if (TAWC_RAW(TAWC_SYS_fstat, fd, (long)&st, 0, 0, 0, 0) < 0 ||
	    ((size_t)st.st_size < sizeof *g_stats &&
	     TAWC_RAW(TAWC_SYS_ftruncate, fd, (long)sizeof *g_stats, 0, 0, 0, 0) < 0)) {
		tawc_close((int)fd);
		return;
	}
	long m = tawc_mmap(0, sizeof *g_stats, PROT_READ | PROT_WRITE,
			   MAP_SHARED, (int)fd, 0);
	tawc_close((int)fd);
	if (m < 0 && m > -4096) return;
	struct tawcroot_stats_file *s = (struct tawcroot_stats_file *)m;
	uint64_t zero = 0;
	if (__atomic_compare_exchange_n(&s->magic, &zero, TAWCROOT_STATS_MAGIC, 0,
					__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		uint64_t f;
		__asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(f));
		__atomic_store_n(&s->freq, f, __ATOMIC_RELEASE);
	}
	if (__atomic_load_n(&s->magic, __ATOMIC_ACQUIRE) != TAWCROOT_STATS_MAGIC) {
		(void)tawc_munmap(s, sizeof *s);   /* another layout: leave it */
		return;
	}
	g_stats = s;
	if (top_level) add(&s->sessions, 1);
}

void tawcroot_stats_trap(long nr, uint64_t ticks, uint64_t inner)
{
	if (!g_stats || nr < 0 || nr >= TAWCROOT_STATS_NR) return;
	add(&g_stats->count[nr], 1);
	add(&g_stats->ticks[nr], ticks);
	add(&g_stats->inner_by_trap[nr], inner);
}

void tawcroot_stats_exec_prepared(uint64_t ticks)
{
	if (!g_stats) return;
	add(&g_stats->execs, 1);
	add(&g_stats->exec_prep_ticks, ticks);
}

void tawcroot_stats_main_entered(void)
{
	g_mark_faults = minflt();
	g_main_entered = g_mark_ticks = tawcroot_stats_now();
}

/* Before the mapping exists (early phases) the deltas are kept locally
 * and published at guest entry. */
void tawcroot_stats_mark(int phase)
{
	if (!g_main_entered || phase < 0 || phase >= TAWCROOT_PHASES) return;
	uint64_t t = tawcroot_stats_now(), f = minflt();
	g_phase_ticks[phase] += t - g_mark_ticks;
	g_phase_faults[phase] += f - g_mark_faults;
	g_mark_ticks = tawcroot_stats_now();
	g_mark_faults = f;
}

void tawcroot_stats_guest_entering(void)
{
	/* Only --exec-child children count; top-level entries have no
	 * prior exec to attribute this to. */
	if (!g_stats || !g_main_entered) return;
	tawcroot_stats_mark(TAWCROOT_PHASE_ENTRY);
	for (int i = 0; i < TAWCROOT_PHASES; i++) {
		add(&g_stats->phase_ticks[i], g_phase_ticks[i]);
		add(&g_stats->phase_faults[i], g_phase_faults[i]);
	}
	add(&g_stats->children, 1);
	add(&g_stats->child_init_ticks, tawcroot_stats_now() - g_main_entered);
}

#endif
