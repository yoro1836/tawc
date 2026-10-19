/* Emulated mounts for `--init` sessions (notes/tawcroot/systemd.md). */
#pragma once
#include "dispatch.h"

/* Init-time: an empty shared mount table for the session. 0 or -errno. */
long tawcroot_mounts_setup(void);
/* --exec-child: map the session's table again. */
void tawcroot_mounts_attach(void);
/* Mirror the shared table into this process's bind table if it moved.
 * Cheap when it has not; called on every path translation. */
void tawcroot_mounts_sync(void);

/* mount(2)/umount2(2) in an init session. Guest pointers. */
long tawcroot_mounts_mount(const char *source, const char *target,
			   const char *type, unsigned long flags,
			   const char *data);
long tawcroot_mounts_umount(const char *target, int flags);

/* /proc/<pid>/mountinfo|mounts and /proc/mounts: a synthesized fd, or
 * TAWC_ENOSYS. */
long tawcroot_mounts_proc_open(const char *path, int flags);

/* statfs f_type for a path routed through bind src `fd`, or 0. */
long tawcroot_mounts_magic(int fd);

/* epoll_ctl on a synthesized mountinfo fd: registers a watch on the
 * session's mount-change events instead (lazy; fds >= the floor). */
long tawcroot_mounts_epoll_ctl(const tawcroot_syscall_args *a, ucontext_t *uc);

/* fstatfs in init sessions (lazy). */
long tawcroot_mounts_fstatfs(const tawcroot_syscall_args *a, ucontext_t *uc);
