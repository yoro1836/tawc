/* fd -> guest path cache (src/fdpath.c) through the real handlers.
 *
 * The cache only runs with the shared path cache on, so each test maps
 * one for its view and turns it off again before teardown. Correctness
 * cases drive a dirfd-relative fstatat after the directory behind the
 * fd moves, after a parent moves, and after the fd number is reused: a
 * stale cached path would resolve the name in the wrong directory. */
#include <cleat/test.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hosted.h"
#include "../integration/rootfs_helpers.h"
#include "errno_neg.h"
#include "pathcache.h"
#include "sysnr.h"

static int procfd_readlinks;
static bool count_procfd(long nr, const long args[6], long *ret)
{
	(void)ret;
	if (nr == TAWC_SYS_readlinkat &&
	    strncmp((const char *)args[1], "/proc/self/fd/", 14) == 0)
		procfd_readlinks++;
	return false;   /* observe only */
}

static void cache_on(th_view *v, char *store, size_t cap)
{
	snprintf(store, cap, "%s-store", v->root);
	tawcroot_pathcache_init(store);
}

static void cache_off(const char *store)
{
	tawcroot_pathcache_init(NULL);
	char p[4300];
	snprintf(p, sizeof p, "%s-pathcache", store);
	unlink(p);
}

static void make(TestCtx *test_ctx, th_view *v, const char *rel, int dir)
{
	char p[4300];
	snprintf(p, sizeof p, "%s%s", v->root, rel);
	if (dir) {
		test_true(rh_mkdir_p(p, 0755));
	} else {
		test_true(rh_write_text(p, "x\n"));
	}
}

static long stat_at(TestCtx *test_ctx, int dirfd, const char *name)
{
	struct stat st;
	return th_sys(TAWC_SYS_fstatat, dirfd, name, &st, AT_SYMLINK_NOFOLLOW, 0, 0);
}

static int open_dir(TestCtx *test_ctx, const char *guest)
{
	long fd = th_sys(TAWC_SYS_openat, AT_FDCWD, guest,
			 O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0, 0, 0);
	test_true(fd >= 0);
	return (int)fd;
}

test(hosted_fdpath_second_lift_skips_proc_readlink)
{
	th_view v;
	th_setup(&v, "fdpath-hit");
	char store[4300];
	cache_on(&v, store, sizeof store);
	make(test_ctx, &v, "/tmp/d", 1);
	make(test_ctx, &v, "/tmp/d/f", 0);
	int fd = open_dir(test_ctx, "/tmp/d");
	procfd_readlinks = 0;
	tawcroot_test_raw_hook = count_procfd;
	test_int_eq(stat_at(test_ctx, fd, "f"), 0);
	int first = procfd_readlinks;
	test_int_eq(stat_at(test_ctx, fd, "f"), 0);
	test_int_eq(stat_at(test_ctx, fd, "f"), 0);
	tawcroot_test_raw_hook = NULL;
	test_true(first >= 1);
	test_int_eq(procfd_readlinks, first);   /* later lifts were cached */
	close(fd);
	cache_off(store);
	th_teardown(&v);
}

test(hosted_fdpath_follows_renamed_directory)
{
	th_view v;
	th_setup(&v, "fdpath-rename");
	char store[4300];
	cache_on(&v, store, sizeof store);
	make(test_ctx, &v, "/tmp/a", 1);
	make(test_ctx, &v, "/tmp/a/f", 0);
	int fd = open_dir(test_ctx, "/tmp/a");
	test_int_eq(stat_at(test_ctx, fd, "f"), 0);          /* fills the cache */
	test_int_eq(th_sys(TAWC_SYS_renameat, AT_FDCWD, "/tmp/a", AT_FDCWD, "/tmp/b", 0, 0), 0);
	make(test_ctx, &v, "/tmp/a", 1);            /* same name, other dir */
	make(test_ctx, &v, "/tmp/a/g", 0);
	test_int_eq(stat_at(test_ctx, fd, "f"), 0);          /* fd follows the inode */
	test_int_eq(stat_at(test_ctx, fd, "g"), TAWC_ENOENT);
	close(fd);
	cache_off(store);
	th_teardown(&v);
}

test(hosted_fdpath_follows_renamed_parent)
{
	th_view v;
	th_setup(&v, "fdpath-parent");
	char store[4300];
	cache_on(&v, store, sizeof store);
	make(test_ctx, &v, "/tmp/p/q", 1);
	make(test_ctx, &v, "/tmp/p/q/f", 0);
	int fd = open_dir(test_ctx, "/tmp/p/q");
	test_int_eq(stat_at(test_ctx, fd, "f"), 0);
	test_int_eq(th_sys(TAWC_SYS_renameat, AT_FDCWD, "/tmp/p", AT_FDCWD, "/tmp/p2", 0, 0), 0);
	make(test_ctx, &v, "/tmp/p/q", 1);
	make(test_ctx, &v, "/tmp/p/q/g", 0);
	test_int_eq(stat_at(test_ctx, fd, "f"), 0);
	test_int_eq(stat_at(test_ctx, fd, "g"), TAWC_ENOENT);
	close(fd);
	cache_off(store);
	th_teardown(&v);
}

