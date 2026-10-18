/* See include/linkobjs.h. */

#include <stddef.h>
#include <stdint.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "io.h"
#include "linkobjs.h"
#include "linkstore.h"
#include "path_scratch.h"
#include "raw_sys.h"
#include "sysnr.h"
#include "tawc_string.h"

#define LO_MAGIC   0x7461776c6f626a31ull   /* "tawlobj1" */
#define LO_SLOTS   65536u                  /* 512 KiB */
#define LO_PROBE   32u

struct lo_map {
	uint64_t magic;
	uint64_t populated;     /* 1 once some process scanned link/ */
	uint64_t pad[6];
	uint64_t slot[LO_SLOTS];   /* inode + 1; 0 = empty */
};

static struct lo_map *g_lo;
static int g_tried;   /* lazy init attempted in this process */

static uint64_t ld(const uint64_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }

static uint32_t home(uint64_t key)
{
	return (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 48) & (LO_SLOTS - 1);
}

static void insert(struct lo_map *m, uint64_t ino)
{
	uint64_t key = ino + 1;
	uint32_t h = home(key);
	for (uint32_t i = 0; i < LO_PROBE; i++) {
		uint64_t *s = &m->slot[(h + i) & (LO_SLOTS - 1)];
		uint64_t cur = ld(s);
		if (cur == key) return;
		if (cur == 0) {
			if (__atomic_compare_exchange_n(s, &cur, key, 0,
							__ATOMIC_ACQ_REL,
							__ATOMIC_ACQUIRE))
				return;
			if (cur == key) return;
		}
	}
	/* Window full: lookups for this key also run off the window and
	 * answer "maybe", so nothing is lost. */
}

