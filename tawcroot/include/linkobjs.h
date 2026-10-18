/* Shared set of link-store object inode numbers (linkstore.h).
 *
 * fstat-family handlers must report an emulated hardlink's count when
 * the fd refers to a store OBJECT, which used to cost a /proc/self/fd
 * readlink for every regular file with nlink 1 on the store's
 * filesystem. Objects only come into being when tawcroot_link_new
 * renames a file into `<store>/link/`, so a set of their inode numbers
 * answers "definitely not an object" from memory.
 *
 * `<store>/objcache-<ino of link/>` is a MAP_SHARED open-addressing table of inode
 * numbers. link_new adds the inode before the rename that makes it an
 * object; the first mapper scans `link/` once to cover objects made
 * before the set existed, and until that scan has finished every query
 * answers "maybe". Entries are never removed (a deleted object's inode
 * only costs a needless probe), and a lookup that runs off its probe
 * window also answers "maybe", so the set can only ever err toward the
 * readlink. Any setup failure leaves it answering "maybe".
 */

#pragma once

#include <stdint.h>

/* Map the set for this process (the store calls this lazily, through
 * tawcroot_linkstore_objs_init, on the first add or query),
 * scanning `link_dirfd` if no process has populated it yet. The file
 * lives in the store root and is named after link/'s inode, so a copied
 * or re-imported store never inherits a set built for other inodes. */
void tawcroot_linkobjs_init(int store_dirfd, int link_dirfd);

/* Forget this process's mapping (store reconfigured). */
void tawcroot_linkobjs_reset(void);

/* Top-level start: delete sets named after an earlier link/. */
void tawcroot_linkobjs_sweep(int store_dirfd, int link_dirfd);

/* Record that `ino` is (about to be) a store object. */
void tawcroot_linkobjs_add(uint64_t ino);

/* 0 when `ino` is certainly not a store object; 1 otherwise. */
int tawcroot_linkobjs_maybe(uint64_t ino);
