/* See include/pathcache.h. */

#include <stddef.h>
#include <stdint.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "io.h"
#include "path.h"
#include "path_scratch.h"
#include "pathcache.h"
#include "raw_sys.h"
#include "sysnr.h"

#define PC_MAGIC    0x7461776370636331ull   /* "tawcpcc1" */
#define PC_VERSION  1u
#define PC_ENTRIES  16384u                  /* 512 KiB of entries */
#define PC_STAMPS   8192u                   /* 64 KiB of stamps */

struct pc_header {
	uint64_t magic;
	uint32_t version;
	uint32_t entries;
	uint32_t stamps;
	uint32_t pad;
	uint64_t epoch;    /* bumped by every mutation; never 0 once live */
	uint64_t floor;    /* entries older than this are invalid */
	uint64_t pad2[3];
};

/* One cached prefix, guarded by a per-slot seqlock (odd = being
 * written). Two independent 64-bit hashes make a false hit
 * negligible. */
struct pc_entry {
	uint32_t seq;
	uint32_t pad;
	uint64_t h1;
	uint64_t h2;
	uint64_t epoch;
};

struct pc_map {
	struct pc_header hdr;
	struct pc_entry  entries[PC_ENTRIES];
	uint64_t         stamps[PC_STAMPS];  /* epoch of last mutation, by h1 */
};

static struct pc_map *g_pc;

/* Hash state after the current root's host path, recomputed when a
 * chroot swaps the root. */
static int      g_root_fd = -1;
static size_t   g_root_len;
static uint64_t g_root_h1, g_root_h2;

static void mix(uint64_t *h1, uint64_t *h2, unsigned char c)
{
	*h1 = (*h1 ^ c) * 0x100000001b3ull;                  /* FNV-1a */
	*h2 = (*h2 + c + 1) * 0x9e3779b97f4a7c15ull;
	*h2 ^= *h2 >> 29;
}

static void root_state(uint64_t *h1, uint64_t *h2)
{
	int fd = tawcroot_rootfs_fd;
	size_t len = tawcroot_rootfs_host_path_len;
	if (fd != g_root_fd || len != g_root_len) {
		uint64_t a = 0xcbf29ce484222325ull, b = 0x84222325cbf29ce4ull;
		for (size_t i = 0; i < len; i++)
			mix(&a, &b, (unsigned char)tawcroot_rootfs_host_path[i]);
		if (len == 0 || tawcroot_rootfs_host_path[len - 1] != '/')
			mix(&a, &b, '/');
		g_root_h1 = a;
		g_root_h2 = b;
		g_root_len = len;
		g_root_fd = fd;
	}
	*h1 = g_root_h1;
	*h2 = g_root_h2;
}

