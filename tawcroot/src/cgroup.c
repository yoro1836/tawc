/* Fake cgroup v2 for `--cgroup` sessions. Android gives the app no
 * writable cgroupfs and systemd will not boot without one, so
 * /sys/fs/cgroup is a bind of <store>/cgroup: directories are real,
 * interface files are real files (ls, stat and inotify work), and the
 * dynamic parts are emulated:
 *
 *   - membership lives in <store>/cgroup.registry, a shared pid ->
 *     cgroup table written at fork/exec and by cgroup.procs writes;
 *   - reads of cgroup.procs/threads/events are synthesized from it, and
 *     cgroup.events is rewritten when a move changes "populated" so
 *     inotify watchers wake up;
 *   - writable opens of cgroup.procs/threads/kill land at or above the
 *     reserved fd floor, where a stacked filter traps write();
 *   - statfs reports CGROUP2_SUPER_MAGIC and /proc/<pid>/cgroup is
 *     synthesized.
 *
 * No resource control: cgroup.controllers is empty. See
 * notes/tawcroot/systemd.md. */
#include <stdint.h>
#include <fcntl.h>
#include <asm/unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "cgroup.h"
#include "errno_neg.h"
#include "fdtab.h"
#include "filter.h"
#include "io.h"
#include "linkstore.h"
#include "namespace.h"
#include "path.h"
#include "path_scratch.h"
#include "raw_sys.h"
#include "sysnr.h"
#include "tawc_string.h"
#include "usercopy.h"

#define SLOTS 4096
#define MAGIC 0x43475231
/* pids first and dense: lookups scan 16 KiB, not the whole table. */
struct registry {
    uint32_t magic, pad;
    int32_t pid[SLOTS];
    uint64_t start[SLOTS];
    char path[SLOTS][240];
};

enum { F_PROCS = 1, F_THREADS, F_EVENTS, F_KILL };

/* Interface files of every cgroup. `root` ones also exist at the top. */
static const struct { const char *name, *text; unsigned mode, root, kind; } files[] = {
    { "cgroup.procs",           "",                    0644, 1, F_PROCS },
    { "cgroup.threads",         "",                    0644, 1, F_THREADS },
    { "cgroup.controllers",     "",                    0444, 1, 0 },
    { "cgroup.subtree_control", "",                    0644, 1, 0 },
    { "cgroup.max.depth",       "max\n",               0644, 1, 0 },
    { "cgroup.max.descendants", "max\n",               0644, 1, 0 },
    { "cgroup.stat",            "nr_descendants 0\nnr_dying_descendants 0\n", 0444, 1, 0 },
    { "cgroup.events",          "populated 0\nfrozen 0\n", 0444, 0, F_EVENTS },
    { "cgroup.type",            "domain\n",            0644, 0, 0 },
    { "cgroup.freeze",          "0\n",                 0644, 0, 0 },
    { "cgroup.kill",            "",                    0200, 0, F_KILL },
};
#define N_FILES (sizeof files / sizeof files[0])

static int active(void)
{
    return (tawcroot_namespace.session & TAWC_NS_CGROUP) != 0;
}

static struct tawcroot_bind *cg_bind(void)
{
    if (!active()) return 0;
    for (size_t i = 0; i < tawcroot_n_binds; ++i)
        if (tawcroot_binds[i].active && tawc_streq(tawcroot_binds[i].dst, "sys/fs/cgroup"))
            return &tawcroot_binds[i];
    return 0;
}

/* --- registry --------------------------------------------------------- */

/* <store>/<name> into a scratch buffer. */
static long store_path(char *out, const char *name)
{
    size_t n = 0;
    if (!tawcroot_store_host_path_len) return TAWC_ENOENT;
    if (tawc_str_append(out, TAWCROOT_PATH_SCRATCH_SIZE, &n, tawcroot_store_host_path) ||
        tawc_str_append(out, TAWCROOT_PATH_SCRATCH_SIZE, &n, name))
        return TAWC_ENAMETOOLONG;
    return 0;
}

