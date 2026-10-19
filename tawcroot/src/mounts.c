/* Emulated mounts for `--init` sessions. Android gives the app no
 * mount(2), but an init mounts tmpfs on /tmp and friends, moves and
 * binds mounts, and reads /proc/self/mountinfo to see that it worked.
 *
 *   - tmpfs/ramfs mounts get a fresh backing directory under
 *     <store>/mnt; bind mounts point at their source's host path.
 *   - Each mount is an entry in <store>/mounts, a table shared by the
 *     whole session and bumped by a generation counter. Every process
 *     mirrors it into its bind table (mount_id != 0) on the next path
 *     translation after the generation moves.
 *   - /proc/<pid>/mountinfo and mounts are synthesized from the view.
 *
 * Filesystems we cannot emulate stay EPERM, as for an unprivileged
 * container. See notes/tawcroot/systemd.md. */
#include <stdint.h>
#include <fcntl.h>
#include <asm/unistd.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include "mounts.h"
#include "cgroup.h"
#include "errno_neg.h"
#include "filter.h"
#include "fdtab.h"
#include "io.h"
#include "linkstore.h"
#include "namespace.h"
#include "path.h"
#include "path_scratch.h"
#include "raw_sys.h"
#include "sysnr.h"
#include "tawc_string.h"
#include "usercopy.h"

#define MAX_MOUNTS 64
#define MAGIC 0x4d4e5431
#define TMPFS_MAGIC 0x01021994
#define RAMFS_MAGIC 0x858458f6
#define IN_NONBLOCK_FLAG 04000
#define MADV_WIPEONFORK_ 18

struct entry {
    uint32_t id, ro;
    char dst[256];      /* folded, no leading '/', like a bind's dst */
    char src[256];      /* host path */
    char type[32];
    char source[64];
};
struct table {
    uint32_t magic, gen, next_id, pad;
    struct entry e[MAX_MOUNTS];
};

static struct table *g_tab;
/* Pid whose bind table this memory is. A MADV_WIPEONFORK page: a fork
 * child reads 0 and takes over its private copy, while a CLONE_VM child
 * (vfork, posix_spawn) still reads its parent's pid. */
static volatile long *g_owner;
static uint32_t g_seen;
static int g_syncing, g_mounting;

static int session(void)
{
    return (tawcroot_namespace.session & TAWC_NS_INIT_SESSION) != 0;
}

static long store_path(char *out, const char *name)
{
    size_t n = 0;
    if (!tawcroot_store_host_path_len) return TAWC_ENOENT;
    if (tawc_str_append(out, TAWCROOT_PATH_SCRATCH_SIZE, &n, tawcroot_store_host_path) ||
        tawc_str_append(out, TAWCROOT_PATH_SCRATCH_SIZE, &n, name))
        return TAWC_ENAMETOOLONG;
    return 0;
}

/* A fresh descriptor per lock: flock is per open file description, and
 * forked processes share ours. Returns the fd (locked) or -errno. */
static long lock(int op)
{
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    char *path = scratch->buf[0];
    long e = store_path(path, "/mounts");
    if (e < 0) return e;
    long fd = tawc_openat(AT_FDCWD, path, O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0);
    if (fd < 0) return fd;
    e = TAWC_RAW(__NR_flock, fd, op, 0, 0, 0, 0);
    if (e < 0) { tawc_close((int)fd); return e; }
    return fd;
}

static void notify(void);

static void bump(void)
{
    __atomic_add_fetch(&g_tab->gen, 1, __ATOMIC_RELEASE);
}

static long map_table(int create)
{
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    char *path = scratch->buf[0];
    long fd = store_path(path, "/mounts");
    if (fd < 0) return fd;
    fd = tawc_openat(AT_FDCWD, path, O_RDWR | O_CLOEXEC | O_NOFOLLOW | (create ? O_CREAT : 0), 0600);
    if (fd < 0) return fd;
    long e = create ? TAWC_RAW(TAWC_SYS_ftruncate, fd, sizeof(struct table), 0, 0, 0, 0) : 0;
    long p = e ? e : tawc_mmap(0, sizeof(struct table), PROT_READ | PROT_WRITE, MAP_SHARED, (int)fd, 0);
    tawc_close((int)fd);
    if (p < 0 && p >= -4095) return p;
    g_tab = (struct table *)p;
    if (!g_owner) {
        long m = tawc_mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m >= 0 || m < -4095) {
            if (TAWC_RAW(TAWC_SYS_madvise, m, 4096, MADV_WIPEONFORK_, 0, 0, 0) < 0)
                tawc_munmap((void *)m, 4096);
            else
                g_owner = (volatile long *)m;
        }
    }
    if (g_owner) *g_owner = TAWC_RAW(TAWC_SYS_getpid, 0, 0, 0, 0, 0, 0);
    return 0;
}

