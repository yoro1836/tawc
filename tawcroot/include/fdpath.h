/* Per-process cache of fd -> guest absolute path.
 *
 * Lifting a dirfd-relative path to guest-absolute (translate_local) reads
 * /proc/self/fd/N for every *at call with a real dirfd: find/tar/grep issue
 * thousands. This caches the answer per fd number and revalidates on use:
 *
 *   - fstat(fd) must still report the cached dev/ino, so a closed and
 *     reused fd number (close is untrapped below the reserved range), a
 *     dup2 over it, or a vfork child's divergent table never serves a
 *     stale path;
 *   - the shared path cache must report no mutation of the path or any
 *     ancestor since the epoch read before the readlink (rename/rmdir of
 *     the directory, or of a parent, while the fd stays open);
 *   - the root view must be unchanged (chroot).
 *
 * Only paths that lie plainly in the rootfs are cached: the path cache
 * only stamps rootfs names, so bind-routed fds always take the readlink.
 * With the path cache off nothing is cached. Handler-safe: fixed table,
 * per-entry seqlock, raw syscalls only.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* Copy the cached guest path of `fd` into `out`. Returns its length, or
 * -1 on a miss; on a miss the fd's identity and the epoch to fill with
 * are left in `*probe` for tawcroot_fdpath_fill. */
struct tawcroot_fdpath_probe {
	uint64_t dev, ino, epoch;
	int      valid;
};
long tawcroot_fdpath_lookup(int fd, char *out, size_t out_cap,
			    struct tawcroot_fdpath_probe *probe);

/* Record `guest` (absolute, `len` bytes) for `fd`, as observed after
 * `probe` was taken. `host`/`host_len` is the kernel path it came from;
 * fills only when that is the rootfs host path plus the same suffix. */
void tawcroot_fdpath_fill(int fd, const struct tawcroot_fdpath_probe *probe,
			  const char *guest, size_t len,
			  const char *host, size_t host_len);
