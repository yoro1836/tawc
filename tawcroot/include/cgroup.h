/* Fake cgroup v2 for `--cgroup` sessions (notes/tawcroot/systemd.md). */
#pragma once
#include <stddef.h>

/* Init-time: back /sys/fs/cgroup with an empty <store>/cgroup tree and
 * put the calling process in the root cgroup. 0 or -errno. */
long tawcroot_cgroup_setup(void);
long tawcroot_cgroup_join(void);

/* Whether (fd, suffix), as translated for a syscall, lies in the fake
 * cgroupfs. Writes its cgroup-relative path ("" for the root) to `rel`. */
int tawcroot_cgroup_locate(int fd, const char *suffix, char *rel, size_t cap);

/* Membership bookkeeping: a forked child joins its parent's cgroup; an
 * exec'd image registers itself if it never went through a trapped fork
 * (vfork, posix_spawn). No-ops outside a --cgroup session. */
void tawcroot_cgroup_fork_child(long parent);
void tawcroot_cgroup_exec(void);

/* Hooks for the cgroupfs paths. TAWC_ENOSYS means "not a cgroup
 * special case, carry on normally". `rel` comes from _locate. */
void tawcroot_cgroup_mkdir(int fd, const char *path, const char *rel);
long tawcroot_cgroup_rmdir(int fd, const char *path, const char *rel);
long tawcroot_cgroup_open(int fd, const char *path, int flags, const char *rel);
long tawcroot_cgroup_write(int fd, const void *buf, size_t len);

/* /proc/self/cgroup, /proc/<pid>/cgroup: a synthesized fd, or TAWC_ENOSYS. */
long tawcroot_cgroup_proc_open(const char *path, int flags);

/* Empty a host directory (setup-time; not thread-safe). */
void tawcroot_wipe_dir(int dirfd);

#define TAWCROOT_CGROUP2_SUPER_MAGIC 0x63677270