static uint64_t load(const uint64_t *p)
{
	return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static uint64_t *stamp(uint64_t h1)
{
	return &g_pc->stamps[(h1 >> 17) & (PC_STAMPS - 1)];
}

static struct pc_entry *entry(uint64_t h1)
{
	return &g_pc->entries[h1 & (PC_ENTRIES - 1)];
}

/* Hash the first `len` bytes of `suffix` under the current root. With
 * `newest` set, also report the newest mutation stamp on the path
 * itself or any of its prefixes. */
static int hash_suffix(const char *suffix, size_t len, uint64_t *h1,
		       uint64_t *h2, uint64_t *newest)
{
	if (!suffix || len == 0 || suffix[0] == '/') return 0;
	root_state(h1, h2);
	uint64_t m = 0;
	for (size_t i = 0; i < len; i++) {
		if (suffix[i] == '/' && newest) {
			uint64_t s = load(stamp(*h1));
			if (s > m) m = s;
		}
		mix(h1, h2, (unsigned char)suffix[i]);
	}
	if (newest) {
		uint64_t s = load(stamp(*h1));
		*newest = s > m ? s : m;
	}
	return 1;
}

void tawcroot_pathcache_init(const char *store_host_path)
{
	g_pc = 0;
	g_root_fd = -1;
	if (!store_host_path || store_host_path[0] != '/') return;

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path = scratch->buf[0];
	size_t pos = 0;
	if (tawc_str_append(path, TAWCROOT_PATH_SCRATCH_SIZE, &pos,
			    store_host_path) ||
	    tawc_str_append(path, TAWCROOT_PATH_SCRATCH_SIZE, &pos,
			    "-pathcache"))
		return;
	long fd = tawc_openat(AT_FDCWD, path,
			      O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) return;
	struct stat st;
	if (TAWC_RAW(TAWC_SYS_fstat, fd, (long)&st, 0, 0, 0, 0) < 0 ||
	    !S_ISREG(st.st_mode) ||
	    ((size_t)st.st_size < sizeof(struct pc_map) &&
	     TAWC_RAW(TAWC_SYS_ftruncate, fd, (long)sizeof(struct pc_map),
		      0, 0, 0, 0) < 0)) {
		tawc_close((int)fd);
		return;
	}
	long m = tawc_mmap(0, sizeof(struct pc_map), PROT_READ | PROT_WRITE,
			   MAP_SHARED, (int)fd, 0);
	tawc_close((int)fd);
	if (m < 0 && m > -4096) return;
	struct pc_map *pc = (struct pc_map *)m;

	/* First mapper initializes the header; zeroed slots are empty. */
	uint64_t zero = 0;
	if (__atomic_compare_exchange_n(&pc->hdr.magic, &zero, PC_MAGIC, 0,
					__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		pc->hdr.version = PC_VERSION;
		pc->hdr.entries = PC_ENTRIES;
		pc->hdr.stamps  = PC_STAMPS;
		__atomic_store_n(&pc->hdr.epoch, 1, __ATOMIC_RELEASE);
	}
	if (load(&pc->hdr.magic) != PC_MAGIC ||
	    __atomic_load_n(&pc->hdr.version, __ATOMIC_ACQUIRE) != PC_VERSION ||
	    pc->hdr.entries != PC_ENTRIES || pc->hdr.stamps != PC_STAMPS ||
	    load(&pc->hdr.epoch) == 0) {
		(void)tawc_munmap(pc, sizeof(struct pc_map));
		return;
	}
	g_pc = pc;
}

void tawcroot_pathcache_session_start(void)
{
	if (!g_pc) return;
	uint64_t e = __atomic_add_fetch(&g_pc->hdr.epoch, 1, __ATOMIC_ACQ_REL);
	__atomic_store_n(&g_pc->hdr.floor, e, __ATOMIC_RELEASE);
}

uint64_t tawcroot_pathcache_epoch(void)
{
	return g_pc ? load(&g_pc->hdr.epoch) : 0;
}

int tawcroot_pathcache_known_plain(const char *suffix)
{
	if (!g_pc) return 0;
	uint64_t h1, h2, newest;
	size_t len = suffix ? tawc_strlen(suffix) : 0;
	/* Prefixes never end in '/'; a dir-marker form is not a key. */
	if (len == 0 || suffix[len - 1] == '/') return 0;
	if (!hash_suffix(suffix, len, &h1, &h2, &newest)) return 0;
	struct pc_entry *e = entry(h1);
	uint32_t s1 = __atomic_load_n(&e->seq, __ATOMIC_ACQUIRE);
	if (s1 & 1) return 0;
	uint64_t k1 = load(&e->h1), k2 = load(&e->h2), ep = load(&e->epoch);
	uint32_t s2 = __atomic_load_n(&e->seq, __ATOMIC_ACQUIRE);
	return s1 == s2 && k1 == h1 && k2 == h2 && ep >= newest &&
	       ep >= load(&g_pc->hdr.floor);
}

int tawcroot_pathcache_unchanged_since(const char *suffix, uint64_t epoch)
{
	if (!g_pc || !epoch || epoch < load(&g_pc->hdr.floor)) return 0;
	size_t len = suffix ? tawc_strlen(suffix) : 0;
	if (len == 0) return 1;   /* the root itself: covered by the floor */
	if (suffix[len - 1] == '/') return 0;
	uint64_t h1, h2, newest;
	if (!hash_suffix(suffix, len, &h1, &h2, &newest)) return 0;
	return epoch >= newest;
}

void tawcroot_pathcache_note_plain(const char *suffix, uint64_t epoch)
{
	if (!g_pc || !epoch) return;
	uint64_t h1, h2;
	size_t len = suffix ? tawc_strlen(suffix) : 0;
	if (len == 0 || suffix[len - 1] == '/') return;
	if (!hash_suffix(suffix, len, &h1, &h2, 0)) return;
	struct pc_entry *e = entry(h1);
	uint32_t s = __atomic_load_n(&e->seq, __ATOMIC_ACQUIRE);
	if (s & 1) return;
	if (!__atomic_compare_exchange_n(&e->seq, &s, s + 1, 0,
					 __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return;
	__atomic_store_n(&e->h1, h1, __ATOMIC_RELAXED);
	__atomic_store_n(&e->h2, h2, __ATOMIC_RELAXED);
	__atomic_store_n(&e->epoch, epoch, __ATOMIC_RELAXED);
	__atomic_store_n(&e->seq, s + 2, __ATOMIC_RELEASE);
}

void tawcroot_pathcache_mutated(int base_fd, const char *suffix)
{
	if (!g_pc || base_fd != tawcroot_rootfs_fd) return;
	uint64_t h1, h2;
	/* Trailing '/' (rmdir "dir/") names the same object. */
	size_t len = suffix ? tawc_strlen(suffix) : 0;
	while (len > 0 && suffix[len - 1] == '/') len--;
	if (!hash_suffix(suffix, len, &h1, &h2, 0)) {
		/* The root itself (or an unexpected form): drop everything. */
		tawcroot_pathcache_session_start();
		return;
	}
	/* Stamp first, then advance the epoch: an entry stamped before
	 * this call is older than the stamp from the moment it lands, and
	 * a reader that sees the new epoch also sees the stamp. */
	uint64_t want = load(&g_pc->hdr.epoch) + 1;
	uint64_t *st = stamp(h1);
	uint64_t cur = load(st);
	while (cur < want &&
	       !__atomic_compare_exchange_n(st, &cur, want, 0,
					    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
	__atomic_add_fetch(&g_pc->hdr.epoch, 1, __ATOMIC_ACQ_REL);
}
