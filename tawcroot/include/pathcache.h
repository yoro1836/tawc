/* Shared "this rootfs prefix is not a symlink" cache.
 *
 * The symlink resolver (path_resolve.c) asks readlinkat about every
 * prefix of every path, one syscall per component; on Android each
 * costs more than the work the guest asked for. This cache lets the
 * production oracle answer "not a symlink" from memory instead.
 *
 * Shared by every tawcroot process of a distro through a MAP_SHARED
 * file beside the link store (`<rootfs>/../tawcroot-pathcache`), so a
 * rename in one session invalidates lookups in all of them.
 *
 * Only one fact is cached: readlinkat(prefix) == -EINVAL, i.e. the
 * name exists and is not a symlink. That fact goes stale only when the
 * name, or one of its ancestors, is unlinked, renamed over/away or
 * turned into an emulated hardlink. Those handlers report the name
 * AFTER the host call (tawcroot_pathcache_mutated) and stamp it with
 * a fresh epoch; a cached prefix is valid only while every one of its
 * own prefixes carries an older stamp than the entry. Entries record
 * the epoch read before their readlinkat, so a fact observed before a
 * mutation finished can never survive it.
 *
 * Keys are hashes of the canonical HOST path (rootfs host path + '/' +
 * suffix), so a guest chroot sees the same keys for the same objects.
 * Bind routes are never cached.
 *
 * Changes made outside tawcroot (the app's own installers) are not
 * seen while a session runs; every top-level tawcroot start therefore
 * invalidates the whole cache (tawcroot_pathcache_session_start).
 *
 * Handler-safe: raw syscalls only at init, lock-free per-slot seqlocks
 * across processes afterwards. Any failure leaves the cache off.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* Map the cache for this process. `store_host_path` is the link-store
 * path (supervisor args); NULL disables the cache. */
void tawcroot_pathcache_init(const char *store_host_path);

/* Top-level entry only: invalidate everything cached so far. */
void tawcroot_pathcache_session_start(void);

/* Epoch to stamp an insert with. Read it BEFORE the readlinkat whose
 * result is being cached. 0 means "cache off". */
uint64_t tawcroot_pathcache_epoch(void);

/* 1 when rootfs-relative `suffix` (no leading '/') is known to exist
 * and not be a symlink. */
int tawcroot_pathcache_known_plain(const char *suffix);

/* 1 when rootfs-relative `suffix` ("" = the root) and every prefix of
 * it have had no reported mutation since `epoch` (read before the
 * observation being validated) and no invalidation since. 0 when the
 * cache is off. Lets other caches key facts on a guest path. */
int tawcroot_pathcache_unchanged_since(const char *suffix, uint64_t epoch);

/* Record readlinkat(rootfs_fd, suffix) == -EINVAL, observed at `epoch`. */
void tawcroot_pathcache_note_plain(const char *suffix, uint64_t epoch);

/* known_plain/note_plain for a resolver pass (path_oracle.h
 * walk_begin): probes of growing prefixes of one buffer extend the
 * previous probe's hash instead of rehashing the path from the start.
 * Zero the walk at the start of each pass. Prefix stamps are read when
 * first hashed, so a mutation racing the pass may go unseen for the
 * rest of it, like a native walk racing a rename. */
struct tawcroot_pathcache_walk {
	const char *base;      /* NULL: nothing carried */
	size_t      len;
	uint64_t    h1, h2, newest;
};
int tawcroot_pathcache_walk_plain(struct tawcroot_pathcache_walk *w,
				  const char *suffix, size_t len);
void tawcroot_pathcache_walk_note(struct tawcroot_pathcache_walk *w,
				  const char *suffix, size_t len,
				  uint64_t epoch);

/* A namespace mutation of (base_fd, suffix) has just been attempted.
 * Only rootfs-based names matter; anything else is ignored. */
void tawcroot_pathcache_mutated(int base_fd, const char *suffix);
