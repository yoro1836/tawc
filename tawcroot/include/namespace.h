/* Startup compatibility only; Android UID/SELinux remain the security boundary. */
#pragma once
#include <stdint.h>
struct tawc_namespace {
    int32_t init_pid;
    uint32_t user;
    uint32_t capabilities[6];
    int32_t maps[3];
    uint32_t session;  /* TAWC_NS_* flags */
    uint32_t capbset_dropped[2];  /* --init: emulated bounding set */
};
#define TAWC_NS_INIT_SESSION 1u
#define TAWC_NS_CGROUP 2u        /* --cgroup: fake cgroupfs (cgroup.h) */
#define TAWC_NS_CG_WRITE_TRAP 4u /* write() on fds >= the reserved floor traps */
extern struct tawc_namespace tawcroot_namespace;
void tawcroot_namespace_register(void);
void tawcroot_namespace_restore(void);
/* `--init`: the session's first guest process is pid 1. */
long tawcroot_namespace_init_session(void);
long tawcroot_namespace_open(const char *path, int flags);
/* /proc/<pid>/stat|status with the fake init as pid 1, or TAWC_ENOSYS. */
long tawcroot_namespace_proc_open(const char *path, int flags);
/* --init capability prctls (bounding/ambient sets, securebits), or TAWC_ENOSYS. */
long tawcroot_namespace_prctl(long option, long arg);
/* In a child of the init: SIGKILL it when the init dies. */
void tawcroot_namespace_tie_to_init(void);
int tawcroot_namespace_probe(const char *path);