/* Whether this process may rewrite the bind table. A CLONE_VM child
 * shares its parent's table but not its fds: binds it opened would
 * name fds the parent doesn't have. It routes through the parent's
 * view (its fd table is a copy) until exec gives it its own. */
static int own_view(void)
{
    if (!g_owner) return 0;
    long pid = TAWC_RAW(TAWC_SYS_getpid, 0, 0, 0, 0, 0, 0);
    if (!*g_owner) *g_owner = pid;
    return *g_owner == pid;
}

long tawcroot_mounts_setup(void)
{
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    char *path = scratch->buf[1];
    long e = store_path(path, "/mnt");
    if (e < 0) return e;
    (void)TAWC_RAW(TAWC_SYS_mkdirat, AT_FDCWD, (long)tawcroot_store_host_path, 0700, 0, 0, 0);
    (void)TAWC_RAW(TAWC_SYS_mkdirat, AT_FDCWD, (long)path, 0700, 0, 0, 0);
    long dir = tawc_openat(AT_FDCWD, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, 0);
    if (dir < 0) return dir;
    tawcroot_wipe_dir((int)dir);
    tawc_close((int)dir);
    e = map_table(1);
    if (e) return e;
    long fd = lock(2 /*LOCK_EX*/);
    if (fd < 0) return fd;
    memset(g_tab, 0, sizeof *g_tab);
    g_tab->magic = MAGIC;
    g_tab->next_id = 1;
    g_tab->gen = 1;
    g_seen = 1;
    tawc_close((int)fd);
    notify();
    return 0;
}

void tawcroot_mounts_attach(void)
{
    if (!session() || map_table(0)) return;
    if (g_tab->magic != MAGIC) {
        tawc_munmap(g_tab, sizeof *g_tab);
        g_tab = 0;
    }
    g_seen = 0;
}

/* --- mirroring ---------------------------------------------------------- */

static const struct entry *find_id(uint32_t id)
{
    for (int i = 0; i < MAX_MOUNTS; ++i)
        if (g_tab->e[i].id == id) return &g_tab->e[i];
    return 0;
}

static void drop(struct tawcroot_bind *b)
{
    __atomic_store_n(&b->active, 0, __ATOMIC_RELEASE);
    tawcroot_fd_forget_reserved(b->src_fd);
    tawc_close(b->src_fd);
    b->src_fd = -1;
}

static void add(const struct entry *e)
{
    struct tawcroot_bind *b = 0;
    for (size_t i = 0; i < tawcroot_n_binds && !b; ++i)
        if (tawcroot_binds[i].mount_id && !tawcroot_binds[i].active) b = &tawcroot_binds[i];
    if (b) {
        if (!tawcroot_path_fill_bind(b, e->src, e->dst, (int)e->ro)) b->mount_id = e->id;
        return;
    }
    if (tawcroot_n_binds >= TAWCROOT_MAX_BINDS) return;
    b = &tawcroot_binds[tawcroot_n_binds];
    b->mount_id = e->id;
    if (!tawcroot_path_fill_bind(b, e->src, e->dst, (int)e->ro))
        __atomic_store_n(&tawcroot_n_binds, tawcroot_n_binds + 1, __ATOMIC_RELEASE);
}

void tawcroot_mounts_sync(void)
{
    if (!g_tab || __atomic_load_n(&g_tab->gen, __ATOMIC_ACQUIRE) == g_seen) return;
    /* A mount op in flight holds the table lock; its own translations
     * use the view it synced before taking it. */
    if (__atomic_load_n(&g_mounting, __ATOMIC_ACQUIRE)) return;
    /* One mirroring thread at a time; the others route through the
     * previous view until it is done. */
    if (!own_view()) return;
    if (__atomic_exchange_n(&g_syncing, 1, __ATOMIC_ACQUIRE)) return;
    long fd = lock(1 /*LOCK_SH*/);
    if (fd >= 0) {
        uint32_t gen = g_tab->gen;
        for (size_t i = 0; i < tawcroot_n_binds; ++i) {
            struct tawcroot_bind *b = &tawcroot_binds[i];
            if (!b->mount_id || !b->active) continue;
            const struct entry *e = find_id(b->mount_id);
            if (!e || !tawc_streq(e->dst, b->dst) || e->ro != (uint32_t)b->read_only) drop(b);
        }
        for (int i = 0; i < MAX_MOUNTS; ++i) {
            const struct entry *e = &g_tab->e[i];
            if (!e->id) continue;
            int have = 0;
            for (size_t j = 0; j < tawcroot_n_binds && !have; ++j)
                have = tawcroot_binds[j].active && tawcroot_binds[j].mount_id == e->id;
            if (!have) add(e);
        }
        g_seen = gen;
        tawc_close((int)fd);
    }
    __atomic_store_n(&g_syncing, 0, __ATOMIC_RELEASE);
}