test(hosted_fdpath_reused_fd_number_revalidates)
{
	th_view v;
	th_setup(&v, "fdpath-reuse");
	char store[4300];
	cache_on(&v, store, sizeof store);
	make(test_ctx, &v, "/tmp/one", 1);
	make(test_ctx, &v, "/tmp/one/f", 0);
	make(test_ctx, &v, "/tmp/two", 1);
	make(test_ctx, &v, "/tmp/two/g", 0);
	int fd = open_dir(test_ctx, "/tmp/one");
	test_int_eq(stat_at(test_ctx, fd, "f"), 0);
	close(fd);                                  /* untrapped in production */
	int again = open_dir(test_ctx, "/tmp/two");
	test_int_eq(again, fd);                     /* lowest free number */
	test_int_eq(stat_at(test_ctx, again, "g"), 0);
	test_int_eq(stat_at(test_ctx, again, "f"), TAWC_ENOENT);
	close(again);
	cache_off(store);
	th_teardown(&v);
}

/* One plain name under a known dirfd skips the root-based route
 * (syscalls_fs.c dirfd_leaf_shortcut). These cover what the shortcut
 * must still get right: bind mount points, `..` at the root, leaf
 * symlinks with absolute targets, and that it is actually taken. */
static long last_stat_dirfd;
static char last_stat_path[256];
static bool note_fstatat(long nr, const long args[6], long *ret)
{
	(void)ret;
	if (nr == TAWC_SYS_fstatat) {
		last_stat_dirfd = args[0];
		snprintf(last_stat_path, sizeof last_stat_path, "%s",
			 (const char *)args[1]);
	}
	return false;
}

static ino_t host_ino(const char *host)
{
	struct stat st;
	return stat(host, &st) == 0 ? st.st_ino : 0;
}

test(hosted_fdpath_leaf_shortcut_respects_binds_and_root)
{
	th_view v;
	th_setup(&v, "fdpath-leaf-bind");
	char store[4300], src[4300];
	snprintf(src, sizeof src, "%s", th_add_bind(&v, "/run"));
	cache_on(&v, store, sizeof store);
	int root = open_dir(test_ctx, "/");
	for (int pass = 0; pass < 2; pass++) {
		struct stat st;
		test_int_eq(th_sys(TAWC_SYS_fstatat, root, "run", &st,
				   AT_SYMLINK_NOFOLLOW, 0, 0), 0);
		test_int_eq((long)st.st_ino, (long)host_ino(src));
		for (int nofollow = 0; nofollow < 2; nofollow++) {
			test_int_eq(th_sys(TAWC_SYS_fstatat, root, "..", &st,
					   nofollow ? AT_SYMLINK_NOFOLLOW : 0,
					   0, 0), 0);
			test_int_eq((long)st.st_ino, (long)host_ino(v.root));
		}
	}
	int run = open_dir(test_ctx, "/run");
	test_int_eq(stat_at(test_ctx, run, "probe.txt"), 0);
	test_int_eq(stat_at(test_ctx, root, "etc"), 0);
	close(run);
	close(root);
	cache_off(store);
	th_teardown(&v);
}

test(hosted_fdpath_leaf_shortcut_follow)
{
	th_view v;
	th_setup(&v, "fdpath-leaf-follow");
	char store[4300], p[4300];
	cache_on(&v, store, sizeof store);
	snprintf(p, sizeof p, "%s/etc/abs", v.root);
	test_int_eq(symlink("/etc/probe", p), 0);
	snprintf(p, sizeof p, "%s/etc/probe", v.root);
	ino_t probe = host_ino(p);
	int etc = open_dir(test_ctx, "/etc");
	tawcroot_test_raw_hook = note_fstatat;
	for (int pass = 0; pass < 3; pass++) {
		struct stat st;
		test_int_eq(th_sys(TAWC_SYS_fstatat, etc, "abs", &st, 0, 0, 0), 0);
		test_int_eq((long)st.st_ino, (long)probe);
		test_int_eq(th_sys(TAWC_SYS_fstatat, etc, "probe", &st, 0, 0, 0), 0);
		test_int_eq((long)st.st_ino, (long)probe);
	}
	/* Warm by now: the plain leaf went straight to the dirfd. */
	test_int_eq(last_stat_dirfd, etc);
	test_str_eq(last_stat_path, "probe");
	tawcroot_test_raw_hook = NULL;
	close(etc);
	cache_off(store);
	th_teardown(&v);
}
