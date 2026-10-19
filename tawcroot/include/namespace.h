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
#define TAWC_NS_EPOLL_TRAP 8u
#define TAWC_NS_RECV_TRAP 16u     /* recv* on fds >= the floor trap (ports.c) */    /* epoll_ctl() on fds >= the floor traps (mounts.c) */
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
/* An init session presents as a user-namespaced container: stat of
 * /proc/<pid>/ns/user reports this inode instead of the initial
 * namespace's (which inits compare against). */
#define TAWCROOT_FAKE_USERNS_INO 0xeffffff0u
int tawcroot_namespace_is_userns_link(const char *path);

/* Whether `pid` is the fake init or a descendant (1 without an init). */
int tawcroot_namespace_is_guest(long pid);
/* In a child of the init: SIGKILL it when the init dies. */
void tawcroot_namespace_tie_to_init(void);
/* Init sessions: per-process virtual ids for SO_PEERCRED. */
void tawcroot_namespace_ids_setup(void);
void tawcroot_namespace_publish_ids(uint32_t uid, uint32_t gid);
/* 1 with `pid`'s virtual uid/gid filled in, 0 outside init sessions. */
int tawcroot_namespace_peer_ids(long pid, uint32_t *uid, uint32_t *gid);
int tawcroot_namespace_probe(const char *path);