/* --- mount(2) ----------------------------------------------------------- */

/* Guest path of an existing directory (or, with `file_ok`, file), as a
 * folded absolute path in `out`. */
static long guest_target(const char *gpath, char *out, int file_ok)
{
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    char *name = scratch->buf[0], *suffix = scratch->buf[1];
    long e = tawc_copy_string_from_guest(name, TAWCROOT_PATH_SCRATCH_SIZE, gpath);
    if (e < 0) return e;
    tawcroot_path_result r = tawcroot_path_translate(name, suffix, TAWCROOT_PATH_SCRATCH_SIZE,
        TAWCROOT_PATH_FOLLOW, TAWCROOT_PATH_INTENT_READ);
    if (r.err) return r.err;
    long fd = tawc_openat(r.base_fd, *suffix ? suffix : ".",
        O_PATH | O_CLOEXEC | (file_ok ? 0 : O_DIRECTORY), 0);
    if (fd < 0) return fd;
    /* By route, not by fd: two mounts may share one host source. But
     * a /proc/self/fd/N target (systemd mounts onto O_PATH fds) names
     * whatever that fd is. */
    e = tawcroot_path_route_to_guest(r.base_fd, suffix, out, TAWCROOT_PATH_SCRATCH_SIZE);
    if (e >= 0 && tawc_starts_with(out, "/proc/"))
        e = tawcroot_fd_to_guest_abs((int)fd, out, TAWCROOT_PATH_SCRATCH_SIZE);
    tawc_close((int)fd);
    return e < 0 ? e : 0;
}

/* A missing mount point whose parent directory exists: emulated
 * filesystems can still be mounted there (e.g. /dev/mqueue under a
 * host-bound /dev the guest cannot mkdir in). */
