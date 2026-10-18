/* Measurement-only trap/exec accounting (build with -DTAWCROOT_STATS).
 *
 * Every tawcroot process of a distro adds into one MAP_SHARED file
 * beside the link store (`<store>-stats`): per-syscall trap counts and
 * handler time, guest execs with the time spent preparing them, and
 * --exec-child init time from main() to the guest jump. Times are in
 * ticks of the ARM64 virtual counter (cntvct_el0, frequency recorded in
 * the header), readable from user space without a syscall. Production
 * builds compile all of this out; on non-ARM64 builds the hooks are
 * no-ops. tawcroot/perf/trap-stats.py reads the file.
 */

#pragma once

#include <stdint.h>

#define TAWCROOT_STATS_MAGIC  0x7461776373746135ull  /* "tawcsta5" */
#define TAWCROOT_STATS_NR     1024

/* Counted events (tawcroot_stats_event). */
enum {
	TAWCROOT_EV_PROCFD_READLINK,  /* any readlink of /proc/self/fd/N */
	TAWCROOT_EV_DIRFD_LIFT,       /* translate_local lifting a dirfd */
	TAWCROOT_EV_FSTAT_NLINK,      /* fd_object_nlink probing a fd */
	TAWCROOT_EV_GETDENTS_CLASSIFY,/* getdents64 classifying its fd */
	TAWCROOT_EV_RO_BIND_FD,       /* fd_in_ro_bind */
};

/* --exec-child phases (tawcroot_stats_mark). */
enum {
	TAWCROOT_PHASE_STATE,       /* main() -> exec state read */
	TAWCROOT_PHASE_SV_ROOT,     /* -> root fd, canonical path, usercopy */
	TAWCROOT_PHASE_SV_BINDS,    /* -> bind table */
	TAWCROOT_PHASE_SV_STORE,    /* -> shm table, link store, path cache */
	TAWCROOT_PHASE_SV_MEMO,     /* -> well-known symlink memo */
	TAWCROOT_PHASE_SV_HANDLER,  /* -> dispatch table, handler, mask, self path */
	TAWCROOT_PHASE_SUPERVISOR,  /* -> remaining child view state */
	TAWCROOT_PHASE_BINARY,      /* -> guest binary opened and mapped */
	TAWCROOT_PHASE_INTERP,      /* -> ld.so mapped */
	TAWCROOT_PHASE_ENTRY,       /* -> stack built, jumping */
	TAWCROOT_PHASES = 16,
};

struct tawcroot_stats_file {
	uint64_t magic;
	uint64_t freq;               /* cntfrq_el0, ticks per second */
	uint64_t execs;              /* guest execve/execveat reaching commit */
	uint64_t exec_prep_ticks;    /* handler entry -> commit */
	uint64_t children;           /* --exec-child processes reaching the guest */
	uint64_t child_init_ticks;   /* main() -> guest entry jump */
	uint64_t sessions;           /* top-level tawcroot starts */
	uint64_t pad[9];
	/* --exec-child init split at tawcroot_stats_mark() points: ticks
	 * and minor faults since the previous mark (or main()). */
	uint64_t phase_ticks[TAWCROOT_PHASES];
	uint64_t phase_faults[TAWCROOT_PHASES];
	uint64_t pc_lookups;         /* path cache consulted (rootfs prefix) */
	uint64_t pc_hits;            /* ... and answered "not a symlink" */
	uint64_t events[6];          /* TAWCROOT_EV_* */
	uint64_t count[TAWCROOT_STATS_NR];
	uint64_t ticks[TAWCROOT_STATS_NR];
	uint64_t inner[TAWCROOT_STATS_NR];          /* syscalls tawcroot issued, by nr */
	uint64_t inner_by_trap[TAWCROOT_STATS_NR];  /* ... while handling trap nr */
};

#if defined(TAWCROOT_STATS) && defined(__aarch64__)

static inline uint64_t tawcroot_stats_now(void)
{
	uint64_t v;
	__asm__ __volatile__("isb; mrs %0, cntvct_el0" : "=r"(v) :: "memory");
	return v;
}

void tawcroot_stats_init(const char *store_host_path, int top_level);
void tawcroot_stats_trap(long nr, uint64_t ticks, uint64_t inner);
uint64_t tawcroot_stats_inner_seen(void);
void tawcroot_stats_exec_prepared(uint64_t ticks);
void tawcroot_stats_main_entered(void);
void tawcroot_stats_guest_entering(void);
void tawcroot_stats_mark(int phase);
void tawcroot_stats_pathcache(int hit);
void tawcroot_stats_event(int ev);

#else

static inline uint64_t tawcroot_stats_now(void) { return 0; }
static inline void tawcroot_stats_init(const char *s, int t) { (void)s; (void)t; }
static inline void tawcroot_stats_trap(long nr, uint64_t t, uint64_t i) { (void)nr; (void)t; (void)i; }
static inline uint64_t tawcroot_stats_inner_seen(void) { return 0; }
static inline void tawcroot_stats_exec_prepared(uint64_t t) { (void)t; }
static inline void tawcroot_stats_main_entered(void) {}
static inline void tawcroot_stats_guest_entering(void) {}
static inline void tawcroot_stats_mark(int phase) { (void)phase; }
static inline void tawcroot_stats_pathcache(int hit) { (void)hit; }
static inline void tawcroot_stats_event(int ev) { (void)ev; }

#endif