static long reg_open(struct registry **out)
{
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    char *path = scratch->buf[0];
    long fd = store_path(path, "/cgroup.registry");
    if (fd < 0) return fd;
    fd = tawc_openat(AT_FDCWD, path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return fd;
    long e = TAWC_RAW(__NR_flock, fd, 2 /*LOCK_EX*/, 0, 0, 0, 0);
    if (!e) e = TAWC_RAW(TAWC_SYS_ftruncate, fd, sizeof(struct registry), 0, 0, 0, 0);
    if (e < 0) { tawc_close((int)fd); return e; }
    long p = tawc_mmap(0, sizeof(struct registry), PROT_READ | PROT_WRITE, MAP_SHARED, (int)fd, 0);
    if (p < 0 && p >= -4095) { tawc_close((int)fd); return p; }
    *out = (struct registry *)p;
    if ((*out)->magic != MAGIC) {
        memset(*out, 0, sizeof **out);
        (*out)->magic = MAGIC;
    }
    return fd;
}

static void reg_close(long fd, struct registry *r)
{
    tawc_munmap(r, sizeof *r);
    tawc_close((int)fd);
}

/* /proc/<pid>/stat (pid 0: self): state, ppid and start time. */
static long proc_stat(long pid, char *state, long *ppid, uint64_t *start)
{
    char buf[512];
    size_t n = 0;
    if (tawc_str_append(buf, sizeof buf, &n, "/proc/")) return TAWC_EINVAL;
    if (pid ? tawc_str_append_dec(buf, sizeof buf, &n, pid) :
              tawc_str_append(buf, sizeof buf, &n, "self"))
        return TAWC_EINVAL;
    if (tawc_str_append(buf, sizeof buf, &n, "/stat")) return TAWC_EINVAL;
    long fd = tawc_openat(AT_FDCWD, buf, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return fd == TAWC_ENOENT ? TAWC_ESRCH : fd;
    long len = tawc_read((int)fd, buf, sizeof buf - 1);
    tawc_close((int)fd);
    if (len <= 0) return TAWC_ESRCH;
    buf[len] = 0;
    const char *p = 0;
    for (long i = 0; i < len; ++i) if (buf[i] == ')') p = buf + i + 1;
    if (!p) return TAWC_EIO;
    /* Fields after "(comm)" start at 3: state, ppid, ..., starttime (22). */
    int field = 2;
    uint64_t v = 0;
    for (; *p; ++p) {
        if (*p == ' ') { ++field; v = 0; continue; }
        if (field == 3) *state = *p;
        if (*p >= '0' && *p <= '9') v = v * 10 + (uint64_t)(*p - '0');
        if (field == 4 && (p[1] == ' ' || !p[1])) *ppid = (long)v;
        if (field == 22 && (p[1] == ' ' || !p[1])) { *start = v; return 0; }
    }
    return TAWC_EIO;
}

/* Slot i is live, not exited, and still the process that registered. */
static int valid(const struct registry *r, int i)
{
    char state = 0;
    long ppid;
    uint64_t start;
    return r->pid[i] > 0 && !proc_stat(r->pid[i], &state, &ppid, &start) &&
        start == r->start[i] && state != 'Z' && state != 'X';
}

static int find(struct registry *r, long pid)
{
    for (int i = 0; i < SLOTS; ++i) if (r->pid[i] == pid) return i;
    return -1;
}

static long set(struct registry *r, long pid, uint64_t start, const char *path)
{
    int i = find(r, pid);
    if (i < 0) i = find(r, 0);
    if (i < 0) {
        for (int k = 0; k < SLOTS; ++k) if (!valid(r, k)) r->pid[k] = 0;
        i = find(r, 0);
    }
    if (i < 0) return TAWC_ENOSPC;
    if (tawc_str_copy(r->path[i], sizeof r->path[i], path) < 0) return TAWC_ENAMETOOLONG;
    r->start[i] = start;
    r->pid[i] = (int32_t)pid;
    return 0;
}

static int in_subtree(const char *path, const char *rel)
{
    size_t n = tawc_strlen(rel);
    return !n || (tawc_starts_with(path, rel) && (path[n] == 0 || path[n] == '/'));
}

/* Whether any live process sits in `rel` or below. Drops dead entries. */
static int populated(struct registry *r, const char *rel)
{
    int any = 0;
    for (int i = 0; i < SLOTS && !any; ++i) {
        if (!r->pid[i] || !in_subtree(r->path[i], rel)) continue;
        if (valid(r, i)) any = 1;
        else r->pid[i] = 0;
    }
    return any;
}

/* Register `pid` (0: self) in `path`, or in its parent's cgroup when
 * path is NULL. */
static void join(long pid, long parent, const char *path)
{
    char state;
    long ppid;
    uint64_t start;
    if (proc_stat(pid, &state, &ppid, &start)) return;
    if (!pid) pid = tawc_getpid();
    struct registry *r;
    long fd = reg_open(&r);
    if (fd < 0) return;
    int i = find(r, pid);
    if (!path && i >= 0 && r->start[i] == start) { reg_close(fd, r); return; }
    if (!path) {
        int p = find(r, parent ? parent : ppid);
        path = p >= 0 ? r->path[p] : "";
    }
    (void)set(r, pid, start, path);
    reg_close(fd, r);
}

void tawcroot_cgroup_fork_child(long parent)
{
    if (active()) join(0, parent, 0);
}

void tawcroot_cgroup_exec(void)
{
    if (active()) join(0, 0, 0);
}

/* --- tree --------------------------------------------------------------- */

static void populate(int dirfd, int root)
{
    for (size_t i = 0; i < N_FILES; ++i) {
        if (root && !files[i].root) continue;
        long fd = tawc_openat(dirfd, files[i].name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, files[i].mode);
        if (fd < 0) continue;
        (void)tawc_write((int)fd, files[i].text, tawc_strlen(files[i].text));
        tawc_close((int)fd);
    }
}

/* Rewrite <rel>/cgroup.events and its ancestors' when "populated"
 * changed, so inotify watchers (systemd) notice. */
static void refresh_events(struct registry *r, const char *rel)
{
    struct tawcroot_bind *b = cg_bind();
    if (!b) return;
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    char *path = scratch->buf[0];
    if (tawc_str_copy(path, TAWCROOT_PATH_SCRATCH_SIZE, rel) < 0) return;
    for (size_t n = tawc_strlen(path); n; ) {
        char text[40];
        size_t t = 0;
        (void)tawc_str_append(text, sizeof text, &t, populated(r, path) ? "populated 1\n" : "populated 0\n");
        (void)tawc_str_append(text, sizeof text, &t, "frozen 0\n");
        size_t end = n;
        (void)tawc_str_append(path, TAWCROOT_PATH_SCRATCH_SIZE, &end, "/cgroup.events");
        char old[40];
        long fd = tawc_openat(b->src_fd, path, O_RDWR | O_CLOEXEC, 0);
        if (fd >= 0) {
            long got = tawc_read((int)fd, old, sizeof old);
            if (got != (long)t || memcmp(old, text, t)) {
                (void)TAWC_RAW(TAWC_SYS_ftruncate, fd, 0, 0, 0, 0, 0);
                (void)TAWC_RAW(TAWC_SYS_pwrite64, fd, (long)text, (long)t, 0, 0, 0);
            }
            tawc_close((int)fd);
        }
        while (n && path[n - 1] != '/') --n;
        if (n) --n;
        path[n] = 0;
    }
}

/* Remove everything under `dirfd` (init-time wipe of a previous boot's
 * tree; only directories and interface files live there). */
static void wipe(int dirfd, int depth)
{
    static char bufs[16][1024];
    if (depth >= 16) return;
    char *buf = bufs[depth];
    long n;
    for (int pass = 0; pass < 2; ++pass, (void)tawc_lseek(dirfd, 0, 0))
    while ((n = tawc_getdents64(dirfd, buf, 1024)) > 0) {
        for (long pos = 0; pos < n; ) {
            struct { uint64_t ino; int64_t off; unsigned short len; unsigned char type; char name[]; } *d = (void *)(buf + pos);
            pos += d->len;
            if (tawc_streq(d->name, ".") || tawc_streq(d->name, "..")) continue;
            if (d->type == 4 /*DT_DIR*/) {
                long sub = tawc_openat(dirfd, d->name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, 0);
                if (sub >= 0) { wipe((int)sub, depth + 1); tawc_close((int)sub); }
                (void)TAWC_RAW(TAWC_SYS_unlinkat, dirfd, (long)d->name, AT_REMOVEDIR, 0, 0, 0);
            } else {
                (void)TAWC_RAW(TAWC_SYS_unlinkat, dirfd, (long)d->name, 0, 0, 0, 0);
            }
        }
    }
}

void tawcroot_wipe_dir(int dirfd)
{
    wipe(dirfd, 0);
}

long tawcroot_cgroup_setup(void)
{
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    char *path = scratch->buf[0];
    long e = store_path(path, "/cgroup");
    if (e < 0) return e;
    (void)TAWC_RAW(TAWC_SYS_mkdirat, AT_FDCWD, (long)tawcroot_store_host_path, 0700, 0, 0, 0);
    e = TAWC_RAW(TAWC_SYS_mkdirat, AT_FDCWD, (long)path, 0755, 0, 0, 0);
    if (e < 0 && e != TAWC_EEXIST) return e;
    long dir = tawc_openat(AT_FDCWD, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (dir < 0) return dir;
    wipe((int)dir, 0);
    populate((int)dir, 1);
    tawc_close((int)dir);
    e = tawcroot_path_add_bind(path, "sys/fs/cgroup", 0);
    if (e < 0) return e;
    tawcroot_namespace.session |= TAWC_NS_CGROUP;
    struct registry *r;
    long fd = reg_open(&r);
    if (fd < 0) return fd;
    memset(r->pid, 0, sizeof r->pid);
    reg_close(fd, r);
    join(0, 0, "");
    return 0;
}

/* `--join`: the init session's tree, with the caller in the root. */
long tawcroot_cgroup_join(void)
{
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    char *path = scratch->buf[0];
    long e = store_path(path, "/cgroup");
    if (!e) e = tawcroot_path_add_bind(path, "sys/fs/cgroup", 0);
    if (e < 0) return e;
    tawcroot_namespace.session |= TAWC_NS_CGROUP;
    join(0, 0, "");
    return 0;
}

int tawcroot_cgroup_locate(int fd, const char *suffix, char *rel, size_t cap)
{
    struct tawcroot_bind *b = cg_bind();
    if (!b || fd < 0) return 0;
    size_t n = 0;
    rel[0] = 0;
    if (fd != b->src_fd) {
        if (fd == tawcroot_rootfs_fd) return 0;
        for (size_t i = 0; i < tawcroot_n_binds; ++i)
            if (tawcroot_binds[i].src_fd == fd) return 0;
        TAWCROOT_PATH_SCRATCH_AUTO(scratch);
        char *host = scratch->buf[0];
        long len = tawcroot_proc_fd_to_host_path(fd, host, TAWCROOT_PATH_SCRATCH_SIZE);
        if (len < (long)b->src_len || !tawc_starts_with(host, b->src) ||
            (host[b->src_len] && host[b->src_len] != '/'))
            return 0;
        const char *tail = host + b->src_len;
        while (*tail == '/') ++tail;
        if (tawc_str_append(rel, cap, &n, tail)) return 0;
    }
    if (suffix && *suffix && !tawc_streq(suffix, ".")) {
        if (n && tawc_str_append(rel, cap, &n, "/")) return 0;
        if (tawc_str_append(rel, cap, &n, suffix)) return 0;
    }
    while (n && rel[n - 1] == '/') rel[--n] = 0;
    return 1;
}

void tawcroot_cgroup_mkdir(int fd, const char *path, const char *rel)
{
    (void)rel;
    long dir = tawc_openat(fd, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, 0);
    if (dir < 0) return;
    populate((int)dir, 0);
    tawc_close((int)dir);
}

static int is_interface(const char *name)
{
    for (size_t i = 0; i < N_FILES; ++i) if (tawc_streq(files[i].name, name)) return 1;
    return 0;
}

long tawcroot_cgroup_rmdir(int fd, const char *path, const char *rel)
{
    if (!*rel) return TAWC_EBUSY;
    long dir = tawc_openat(fd, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, 0);
    if (dir < 0) return dir;
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    char *buf = scratch->buf[0];
    long n, err = 0;
    while (!err && (n = tawc_getdents64((int)dir, buf, TAWCROOT_PATH_SCRATCH_SIZE)) > 0)
        for (long pos = 0; pos < n && !err; ) {
            struct { uint64_t ino; int64_t off; unsigned short len; unsigned char type; char name[]; } *d = (void *)(buf + pos);
            pos += d->len;
            if (tawc_streq(d->name, ".") || tawc_streq(d->name, "..")) continue;
            if (d->type == 4 /*DT_DIR*/) err = TAWC_EBUSY;
            else if (!is_interface(d->name)) err = TAWC_ENOTEMPTY;
        }
    if (!err) {
        struct registry *r;
        long rfd = reg_open(&r);
        if (rfd >= 0) {
            if (populated(r, rel)) err = TAWC_EBUSY;
            reg_close(rfd, r);
        }
    }
    if (!err)
        for (size_t i = 0; i < N_FILES; ++i)
            (void)TAWC_RAW(TAWC_SYS_unlinkat, dir, (long)files[i].name, 0, 0, 0, 0);
    tawc_close((int)dir);
    if (err) return err;
    return TAWC_RAW(TAWC_SYS_unlinkat, fd, (long)path, AT_REMOVEDIR, 0, 0, 0);
}

/* --- interface files ------------------------------------------------------ */

static long memfd_with(const char *text, size_t len, int flags)
{
    long fd = tawc_memfd_create("tawcroot-cgroup", 1U /*MFD_CLOEXEC*/);
    if (fd < 0) return fd;
    if (len && tawc_write((int)fd, text, len) != (long)len) { tawc_close((int)fd); return TAWC_EIO; }
    (void)tawc_lseek((int)fd, 0, 0);
    if (!(flags & O_CLOEXEC)) (void)tawc_fcntl((int)fd, F_SETFD, 0);
    return fd;
}

static long guest_pid(long pid)
{
    return pid == tawcroot_namespace.init_pid && pid ? 1 : pid;
}

/* Fds open for writing on cgroup.procs/threads/kill, per process. */
static struct { int fd, kind; unsigned long dev, ino; char dir[240]; } wfds[16];

static long ensure_write_trap(void)
{
    if (tawcroot_namespace.session & TAWC_NS_CG_WRITE_TRAP) return 0;
    long e = tawcroot_install_fd_floor_filter(TAWC_SYS_write, 0);
    if (!e) tawcroot_namespace.session |= TAWC_NS_CG_WRITE_TRAP;
    return e;
}

long tawcroot_cgroup_open(int fd, const char *path, int flags, const char *rel)
{
    const char *leaf = rel + tawc_strlen(rel);
    while (leaf > rel && leaf[-1] != '/') --leaf;
    int kind = 0;
    for (size_t i = 0; i < N_FILES; ++i) if (tawc_streq(files[i].name, leaf)) kind = (int)files[i].kind;
    if (!kind || (flags & (O_PATH | O_DIRECTORY))) return TAWC_ENOSYS;
    size_t dirlen = (size_t)(leaf - rel);
    if (dirlen) --dirlen;
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    char *dir = scratch->buf[0], *text = scratch->buf[1];
    memcpy(dir, rel, dirlen);
    dir[dirlen] = 0;

    if ((flags & O_ACCMODE) == O_RDONLY) {
        if (kind == F_KILL) return TAWC_ENOSYS;
        struct registry *r;
        long rfd = reg_open(&r);
        if (rfd < 0) return rfd;
        size_t n = 0;
        if (kind == F_EVENTS) {
            (void)tawc_str_append(text, TAWCROOT_PATH_SCRATCH_SIZE, &n,
                                  populated(r, dir) ? "populated 1\n" : "populated 0\n");
            (void)tawc_str_append(text, TAWCROOT_PATH_SCRATCH_SIZE, &n, "frozen 0\n");
        } else {
            text[0] = 0;
            for (int i = 0; i < SLOTS; ++i) {
                if (!r->pid[i] || !tawc_streq(r->path[i], dir)) continue;
                if (!valid(r, i)) { r->pid[i] = 0; continue; }
                size_t at = n;
                if (tawc_str_append_dec(text, TAWCROOT_PATH_SCRATCH_SIZE, &n, guest_pid(r->pid[i])) ||
                    tawc_str_append(text, TAWCROOT_PATH_SCRATCH_SIZE, &n, "\n")) {
                    n = at;
                    break;
                }
            }
        }
        reg_close(rfd, r);
        return memfd_with(text, n, flags);
    }
    if (kind == F_EVENTS) return TAWC_EACCES;
    long e = ensure_write_trap();
    if (e < 0) return e;
    long real = tawc_openat(fd, path, (flags & ~(O_CREAT | O_TRUNC | O_EXCL)) | O_CLOEXEC, 0);
    if (real < 0) return real;
    long hi = tawc_fcntl((int)real, (flags & O_CLOEXEC) ? F_DUPFD_CLOEXEC : F_DUPFD,
                         TAWCROOT_RESERVED_FD_BASE);
    tawc_close((int)real);
    if (hi < 0) return hi;
    struct stat st;
    if (TAWC_RAW(TAWC_SYS_fstat, hi, (long)&st, 0, 0, 0, 0) < 0) st.st_ino = 0;
    for (size_t i = 0; i < sizeof wfds / sizeof wfds[0]; ++i) {
        struct stat old;
        if (wfds[i].fd && wfds[i].fd != hi &&
            !TAWC_RAW(TAWC_SYS_fstat, wfds[i].fd, (long)&old, 0, 0, 0, 0) &&
            old.st_dev == wfds[i].dev && old.st_ino == wfds[i].ino)
            continue;
        wfds[i].fd = (int)hi;
        wfds[i].kind = kind;
        wfds[i].dev = st.st_dev;
        wfds[i].ino = st.st_ino;
        (void)tawc_str_copy(wfds[i].dir, sizeof wfds[i].dir, dir);
        return hi;
    }
    return hi;  /* table full: writes reach the plain file */
}

static long move(const char *dir, long pid)
{
    if (!pid) pid = tawc_getpid();
    else if (pid == 1 && tawcroot_namespace.init_pid) pid = tawcroot_namespace.init_pid;
    char state;
    long ppid;
    uint64_t start;
    if (proc_stat(pid, &state, &ppid, &start) || state == 'Z') return TAWC_ESRCH;
    struct registry *r;
    long fd = reg_open(&r);
    if (fd < 0) return fd;
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    char *old = scratch->buf[0];
    int i = find(r, pid);
    (void)tawc_str_copy(old, TAWCROOT_PATH_SCRATCH_SIZE, i >= 0 ? r->path[i] : "");
    long e = set(r, pid, start, dir);
    if (!e) {
        refresh_events(r, old);
        refresh_events(r, dir);
    }
    reg_close(fd, r);
    return e;
}

static long kill_tree(const char *dir)
{
    struct registry *r;
    long fd = reg_open(&r);
    if (fd < 0) return fd;
    long self = tawc_getpid();
    for (int i = 0; i < SLOTS; ++i)
        if (r->pid[i] && r->pid[i] != self && in_subtree(r->path[i], dir) && valid(r, i))
            (void)TAWC_RAW(__NR_kill, r->pid[i], 9 /*SIGKILL*/, 0, 0, 0, 0);
    reg_close(fd, r);
    return 0;
}

long tawcroot_cgroup_write(int fd, const void *buf, size_t len)
{
    if (fd < TAWCROOT_RESERVED_FD_BASE || !active()) return TAWC_ENOSYS;
    int slot = -1;
    for (size_t i = 0; i < sizeof wfds / sizeof wfds[0]; ++i) if (wfds[i].fd == fd) slot = (int)i;
    if (slot < 0) return TAWC_ENOSYS;
    struct stat st;
    if (TAWC_RAW(TAWC_SYS_fstat, fd, (long)&st, 0, 0, 0, 0) < 0 ||
        st.st_dev != wfds[slot].dev || st.st_ino != wfds[slot].ino)
        return TAWC_ENOSYS;
    char text[64];
    if (!len || len >= sizeof text) return TAWC_EINVAL;
    if (tawc_copy_from_guest(text, len, buf) < 0) return TAWC_EFAULT;
    text[len] = 0;
    size_t end = len;
    while (end && (text[end - 1] == '\n' || text[end - 1] == ' ')) text[--end] = 0;
    for (size_t i = 0; i < end; ++i) if (text[i] < '0' || text[i] > '9') return TAWC_EINVAL;
    if (!end) return TAWC_EINVAL;
    long v = tawc_parse_long(text), e;
    if (wfds[slot].kind == F_KILL) e = v == 1 ? kill_tree(wfds[slot].dir) : TAWC_EINVAL;
    else e = move(wfds[slot].dir, v);
    return e < 0 ? e : (long)len;
}

/* /proc/self/cgroup, /proc/thread-self/cgroup, /proc/<pid>/cgroup. */
long tawcroot_cgroup_proc_open(const char *path, int flags)
{
    if (!active() || !tawc_starts_with(path, "/proc/") || (flags & O_ACCMODE) != O_RDONLY ||
        (flags & (O_PATH | O_DIRECTORY)))
        return TAWC_ENOSYS;
    const char *p = path + 6;
    long pid = 0;
    if (tawc_starts_with(p, "self/")) p += 5;
    else if (tawc_starts_with(p, "thread-self/")) p += 12;
    else {
        if (*p < '0' || *p > '9') return TAWC_ENOSYS;
        while (*p >= '0' && *p <= '9') pid = pid * 10 + (*p++ - '0');
        if (*p++ != '/') return TAWC_ENOSYS;
        if (pid == 1 && tawcroot_namespace.init_pid) pid = tawcroot_namespace.init_pid;
    }
    if (!tawc_streq(p, "cgroup")) return TAWC_ENOSYS;
    if (!pid) pid = tawc_getpid();
    char state;
    long ppid;
    uint64_t start;
    if (proc_stat(pid, &state, &ppid, &start)) return TAWC_ENOENT;
    struct registry *r;
    long fd = reg_open(&r);
    if (fd < 0) return fd;
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    char *text = scratch->buf[0];
    size_t n = 0;
    int i = find(r, pid);
    (void)tawc_str_append(text, TAWCROOT_PATH_SCRATCH_SIZE, &n, "0::/");
    if (i >= 0 && r->start[i] == start)
        (void)tawc_str_append(text, TAWCROOT_PATH_SCRATCH_SIZE, &n, r->path[i]);
    (void)tawc_str_append(text, TAWCROOT_PATH_SCRATCH_SIZE, &n, "\n");
    reg_close(fd, r);
    return memfd_with(text, n, flags);
}
