/* See include/fdpath.h. */

#include <stddef.h>
#include <stdint.h>

#include <sys/stat.h>

#include "fdpath.h"
#include "io.h"
#include "path.h"
#include "pathcache.h"
#include "raw_sys.h"
#include "stats.h"
#include "sysnr.h"

#define FDPATH_FDS  1024
#define FDPATH_MAX  256     /* longer paths are not cached */

struct fdpath_entry {
	uint32_t seq;           /* odd while being written */
	int32_t  root_fd;       /* root view it was filled under */
	uint64_t dev, ino, epoch;
	uint32_t len;
	char     path[FDPATH_MAX];
};

static struct fdpath_entry g_fdpath[FDPATH_FDS];

long tawcroot_fdpath_lookup(int fd, char *out, size_t out_cap,
			    struct tawcroot_fdpath_probe *probe)
{
	probe->valid = 0;
	if (fd < 0 || fd >= FDPATH_FDS) return -1;
	uint64_t epoch = tawcroot_pathcache_epoch();
	if (!epoch) return -1;   /* no path cache: nothing can validate */
	struct stat st;
	if (TAWC_RAW(TAWC_SYS_fstat, fd, (long)&st, 0, 0, 0, 0) < 0) return -1;
	probe->dev = (uint64_t)st.st_dev;
	probe->ino = (uint64_t)st.st_ino;
	probe->epoch = epoch;
	probe->valid = 1;

	struct fdpath_entry *e = &g_fdpath[fd];
	uint32_t s1 = __atomic_load_n(&e->seq, __ATOMIC_ACQUIRE);
	if (!s1 || (s1 & 1)) return -1;
	uint32_t len = e->len;
	if (e->root_fd != tawcroot_rootfs_fd || e->dev != probe->dev ||
	    e->ino != probe->ino || len == 0 || len >= FDPATH_MAX ||
	    len + 1 > out_cap)
		return -1;
	for (uint32_t i = 0; i < len; i++) out[i] = e->path[i];
	out[len] = 0;
	uint64_t ep = e->epoch;
	if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != s1) return -1;
	/* out is "/<suffix>"; the root itself is "/". */
	if (!tawcroot_pathcache_unchanged_since(out + 1, ep)) return -1;
	tawcroot_stats_event(TAWCROOT_EV_FDPATH_HIT);
	return (long)len;
}

void tawcroot_fdpath_fill(int fd, const struct tawcroot_fdpath_probe *probe,
			  const char *guest, size_t len,
			  const char *host, size_t host_len)
{
	if (!probe->valid || fd < 0 || fd >= FDPATH_FDS) return;
	if (len == 0 || len >= FDPATH_MAX || guest[0] != '/') return;
	/* Plain rootfs path: host == <rootfs host path> + guest (or exactly
	 * the rootfs path for "/"). Anything reached through a bind is not
	 * stamped by the path cache, so it is not cached. */
	size_t rl = tawcroot_rootfs_host_path_len;
	if (rl == 0 || host_len < rl) return;
	for (size_t i = 0; i < rl; i++)
		if (host[i] != tawcroot_rootfs_host_path[i]) return;
	if (len == 1) {
		if (host_len != rl) return;
	} else {
		if (host_len - rl != len) return;
		for (size_t i = 0; i < len; i++)
			if (host[rl + i] != guest[i]) return;
	}

	struct fdpath_entry *e = &g_fdpath[fd];
	uint32_t s = __atomic_load_n(&e->seq, __ATOMIC_ACQUIRE);
	if (s & 1) return;
	if (!__atomic_compare_exchange_n(&e->seq, &s, s + 1, 0,
					 __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return;
	e->root_fd = tawcroot_rootfs_fd;
	e->dev = probe->dev;
	e->ino = probe->ino;
	e->epoch = probe->epoch;
	e->len = (uint32_t)len;
	for (size_t i = 0; i < len; i++) e->path[i] = guest[i];
	__atomic_store_n(&e->seq, s + 2, __ATOMIC_RELEASE);
}
