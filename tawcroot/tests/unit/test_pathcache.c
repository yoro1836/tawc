/* Unit tests for the shared not-a-symlink cache (src/pathcache.c).
 *
 * The contract under test (include/pathcache.h): a noted prefix is
 * served until it, or any of its own prefixes, is reported mutated
 * after the note's epoch; a session start drops everything; keys are
 * per host root; a fact observed before a mutation can never be
 * stamped valid after it. Each test maps a fresh cache file and turns
 * the cache off again before returning so the rest of the hosted
 * suite never sees it.
 */

#include <cleat/test.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "path.h"
#include "pathcache.h"

#ifndef TAWCROOT_TEST_TMPDIR
# define TAWCROOT_TEST_TMPDIR "/tmp"
#endif

#define STORE TAWCROOT_TEST_TMPDIR "/tawcroot-test-pathcache-store"
#define ROOT_FD 100

static int    saved_fd;
static size_t saved_len;
static char   saved_path[sizeof tawcroot_rootfs_host_path];

static void set_root(const char *host)
{
	tawcroot_rootfs_fd = ROOT_FD;
	snprintf(tawcroot_rootfs_host_path, sizeof tawcroot_rootfs_host_path,
		 "%s", host);
	tawcroot_rootfs_host_path_len = strlen(host);
}

static void begin(void)
{
	saved_fd = tawcroot_rootfs_fd;
	saved_len = tawcroot_rootfs_host_path_len;
	memcpy(saved_path, tawcroot_rootfs_host_path, sizeof saved_path);
	unlink(STORE "-pathcache");
	set_root("/data/distros/a/rootfs");
	tawcroot_pathcache_init(STORE);
}

static void end(void)
{
	tawcroot_pathcache_init(NULL);
	unlink(STORE "-pathcache");
	tawcroot_rootfs_fd = saved_fd;
	tawcroot_rootfs_host_path_len = saved_len;
	memcpy(tawcroot_rootfs_host_path, saved_path, sizeof saved_path);
}

test(pathcache_noted_prefix_is_served)
{
	begin();
	test_true(tawcroot_pathcache_epoch() != 0);
	test_false(tawcroot_pathcache_known_plain("usr/lib"));
	tawcroot_pathcache_note_plain("usr/lib", tawcroot_pathcache_epoch());
	test_true(tawcroot_pathcache_known_plain("usr/lib"));
	test_false(tawcroot_pathcache_known_plain("usr/li"));
	test_false(tawcroot_pathcache_known_plain("usr/lib/"));
	end();
}

test(pathcache_mutation_invalidates_name_and_descendants)
{
	begin();
	uint64_t e = tawcroot_pathcache_epoch();
	tawcroot_pathcache_note_plain("usr", e);
	tawcroot_pathcache_note_plain("usr/lib", e);
	tawcroot_pathcache_note_plain("usr/lib/x.so", e);
	tawcroot_pathcache_note_plain("etc", e);

	tawcroot_pathcache_mutated(ROOT_FD, "usr/lib/");  /* rmdir form */
	test_true(tawcroot_pathcache_known_plain("usr"));
	test_false(tawcroot_pathcache_known_plain("usr/lib"));
	test_false(tawcroot_pathcache_known_plain("usr/lib/x.so"));
	test_true(tawcroot_pathcache_known_plain("etc"));

	/* Re-observed after the mutation: valid again. */
	tawcroot_pathcache_note_plain("usr/lib", tawcroot_pathcache_epoch());
	test_true(tawcroot_pathcache_known_plain("usr/lib"));
	end();
}

test(pathcache_fact_observed_before_mutation_never_valid)
{
	/* A reader reads the epoch, probes, and only then inserts — with a
	 * mutation landing in between. Its stale fact must not be served. */
	begin();
	uint64_t before = tawcroot_pathcache_epoch();
	tawcroot_pathcache_mutated(ROOT_FD, "opt/app");
	tawcroot_pathcache_note_plain("opt/app", before);
	test_false(tawcroot_pathcache_known_plain("opt/app"));
	end();
}

test(pathcache_session_start_drops_everything)
{
	begin();
	tawcroot_pathcache_note_plain("usr/lib", tawcroot_pathcache_epoch());
	tawcroot_pathcache_session_start();
	test_false(tawcroot_pathcache_known_plain("usr/lib"));
	end();
}