/* Insert every regular file / symlink currently in link/. */
static void scan(struct lo_map *m, int link_dirfd)
{
	long dfd = tawc_openat(link_dirfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
	if (dfd < 0) return;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *buf = scratch->buf[0];
	int ok = 1;
	for (;;) {
		long n = TAWC_RAW(TAWC_SYS_getdents64, dfd, (long)buf,
				  TAWCROOT_PATH_SCRATCH_SIZE, 0, 0, 0);
		if (n < 0) { ok = 0; break; }
		if (n == 0) break;
		long off = 0;
		/* linux_dirent64: d_ino u64 at 0, d_reclen u16 at 16,
		 * d_name at 19. d_ino is the inode itself. */
		while (off + 19 < n) {
			unsigned short reclen;
			uint64_t ino;
			memcpy(&reclen, buf + off + 16, sizeof reclen);
			memcpy(&ino, buf + off, sizeof ino);
			if (reclen < 20 || off + reclen > n) { ok = 0; break; }
			const char *nm = buf + off + 19;
			off += reclen;
			if (nm[0] == '.') continue;
			insert(m, ino);
		}
		if (!ok) break;
	}
	tawc_close((int)dfd);
	if (ok) __atomic_store_n(&m->populated, 1, __ATOMIC_RELEASE);
}

static void u64_dec(uint64_t v, char *out)
{
	char t[24];
	int n = 0;
	do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (n) *out++ = t[--n];
	*out = 0;
}

void tawcroot_linkobjs_reset(void)
{
	if (g_lo) (void)tawc_munmap(g_lo, sizeof(struct lo_map));
	g_lo = 0;
	g_tried = 0;
}

/* Mapped on first use, not at store open: every --exec-child opens the
 * store, and most never stat an object candidate. */
static struct lo_map *ensure(void)
{
	if (g_lo) return g_lo;
	int expected = 0;
	if (!__atomic_compare_exchange_n(&g_tried, &expected, 1, 0,
					 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return __atomic_load_n(&g_lo, __ATOMIC_ACQUIRE);
	tawcroot_linkstore_objs_init();
	return __atomic_load_n(&g_lo, __ATOMIC_ACQUIRE);
}

void tawcroot_linkobjs_init(int store_dirfd, int link_dirfd)
{
	if (store_dirfd < 0 || link_dirfd < 0) return;
	/* Named after link/'s inode: a copied or re-imported store has a
	 * new link/ (and renumbered objects), so it starts a fresh set
	 * instead of trusting one populated against other inodes. */
	struct stat st;
	if (TAWC_RAW(TAWC_SYS_fstat, link_dirfd, (long)&st, 0, 0, 0, 0) < 0)
		return;
	char name[48] = "objcache-";
	u64_dec((uint64_t)st.st_ino, name + 9);
	long fd = tawc_openat(store_dirfd, name,
			      O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) return;
	if (TAWC_RAW(TAWC_SYS_fstat, fd, (long)&st, 0, 0, 0, 0) < 0 ||
	    !S_ISREG(st.st_mode) ||
	    ((size_t)st.st_size < sizeof(struct lo_map) &&
	     TAWC_RAW(TAWC_SYS_ftruncate, fd, (long)sizeof(struct lo_map),
		      0, 0, 0, 0) < 0)) {
		tawc_close((int)fd);
		return;
	}
	long mrv = tawc_mmap(0, sizeof(struct lo_map), PROT_READ | PROT_WRITE,
			     MAP_SHARED, (int)fd, 0);
	tawc_close((int)fd);
	if (mrv < 0 && mrv > -4096) return;
	struct lo_map *m = (struct lo_map *)mrv;
	uint64_t zero = 0;
	(void)__atomic_compare_exchange_n(&m->magic, &zero, LO_MAGIC, 0,
					  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	if (ld(&m->magic) != LO_MAGIC) {
		(void)tawc_munmap(m, sizeof(struct lo_map));
		return;
	}
	if (!ld(&m->populated)) scan(m, link_dirfd);
	__atomic_store_n(&g_lo, m, __ATOMIC_RELEASE);
}

void tawcroot_linkobjs_sweep(int store_dirfd, int link_dirfd)
{
	/* Drop sets left by earlier incarnations of link/. Top-level start
	 * only; a live session's own set has the current name. */
	struct stat st;
	if (store_dirfd < 0 || link_dirfd < 0 ||
	    TAWC_RAW(TAWC_SYS_fstat, link_dirfd, (long)&st, 0, 0, 0, 0) < 0)
		return;
	char keep[48] = "objcache-";
	u64_dec((uint64_t)st.st_ino, keep + 9);
	long dfd = tawc_openat(store_dirfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
	if (dfd < 0) return;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *buf = scratch->buf[0];
	for (;;) {
		long n = TAWC_RAW(TAWC_SYS_getdents64, dfd, (long)buf,
				  TAWCROOT_PATH_SCRATCH_SIZE, 0, 0, 0);
		if (n <= 0) break;
		long off = 0;
		while (off + 19 < n) {
			unsigned short reclen;
			memcpy(&reclen, buf + off + 16, sizeof reclen);
			if (reclen < 20 || off + reclen > n) break;
			const char *nm = buf + off + 19;
			off += reclen;
			if (tawc_starts_with(nm, "objcache-") && !tawc_streq(nm, keep))
				(void)TAWC_RAW(TAWC_SYS_unlinkat, dfd, (long)nm, 0, 0, 0, 0);
		}
	}
	tawc_close((int)dfd);
}

void tawcroot_linkobjs_add(uint64_t ino)
{
	struct lo_map *m = ensure();
	if (m) insert(m, ino);
}

int tawcroot_linkobjs_maybe(uint64_t ino)
{
	struct lo_map *m = ensure();
	if (!m || !ld(&m->populated)) return 1;
	uint64_t key = ino + 1;
	uint32_t h = home(key);
	for (uint32_t i = 0; i < LO_PROBE; i++) {
		uint64_t cur = ld(&m->slot[(h + i) & (LO_SLOTS - 1)]);
		if (cur == key) return 1;
		if (cur == 0) return 0;
	}
	return 1;
}
