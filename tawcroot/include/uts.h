/* Emulated hostname for `--init` sessions (notes/tawcroot/systemd.md). */
#pragma once
#include "dispatch.h"

/* Init-time: seed the session's names from the kernel's. */
void tawcroot_uts_setup(void);
/* uname, sethostname, setdomainname (lazy, init sessions). */
long tawcroot_uts_syscall(const tawcroot_syscall_args *a, ucontext_t *uc);
/* /proc/sys/kernel/hostname|domainname: a memfd, or TAWC_ENOSYS. */
long tawcroot_uts_proc_open(const char *path, int flags);