test(pathcache_other_bases_ignored_root_flushes)
{
	begin();
	tawcroot_pathcache_note_plain("usr/lib", tawcroot_pathcache_epoch());
	tawcroot_pathcache_mutated(ROOT_FD + 1, "usr/lib");   /* a bind src */
	test_true(tawcroot_pathcache_known_plain("usr/lib"));
	tawcroot_pathcache_mutated(ROOT_FD, "");               /* the root */
	test_false(tawcroot_pathcache_known_plain("usr/lib"));
	end();
}

test(pathcache_keys_follow_host_root)
{
	/* Same suffix under a chroot is a different object... */
	begin();
	tawcroot_pathcache_note_plain("usr/lib", tawcroot_pathcache_epoch());
	set_root("/data/distros/a/rootfs/sub");
	tawcroot_rootfs_fd = ROOT_FD + 2;
	test_false(tawcroot_pathcache_known_plain("usr/lib"));
	/* ...and the same object seen from the chroot shares the key, so
	 * a mutation inside it invalidates the outer view's entry. */
	set_root("/data/distros/a/rootfs");
	tawcroot_pathcache_note_plain("sub/usr", tawcroot_pathcache_epoch());
	set_root("/data/distros/a/rootfs/sub");
	tawcroot_rootfs_fd = ROOT_FD + 2;
	test_true(tawcroot_pathcache_known_plain("usr"));
	tawcroot_pathcache_mutated(ROOT_FD + 2, "usr");
	set_root("/data/distros/a/rootfs");
	test_false(tawcroot_pathcache_known_plain("sub/usr"));
	end();
}

test(pathcache_shared_between_mappings)
{
	/* A second mapping of the same file (another process) sees notes
	 * and mutations made through the first. */
	begin();
	tawcroot_pathcache_note_plain("var/lib", tawcroot_pathcache_epoch());
	tawcroot_pathcache_init(STORE);   /* remap: as a fresh process would */
	test_true(tawcroot_pathcache_known_plain("var/lib"));
	end();
}

/* A resolver pass probes growing prefixes of one buffer; the walk API
 * extends the previous hash. It must agree with the one-shot API on
 * every prefix, in both directions, and see mutations of any prefix. */
test(pathcache_walk_matches_one_shot)
{
	begin();
	char buf[] = "usr/lib/python3/site/x.py";
	size_t ends[] = { 3, 7, 15, 20, sizeof buf - 1 };
	uint64_t e = tawcroot_pathcache_epoch();
	struct tawcroot_pathcache_walk w = { 0 };
	for (size_t i = 0; i < 5; i += 2) {
		buf[ends[i]] = 0;
		tawcroot_pathcache_note_plain(buf, e);
		buf[ends[i]] = i + 1 < 5 ? '/' : 0;
	}
	for (size_t i = 1; i < 5; i += 2)
		tawcroot_pathcache_walk_note(&w, buf, ends[i], e);
	for (size_t i = 0; i < 5; i++) {
		char saved = buf[ends[i]];
		buf[ends[i]] = 0;
		test_true(tawcroot_pathcache_known_plain(buf));
		buf[ends[i]] = saved;
	}
	w.base = 0;
	for (size_t i = 0; i < 5; i++)
		test_true(tawcroot_pathcache_walk_plain(&w, buf, ends[i]));

	buf[7] = 0;
	tawcroot_pathcache_mutated(ROOT_FD, buf);
	buf[7] = '/';
	w.base = 0;
	test_true(tawcroot_pathcache_walk_plain(&w, buf, ends[0]));
	for (size_t i = 1; i < 5; i++)
		test_false(tawcroot_pathcache_walk_plain(&w, buf, ends[i]));
	/* Probes that skip the mutated prefix must still see it, whether
	 * they extend an earlier probe or start fresh. */
	w.base = 0;
	test_true(tawcroot_pathcache_walk_plain(&w, buf, ends[0]));
	test_false(tawcroot_pathcache_walk_plain(&w, buf, ends[3]));
	w.base = 0;
	test_false(tawcroot_pathcache_walk_plain(&w, buf, ends[4]));
	end();
}