static long virtual_target(const char *gpath, char *out)
{
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    char *name = scratch->buf[0], *suffix = scratch->buf[1];
    long e = tawc_copy_string_from_guest(name, TAWCROOT_PATH_SCRATCH_SIZE, gpath);
    if (e < 0) return e;
    tawcroot_path_result r = tawcroot_path_translate(name, suffix, TAWCROOT_PATH_SCRATCH_SIZE,
        TAWCROOT_PATH_NOFOLLOW, TAWCROOT_PATH_INTENT_READ);
    if (r.err) return r.err;
    size_t n = tawc_strlen(suffix);
    if (!n) return TAWC_ENOENT;
    while (n && suffix[n - 1] != '/') --n;
    char saved = suffix[n ? n - 1 : 0];
    if (n) suffix[n - 1] = 0;
    long fd = tawc_openat(r.base_fd, n ? suffix : ".", O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
    if (n) suffix[n - 1] = saved;
    if (fd < 0) return TAWC_ENOENT;
    tawc_close((int)fd);
    e = tawcroot_path_route_to_guest(r.base_fd, suffix, out, TAWCROOT_PATH_SCRATCH_SIZE);
    return e < 0 ? e : 0;
}

/* Host path of guest `gpath` (must exist). */
static long host_source(const char *gpath, char *out)
{
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    char *name = scratch->buf[0], *suffix = scratch->buf[1];
    long e = tawc_copy_string_from_guest(name, TAWCROOT_PATH_SCRATCH_SIZE, gpath);
    if (e < 0) return e;
    tawcroot_path_result r = tawcroot_path_translate(name, suffix, TAWCROOT_PATH_SCRATCH_SIZE,
        TAWCROOT_PATH_FOLLOW, TAWCROOT_PATH_INTENT_READ);
    if (r.err) return r.err;
    long fd = tawc_openat(r.base_fd, *suffix ? suffix : ".", O_PATH | O_CLOEXEC, 0);
    if (fd < 0) return fd;
    e = tawcroot_proc_fd_to_host_path((int)fd, out, TAWCROOT_PATH_SCRATCH_SIZE);
    tawc_close((int)fd);
    return e < 0 ? e : 0;
}

/* Latest entry mounted at guest path `abs` ("/x"), or -1. */
static int at(const char *abs)
{
    int hit = -1;
    for (int i = 0; i < MAX_MOUNTS; ++i)
        if (g_tab->e[i].id && tawc_streq(g_tab->e[i].dst, abs + 1) &&
            (hit < 0 || g_tab->e[i].id > g_tab->e[hit].id)) hit = i;
    return hit;
}

static long add_entry(const char *abs, const char *src, const char *type,
                      const char *source, int ro, uint32_t id)
{
    if (!abs[1]) return TAWC_EBUSY;   /* over the root: not emulated */
    for (int i = 0; i < MAX_MOUNTS; ++i) {
        struct entry *e = &g_tab->e[i];
        if (e->id) continue;
        if (tawc_str_copy(e->dst, sizeof e->dst, abs + 1) < 0 ||
            tawc_str_copy(e->src, sizeof e->src, src) < 0) return TAWC_ENAMETOOLONG;
        (void)tawc_str_copy(e->type, sizeof e->type, type);
        if (tawc_str_copy(e->source, sizeof e->source, source) < 0)
            (void)tawc_str_copy(e->source, sizeof e->source, type);
        e->ro = ro ? 1 : 0;
        e->id = id ? id : g_tab->next_id++;
        bump();
        return 0;
    }
    return TAWC_ENOSPC;
}

/* <store>/mnt/<id>, created with tmpfs's `mode=` (default 1777). */
static long new_backing(char *out, uint32_t id, const char *data)
{
    size_t n = 0;
    if (store_path(out, "/mnt/") < 0) return TAWC_ENOENT;
    n = tawc_strlen(out);
    if (tawc_str_append_dec(out, TAWCROOT_PATH_SCRATCH_SIZE, &n, id)) return TAWC_ENAMETOOLONG;
    long mode = 01777;
    for (const char *p = data; p && *p; ) {
        if (tawc_starts_with(p, "mode=")) {
            mode = 0;
            for (p += 5; *p >= '0' && *p <= '7'; ++p) mode = mode * 8 + (*p - '0');
        }
        while (*p && *p != ',') ++p;
        if (*p) ++p;
    }
    long e = TAWC_RAW(TAWC_SYS_mkdirat, AT_FDCWD, (long)out, 0700, 0, 0, 0);
    if (e < 0) return e;
    return TAWC_RAW(TAWC_SYS_fchmodat, AT_FDCWD, (long)out, mode & 07777, 0, 0, 0);
}

static long do_mount(const char *gsource, const char *gtarget, const char *gtype,
                     unsigned long flags, const char *gdata, char *abs, char *buf)
{
    /* Remount: flags of an emulated mount; others accept silently. */
    if (flags & MS_REMOUNT) {
        long e = guest_target(gtarget, abs, 1);
        if (e) return e;
        int i = at(abs);
        if (i >= 0 && g_tab->e[i].ro != !!(flags & MS_RDONLY)) {
            g_tab->e[i].ro = !!(flags & MS_RDONLY);
            bump();
        }
        return 0;
    }
    if (flags & MS_MOVE) {
        long e = guest_target(gsource, buf, 1);
        if (e) return e;
        int i = at(buf);
        if (i < 0) return TAWC_EINVAL;
        e = guest_target(gtarget, abs, 1);
        if (e) return e;
        if (!abs[1]) return TAWC_EINVAL;
        if (tawc_str_copy(g_tab->e[i].dst, sizeof g_tab->e[i].dst, abs + 1) < 0)
            return TAWC_ENAMETOOLONG;
        bump();
        return 0;
    }
    if (flags & MS_BIND) {
        long e = host_source(gsource, buf);
        if (!e) e = guest_target(gtarget, abs, 1);
        if (e) return e;
        return add_entry(abs, buf, "", "none", (int)(flags & MS_RDONLY), 0);
    }
    long e = guest_target(gtarget, abs, 0);
    int missing = e == TAWC_ENOENT;
    if (missing) e = virtual_target(gtarget, abs);
    if (e) return e;
    static const char *const api[] = {
        "/proc", "/sys", "/dev", "/dev/shm", "/dev/pts", "/run", "/sys/fs/cgroup",
    };
    /* The runtime already provides these; an init mounting them again
     * gets the existing view. */
    for (size_t k = 0; k < sizeof api / sizeof api[0]; ++k)
        if (tawc_streq(abs, api[k])) return 0;
    char type[32], source[64];
    if (!gtype || tawc_copy_string_from_guest(type, sizeof type, gtype) < 0) return TAWC_EFAULT;
    /* An empty directory is a faithful tmpfs/ramfs, and enough of an
     * mqueue (mq_open does not go through the filesystem). */
    if (!tawc_streq(type, "tmpfs") && !tawc_streq(type, "ramfs") && !tawc_streq(type, "mqueue"))
        return missing ? TAWC_ENOENT : TAWC_EPERM;
    if (!gsource || tawc_copy_string_from_guest(source, sizeof source, gsource) < 0)
        (void)tawc_str_copy(source, sizeof source, type);
    char *data = buf + 2048;
    data[0] = 0;
    if (gdata && tawc_copy_string_from_guest(data, 1024, gdata) < 0) data[0] = 0;
    uint32_t id = g_tab->next_id++;
    e = new_backing(buf, id, data);
    if (e < 0) return e;
    return add_entry(abs, buf, type, source, (int)(flags & MS_RDONLY), id);
}

long tawcroot_mounts_mount(const char *gsource, const char *gtarget, const char *gtype,
                           unsigned long flags, const char *gdata)
{
    if (!g_tab) return TAWC_EPERM;
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    tawcroot_mounts_sync();
    __atomic_add_fetch(&g_mounting, 1, __ATOMIC_ACQUIRE);
    long fd = lock(2 /*LOCK_EX*/);
    long e = fd < 0 ? fd :
        do_mount(gsource, gtarget, gtype, flags, gdata, scratch->buf[2], scratch->buf[3]);
    if (fd >= 0) tawc_close((int)fd);
    __atomic_sub_fetch(&g_mounting, 1, __ATOMIC_RELEASE);
    if (!e) notify();
    return e;
}

long tawcroot_mounts_umount(const char *gtarget, int flags)
{
    (void)flags;
    if (!g_tab) return TAWC_ENOSYS;
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    char *abs = scratch->buf[2], *dead = scratch->buf[3];
    tawcroot_mounts_sync();
    long e = guest_target(gtarget, abs, 1);
    if (e) return e == TAWC_ENOENT ? e : TAWC_ENOSYS;
    long fd = lock(2 /*LOCK_EX*/);
    if (fd < 0) return fd;
    int i = at(abs);
    if (i >= 0) {
        struct entry *m = &g_tab->e[i];
        /* A tmpfs's backing directory goes away with it; open files in
         * it keep working, like a lazily detached tmpfs. */
        size_t n = 0;
        if (m->type[0] && !store_path(dead, "/mnt/.dead-")) {
            n = tawc_strlen(dead);
            (void)tawc_str_append_dec(dead, TAWCROOT_PATH_SCRATCH_SIZE, &n, m->id);
            (void)TAWC_RAW(TAWC_SYS_renameat2, AT_FDCWD, (long)m->src, AT_FDCWD, (long)dead, 0, 0);
        }
        memset(m, 0, sizeof *m);
        bump();
    }
    tawc_close((int)fd);
    if (i >= 0) notify();
    return i >= 0 ? 0 : TAWC_ENOSYS;
}

/* --- change notification ------------------------------------------------- */

/* Writers touch <store>/mounts.event after every change. Synthesized
 * mountinfo fds live at or above the reserved floor, where a stacked
 * filter traps epoll_ctl: registering one registers instead an inotify
 * fd watching mounts.event, so libmount's monitor wakes like on a real
 * mount table change. Rendering drains those inotify fds first, so a
 * change after a render always wakes again. */
static struct { int fd, sub; unsigned long ino; } g_watch[16];

static void notify(void)
{
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    char *path = scratch->buf[0];
    if (store_path(path, "/mounts.event") < 0) return;
    long fd = tawc_openat(AT_FDCWD, path, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    (void)TAWC_RAW(TAWC_SYS_pwrite64, fd, (long)"x", 1, 0, 0, 0);
    tawc_close((int)fd);
}

static unsigned long ino_of(int fd)
{
    struct stat st;
    return TAWC_RAW(TAWC_SYS_fstat, fd, (long)&st, 0, 0, 0, 0) ? 0 : (unsigned long)st.st_ino;
}

/* Move a fresh mountinfo memfd above the floor and remember it. */
static long watchable(long fd, int cloexec)
{
    if (!(tawcroot_namespace.session & TAWC_NS_EPOLL_TRAP)) {
        if (tawcroot_install_fd_floor_filter(__NR_epoll_ctl, 2)) return fd;
        tawcroot_namespace.session |= TAWC_NS_EPOLL_TRAP;
    }
    long hi = tawc_fcntl((int)fd, cloexec ? F_DUPFD_CLOEXEC : F_DUPFD, TAWCROOT_RESERVED_FD_BASE);
    if (hi < 0) return fd;
    tawc_close((int)fd);
    unsigned long ino = ino_of((int)hi);
    int slot = -1;
    for (int i = 0; i < 16; ++i) {
        int live = g_watch[i].ino && ino_of(g_watch[i].fd) == g_watch[i].ino;
        if (!live && g_watch[i].sub > 0) {
            tawcroot_fd_forget_reserved(g_watch[i].sub);
            tawc_close(g_watch[i].sub);
            g_watch[i].sub = 0;
        }
        if (!live) g_watch[i].ino = 0;
        if (!live && slot < 0) slot = i;
    }
    if (slot >= 0) {
        g_watch[slot].fd = (int)hi;
        g_watch[slot].ino = ino;
        g_watch[slot].sub = 0;
    }
    return hi;
}

static void drain(void)
{
    char buf[256];
    for (int i = 0; i < 16; ++i)
        if (g_watch[i].sub > 0)
            while (tawc_read(g_watch[i].sub, buf, sizeof buf) > 0) {}
}

long tawcroot_mounts_epoll_ctl(const tawcroot_syscall_args *a, ucontext_t *uc)
{
    (void)uc;
    int fd = (int)a->c, i = 0;
    for (; i < 16; ++i)
        if (g_watch[i].ino && g_watch[i].fd == fd) break;
    if (i == 16 || ino_of(fd) != g_watch[i].ino)
        return TAWC_RAW(__NR_epoll_ctl, a->a, a->b, a->c, a->d, 0, 0);
    if (!g_watch[i].sub) {
        TAWCROOT_PATH_SCRATCH_AUTO(scratch);
        char *path = scratch->buf[0];
        long e = store_path(path, "/mounts.event");
        if (e < 0) return e;
        long in = TAWC_RAW(TAWC_SYS_inotify_init1, IN_NONBLOCK_FLAG | O_CLOEXEC, 0, 0, 0, 0, 0);
        if (in < 0) return in;
        e = TAWC_RAW(TAWC_SYS_inotify_add_watch, in, (long)path, 0x2 /*IN_MODIFY*/, 0, 0, 0);
        long r = e < 0 ? e : tawcroot_fd_reserve((int)in);
        if (r < 0) { tawc_close((int)in); return r; }
        g_watch[i].sub = (int)r;
    }
    struct { uint32_t events; uint64_t data; } __attribute__((packed)) ev;
    long evp = 0;
    if (a->b != 2 /*EPOLL_CTL_DEL*/) {
        long e = tawc_copy_from_guest(&ev, sizeof ev, (const void *)(uintptr_t)a->d);
        if (e < 0) return e;
        ev.events = (ev.events | 0x001 /*EPOLLIN*/) & ~0x002u /*EPOLLPRI*/;
        evp = (long)&ev;
    }
    return TAWC_RAW(__NR_epoll_ctl, a->a, a->b, g_watch[i].sub, evp, 0, 0);
}

/* --- views -------------------------------------------------------------- */

static const char *magic_name(long magic)
{
    switch (magic) {
    case 0xEF53: return "ext4";
    case 0xF2F52010: return "f2fs";
    case TMPFS_MAGIC: return "tmpfs";
    case RAMFS_MAGIC: return "ramfs";
    case 0x9fa0: return "proc";
    case 0x62656572: return "sysfs";
    case 0x1cd1: return "devpts";
    case 0x794c7630: return "overlay";
    case 0x9123683E: return "btrfs";
    case 0x58465342: return "xfs";
    case 0x65735546: return "fuse";
    case 0x63677270: return "cgroup2";
    default: return "none";
    }
}

/* Filesystem type shown for a bind with guest path `dst`. */
static const char *bind_type(const struct tawcroot_bind *b)
{
    if (b->mount_id && g_tab) {
        const struct entry *e = find_id(b->mount_id);
        if (e && e->type[0]) return e->type;
    }
    static const char *const known[][2] = {
        { "proc", "proc" }, { "sys", "sysfs" }, { "dev", "devtmpfs" },
        { "dev/pts", "devpts" }, { "dev/shm", "tmpfs" }, { "run", "tmpfs" },
        { "sys/fs/cgroup", "cgroup2" },
    };
    if (!b->mount_id)
        for (size_t i = 0; i < sizeof known / sizeof known[0]; ++i)
            if (tawc_streq(b->dst, known[i][0])) return known[i][1];
    long st[15];
    if (TAWC_RAW(TAWC_SYS_fstatfs, b->src_fd, (long)st, 0, 0, 0, 0)) return "none";
    return magic_name(st[0]);
}

static const char *bind_source(const struct tawcroot_bind *b, const char *type)
{
    if (b->mount_id && g_tab) {
        const struct entry *e = find_id(b->mount_id);
        if (e && e->source[0]) return e->source;
    }
    return type;
}

static void put_dev(char *line, size_t cap, size_t *n, int fd)
{
    struct stat st;
    unsigned long dev = TAWC_RAW(TAWC_SYS_fstat, fd, (long)&st, 0, 0, 0, 0) ? 0 : st.st_dev;
    (void)tawc_str_append_dec(line, cap, n, (long)(((dev >> 8) & 0xfff) | ((dev >> 32) & ~0xfffUL)));
    (void)tawc_str_append(line, cap, n, ":");
    (void)tawc_str_append_dec(line, cap, n, (long)((dev & 0xff) | ((dev >> 12) & ~0xffUL)));
}

/* Is `p` a proper ancestor of `c` (both bind dsts)? */
static int under(const char *p, size_t pl, const char *c)
{
    for (size_t i = 0; i < pl; ++i) if (p[i] != c[i]) return 0;
    return c[pl] == '/';
}

static long write_line(int fd, const char *line, size_t n)
{
    long w = tawc_write(fd, line, n);
    return w == (long)n ? 0 : w < 0 ? w : TAWC_EIO;
}

/* One line per mount, parents first (shorter dst first). */
static long render(int out, int info)
{
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    int *order = (int *)scratch->buf[0];
    char *line = scratch->buf[1];
    size_t cap = TAWCROOT_PATH_SCRATCH_SIZE, count = 0;
    size_t nb = __atomic_load_n(&tawcroot_n_binds, __ATOMIC_ACQUIRE);
    for (size_t i = 0; i < nb; ++i) if (tawcroot_binds[i].active) order[count++] = (int)i;
    for (size_t i = 1; i < count; ++i)
        for (size_t j = i; j > 0 && tawcroot_binds[order[j]].dst_len < tawcroot_binds[order[j - 1]].dst_len; --j) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    long st[15];
    const char *root_type = TAWC_RAW(TAWC_SYS_fstatfs, tawcroot_rootfs_fd, (long)st, 0, 0, 0, 0)
        ? "none" : magic_name(st[0]);
    for (long k = -1; k < (long)count; ++k) {
        const struct tawcroot_bind *b = k < 0 ? 0 : &tawcroot_binds[order[k]];
        const char *type = b ? bind_type(b) : root_type;
        const char *source = b ? bind_source(b, type) : root_type;
        int ro = b ? b->read_only : tawcroot_root_ro;
        size_t n = 0;
        if (info) {
            long parent = 1;
            for (long p = k - 1; b && p >= 0; --p) {
                const struct tawcroot_bind *pb = &tawcroot_binds[order[p]];
                if (under(pb->dst, pb->dst_len, b->dst)) { parent = order[p] + 2; break; }
            }
            (void)tawc_str_append_dec(line, cap, &n, b ? order[k] + 2 : 1);
            (void)tawc_str_append(line, cap, &n, " ");
            (void)tawc_str_append_dec(line, cap, &n, b ? parent : 0);
            (void)tawc_str_append(line, cap, &n, " ");
            put_dev(line, cap, &n, b ? b->src_fd : tawcroot_rootfs_fd);
            (void)tawc_str_append(line, cap, &n, " / /");
            if (b) (void)tawc_str_append(line, cap, &n, b->dst);
            (void)tawc_str_append(line, cap, &n, ro ? " ro,relatime - " : " rw,relatime - ");
            (void)tawc_str_append(line, cap, &n, type);
            (void)tawc_str_append(line, cap, &n, " ");
            (void)tawc_str_append(line, cap, &n, source);
            (void)tawc_str_append(line, cap, &n, ro ? " ro\n" : " rw\n");
        } else {
            (void)tawc_str_append(line, cap, &n, source);
            (void)tawc_str_append(line, cap, &n, " /");
            if (b) (void)tawc_str_append(line, cap, &n, b->dst);
            (void)tawc_str_append(line, cap, &n, " ");
            (void)tawc_str_append(line, cap, &n, type);
            (void)tawc_str_append(line, cap, &n, ro ? " ro,relatime 0 0\n" : " rw,relatime 0 0\n");
        }
        long e = write_line(out, line, n);
        if (e) return e;
    }
    return 0;
}

long tawcroot_mounts_proc_open(const char *path, int flags)
{
    if (!session() || !tawc_starts_with(path, "/proc/") || (flags & O_ACCMODE) != O_RDONLY ||
        (flags & (O_PATH | O_DIRECTORY)))
        return TAWC_ENOSYS;
    const char *p = path + 6;
    if (!tawc_streq(p, "mounts")) {
        if (tawc_starts_with(p, "self/")) p += 5;
        else if (tawc_starts_with(p, "thread-self/")) p += 12;
        else {
            if (*p < '0' || *p > '9') return TAWC_ENOSYS;
            while (*p >= '0' && *p <= '9') ++p;
            if (*p++ != '/') return TAWC_ENOSYS;
        }
        if (tawc_starts_with(p, "task/")) {
            for (p += 5; *p >= '0' && *p <= '9'; ++p) {}
            if (*p++ != '/') return TAWC_ENOSYS;
        }
        if (!tawc_streq(p, "mountinfo") && !tawc_streq(p, "mounts")) return TAWC_ENOSYS;
    }
    tawcroot_mounts_sync();
    drain();
    long fd = tawc_memfd_create("tawcroot-mounts", (flags & O_CLOEXEC) ? 1 : 0);
    if (fd < 0) return fd;
    long e = render((int)fd, tawc_streq(p, "mountinfo"));
    if (e) { tawc_close((int)fd); return e; }
    (void)tawc_lseek((int)fd, 0, SEEK_SET);
    return g_tab ? watchable(fd, (flags & O_CLOEXEC) != 0) : fd;
}

/* fstatfs, trapped in init sessions: the same container-facing fixups
 * as statfs (tmpfs magic for emulated mounts, read-only /proc/sys), for
 * callers that statfs an O_PATH fd (systemd). */
long tawcroot_mounts_fstatfs(const tawcroot_syscall_args *a, ucontext_t *uc)
{
    (void)uc;
    long rv = TAWC_RAW(TAWC_SYS_fstatfs, a->a, a->b, 0, 0, 0, 0);
    if (rv || !session() || tawcroot_fd_is_reserved((int)a->a)) return rv;
    long *out = (long *)(uintptr_t)a->b;   /* kernel-validated */
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    char *host = scratch->buf[0];
    long n = tawcroot_proc_fd_to_host_path((int)a->a, host, TAWCROOT_PATH_SCRATCH_SIZE);
    if (n < 0) return 0;
    if (out[0] == 0x9fa0 && tawc_starts_with(host, "/proc/sys") && (!host[9] || host[9] == '/'))
        out[10] |= 1; /* ST_RDONLY */
    for (size_t i = 0; g_tab && i < tawcroot_n_binds; ++i) {
        const struct tawcroot_bind *b = &tawcroot_binds[i];
        if (!b->active || !b->mount_id || !tawc_starts_with(host, b->src) ||
            (host[b->src_len] && host[b->src_len] != '/')) continue;
        long m = tawcroot_mounts_magic(b->src_fd);
        if (m) out[0] = m;
    }
    return 0;
}

long tawcroot_mounts_magic(int fd)
{
    if (!g_tab) return 0;
    for (size_t i = 0; i < tawcroot_n_binds; ++i) {
        const struct tawcroot_bind *b = &tawcroot_binds[i];
        if (!b->active || !b->mount_id || b->src_fd != fd) continue;
        const struct entry *e = find_id(b->mount_id);
        if (!e) return 0;
        return tawc_streq(e->type, "tmpfs") ? TMPFS_MAGIC :
            tawc_streq(e->type, "ramfs") ? RAMFS_MAGIC : 0;
    }
    return 0;
}
