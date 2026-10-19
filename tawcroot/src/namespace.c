/* Bounded namespace startup model, not kernel isolation. No application names
 * or command-line detection. State follows the runtime's exec-state transport. */
#include <linux/sched.h>
#include <linux/capability.h>
#include <asm/unistd.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include "namespace.h"
#include "cgroup.h"
#include "mounts.h"
#include "chroot.h"
#include "dispatch.h"
#include "errno_neg.h"
#include "fdtab.h"
#include "filter.h"
#include "io.h"
#include "path_scratch.h"
#include "raw_sys.h"
#include "tawc_string.h"
#include "tawc_uapi.h"
#include "usercopy.h"

#define NS_FLAGS (CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET)
struct tawc_namespace tawcroot_namespace;

int tawcroot_namespace_probe(const char *path)
{
    return tawc_streq(path, "/proc/self/ns/user") ||
        tawc_streq(path, "/proc/self/ns/pid") || tawc_streq(path, "/proc/self/ns/net");
}

/* write, getpid & co only have work once a fake namespace exists, so
 * they stay out of the base filter (every guest write() would pay a
 * SIGSYS otherwise) and get trapped by a stacked filter on first entry.
 * Inherited across fork/exec, as is the namespace state that implies
 * it, so "already active" means "already trapped". */
static const int user_lazy[] = { TAWC_SYS_write };
static const int pid_lazy[] = {
    TAWC_SYS_getpid, TAWC_SYS_getppid, __NR_kill, __NR_tgkill, __NR_tkill,
    __NR_rt_sigqueueinfo, __NR_rt_tgsigqueueinfo, TAWC_SYS_pidfd_open,
};

static long new_user(void)
{
    struct tawc_namespace *ns = &tawcroot_namespace;
    if (!ns->user) {
        long trapped = tawcroot_install_lazy_filter(user_lazy, sizeof user_lazy / sizeof user_lazy[0]);
        if (trapped < 0) return trapped;
    }
    /* Allocate before publishing user state. Descriptor allocation failures
     * must not appear to have successfully created a namespace. */
    int fds[3];
    for (int i = 0; i < 3; ++i) {
        long fd = tawc_memfd_create("tawcroot-id-map", 2);
        if (fd < 0) {
            while (i) tawc_close(fds[--i]);
            return fd;
        }
        fds[i] = (int)fd;
    }
    int reserved[3];
    for (int i = 0; i < 3; ++i) {
        long fd = tawcroot_fd_reserve(fds[i]);
        if (fd < 0) {
            for (int j = i; j < 3; ++j) tawc_close(fds[j]);
            for (int j = 0; j < i; ++j) {
                tawcroot_fd_forget_reserved(reserved[j]);
                tawc_close(reserved[j]);
            }
            return fd;
        }
        reserved[i] = (int)fd;
        (void)tawc_fcntl((int)fd, F_SETFD, 0);
    }
    if (ns->user)
        for (int i = 0; i < 3; ++i) {
            tawcroot_fd_forget_reserved(ns->maps[i]);
            tawc_close(ns->maps[i]);
        }
    memcpy(ns->maps, reserved, sizeof reserved);
    (void)tawc_write(ns->maps[2], "allow\n", 6);
    ns->user = 1;
    ns->capabilities[0] = ns->capabilities[1] = 0xffffffffu;
    ns->capabilities[3] = ns->capabilities[4] = 0x1ff;
    ns->capabilities[2] = ns->capabilities[5] = 0;
    return 0;
}

void tawcroot_namespace_restore(void)
{
    if (tawcroot_namespace.user)
        for (int i = 0; i < 3; ++i)
            (void)tawcroot_fd_record_reserved(tawcroot_namespace.maps[i]);
}

long tawcroot_namespace_prctl(long option, long arg)
{
    struct tawc_namespace *ns = &tawcroot_namespace;
    if (!(ns->session & TAWC_NS_INIT_SESSION)) return TAWC_ENOSYS;
    switch (option) {
    case PR_CAPBSET_READ:
    case PR_CAPBSET_DROP:
        if (arg < 0 || arg >= 64) return TAWC_EINVAL;
        if (option == PR_CAPBSET_READ) return !(ns->capbset_dropped[arg / 32] >> (arg % 32) & 1);
        ns->capbset_dropped[arg / 32] |= 1u << (arg % 32);
        return 0;
    case PR_CAP_AMBIENT:
        return arg == PR_CAP_AMBIENT_IS_SET ? 0 : arg >= 2 && arg <= 4 ? 0 : TAWC_EINVAL;
    case PR_SET_SECUREBITS:
        return 0;
    default:
        return TAWC_ENOSYS;
    }
}

static long fake_pid(long pid)
{
    return pid && pid == tawcroot_namespace.init_pid ? 1 : pid;
}

/* Append `v` with init's pid shown as 1. */
static void put_pid(char *out, size_t cap, size_t *n, long v)
{
    (void)tawc_str_append_dec(out, cap, n, fake_pid(v));
}

/* /proc/<pid>/stat: pid, ppid, pgrp and session fields. */
static size_t rewrite_stat(const char *in, char *out, size_t cap, long pid)
{
    size_t n = 0;
    const char *close = 0;
    for (const char *p = in; *p; ++p) if (*p == ')') close = p;
    if (!close) return 0;
    const char *p = in;
    while (*p >= '0' && *p <= '9') ++p;
    put_pid(out, cap, &n, pid);
    for (const char *q = p; q <= close && n + 1 < cap; ++q) out[n++] = *q;
    out[n] = 0;
    p = close + 1;
    for (int field = 2; *p; ) {
        if (*p == ' ') { if (n + 1 < cap) out[n++] = ' ', out[n] = 0; ++p; ++field; continue; }
        const char *end = p;
        while (*end && *end != ' ') ++end;
        if (field >= 4 && field <= 6) {
            long v = 0;
            for (const char *d = p; d < end && *d >= '0' && *d <= '9'; ++d) v = v * 10 + (*d - '0');
            if (field == 4 && pid == tawcroot_namespace.init_pid) v = 0;
            put_pid(out, cap, &n, v);
        } else {
            for (const char *d = p; d < end && n + 1 < cap; ++d) out[n++] = *d;
            out[n] = 0;
        }
        p = end;
    }
    return n;
}

/* /proc/<pid>/status: the Tgid/Pid/PPid/NS* pid lines. */
static int put_cap(char *out, size_t cap, size_t *n, const char *line, long pid);

static size_t rewrite_status(const char *in, char *out, size_t cap, long pid)
{
    static const char *const keys[] = { "Tgid:", "Pid:", "PPid:", "TracerPid:",
        "NStgid:", "NSpid:", "NSpgid:", "NSsid:" };
    size_t n = 0;
    for (const char *line = in; *line; ) {
        const char *end = line;
        while (*end && *end != '\n') ++end;
        size_t k = 0;
        for (; k < sizeof keys / sizeof keys[0]; ++k) if (tawc_starts_with(line, keys[k])) break;
        if (k == sizeof keys / sizeof keys[0]) {
            if (!put_cap(out, cap, &n, line, pid))
                for (const char *d = line; d < end && n + 1 < cap; ++d) out[n++] = *d;
        } else {
            const char *d = line;
            while (d < end && *d != ':') out[n++] = *d++;
            for (; d < end && n + 1 < cap; ) {
                if (*d < '0' || *d > '9') { out[n++] = *d++; continue; }
                long v = 0;
                while (d < end && *d >= '0' && *d <= '9') v = v * 10 + (*d++ - '0');
                if (k == 2 && pid == tawcroot_namespace.init_pid) v = 0;
                put_pid(out, cap, &n, v);
            }
        }
        if (*end && n + 1 < cap) out[n++] = '\n';
        out[n] = 0;
        line = *end ? end + 1 : end;
    }
    return n;
}

int tawcroot_namespace_is_userns_link(const char *path)
{
    if (!(tawcroot_namespace.session & TAWC_NS_INIT_SESSION) || tawcroot_namespace.user ||
        !tawc_starts_with(path, "/proc/")) return 0;
    const char *p = path + 6;
    if (tawc_starts_with(p, "self/")) p += 5;
    else if (tawc_starts_with(p, "thread-self/")) p += 12;
    else {
        const char *d = p;
        while (*p >= '0' && *p <= '9') ++p;
        if (p == d || *p++ != '/') return 0;
    }
    if (tawc_starts_with(p, "task/")) {
        for (p += 5; *p >= '0' && *p <= '9'; ++p) {}
        if (*p++ != '/') return 0;
    }
    return tawc_streq(p, "ns/user");
}

/* Cap* lines of an init session's /proc/<pid>/status: the shadow set
 * for the caller, the session's default set for other processes. */
static int put_cap(char *out, size_t cap, size_t *n, const char *line, long pid)
{
    static const char *const keys[] = { "CapInh:", "CapPrm:", "CapEff:", "CapBnd:", "CapAmb:" };
    const struct tawc_namespace *ns = &tawcroot_namespace;
    if (!(ns->session & TAWC_NS_INIT_SESSION) || ns->user) return 0;
    int k = 0;
    for (; k < 5; ++k) if (tawc_starts_with(line, keys[k])) break;
    if (k == 5) return 0;
    int self = pid == tawc_getpid();
    uint64_t v = 0, dflt = 0xfdecafffu;
    switch (k) {
    case 0: v = self ? ns->capabilities[2] | (uint64_t)ns->capabilities[5] << 32 : 0; break;
    case 1: v = self ? ns->capabilities[1] | (uint64_t)ns->capabilities[4] << 32 : dflt; break;
    case 2: v = self ? ns->capabilities[0] | (uint64_t)ns->capabilities[3] << 32 : dflt; break;
    case 3: v = self ? ~(ns->capbset_dropped[0] | (uint64_t)ns->capbset_dropped[1] << 32) &
                       0x1ffffffffffULL : dflt; break;
    default: v = 0;
    }
    (void)tawc_str_append(out, cap, n, keys[k]);
    (void)tawc_str_append(out, cap, n, "\t");
    for (int sh = 60; sh >= 0; sh -= 4) {
        if (*n + 1 >= cap) break;
        out[(*n)++] = "0123456789abcdef"[(v >> sh) & 0xf];
    }
    out[*n] = 0;
    return 1;
}

/* With a fake init, /proc/<pid>/stat and status show it as pid 1 and as
 * the parent of what it adopted. Other pids stay host values. */
long tawcroot_namespace_proc_open(const char *path, int flags)
{
    if (!tawcroot_namespace.init_pid || !tawc_starts_with(path, "/proc/") ||
        (flags & O_ACCMODE) != O_RDONLY || (flags & (O_PATH | O_DIRECTORY)))
        return TAWC_ENOSYS;
    const char *p = path + 6;
    long pid = 0;
    if (tawc_starts_with(p, "self/")) { pid = tawc_getpid(); p += 5; }
    else if (tawc_starts_with(p, "thread-self/")) { pid = tawc_getpid(); p += 12; }
    else {
        while (*p >= '0' && *p <= '9') pid = pid * 10 + (*p++ - '0');
        if (!pid || *p++ != '/') return TAWC_ENOSYS;
        if (pid == 1) pid = tawcroot_namespace.init_pid;
    }
    const char *rest = p;
    if (tawc_starts_with(p, "task/")) {
        p += 5;
        while (*p >= '0' && *p <= '9') ++p;
        if (*p++ != '/') return TAWC_ENOSYS;
    }
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    /* An init session looks like a user-namespaced container: root
     * maps to the app's single id, so inits skip what needs real root. */
    int map = tawc_streq(p, "uid_map") ? 1 : tawc_streq(p, "gid_map") ? 2 :
        tawc_streq(p, "setgroups") ? 3 : 0;
    if (map && (tawcroot_namespace.session & TAWC_NS_INIT_SESSION)) {
        char *text = scratch->buf[0];
        size_t n = 0;
        if (map == 3) (void)tawc_str_append(text, 64, &n, "deny\n");
        else {
            long id = TAWC_RAW(map == 1 ? TAWC_SYS_getuid : TAWC_SYS_getgid, 0, 0, 0, 0, 0, 0);
            long v[3] = { 0, id, 1 };
            for (int i = 0; i < 3; ++i) {
                char num[16];
                size_t k = 0;
                (void)tawc_str_append_dec(num, sizeof num, &k, v[i]);
                for (size_t pad = k; pad < 10; ++pad) text[n++] = ' ';
                for (size_t j = 0; j < k; ++j) text[n++] = num[j];
                text[n++] = i < 2 ? ' ' : '\n';
            }
        }
        long copy = tawc_memfd_create("tawcroot-map", (flags & O_CLOEXEC) ? 1 : 0);
        if (copy < 0) return copy;
        if (tawc_write((int)copy, text, n) != (long)n) { tawc_close((int)copy); return TAWC_EIO; }
        (void)tawc_lseek((int)copy, 0, SEEK_SET);
        return copy;
    }
    int status = tawc_streq(p, "status");
    if (!status && !tawc_streq(p, "stat")) return TAWC_ENOSYS;
    char *real = scratch->buf[0], *in = scratch->buf[1], *out = scratch->buf[2];
    size_t n = 0;
    if (tawc_str_append(real, TAWCROOT_PATH_SCRATCH_SIZE, &n, "/proc/") ||
        tawc_str_append_dec(real, TAWCROOT_PATH_SCRATCH_SIZE, &n, pid) ||
        tawc_str_append(real, TAWCROOT_PATH_SCRATCH_SIZE, &n, "/") ||
        tawc_str_append(real, TAWCROOT_PATH_SCRATCH_SIZE, &n, rest))
        return TAWC_ENAMETOOLONG;
    long fd = tawc_openat(AT_FDCWD, real, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return fd;
    long len = tawc_read((int)fd, in, TAWCROOT_PATH_SCRATCH_SIZE - 1);
    tawc_close((int)fd);
    if (len < 0) return len;
    in[len] = 0;
    size_t m = status ? rewrite_status(in, out, TAWCROOT_PATH_SCRATCH_SIZE, pid)
                      : rewrite_stat(in, out, TAWCROOT_PATH_SCRATCH_SIZE, pid);
    long copy = tawc_memfd_create("tawcroot-proc", (flags & O_CLOEXEC) ? 1 : 0);
    if (copy < 0) return copy;
    if (tawc_write((int)copy, out, m) != (long)m) { tawc_close((int)copy); return TAWC_EIO; }
    (void)tawc_lseek((int)copy, 0, SEEK_SET);
    return copy;
}

long tawcroot_namespace_open(const char *path, int flags)
{
    if (!tawcroot_namespace.user || !tawc_starts_with(path, "/proc/")) return TAWC_ENOSYS;
    const char *leaf = path + 6;
    if (tawc_starts_with(leaf, "self/")) leaf += 5;
    else {
        long pid = 0;
        while (*leaf >= '0' && *leaf <= '9') {
            pid = pid * 10 + *leaf++ - '0';
            if (pid > 0x7fffffff) return TAWC_ENOSYS;
        }
        if (*leaf != '/' || (pid != tawc_getpid() &&
            !(pid == 1 && tawcroot_namespace.init_pid == tawc_getpid()))) return TAWC_ENOSYS;
        ++leaf;
    }
    int kind = tawc_streq(leaf, "uid_map") ? 0 : tawc_streq(leaf, "gid_map") ? 1 :
        tawc_streq(leaf, "setgroups") ? 2 : -1;
    if (kind < 0) return TAWC_ENOSYS;
    if (flags & (O_CREAT | O_EXCL | O_DIRECTORY | O_PATH | O_TRUNC)) return TAWC_EINVAL;
    int fd = tawcroot_namespace.maps[kind];
    if ((flags & O_ACCMODE) == O_RDONLY) {
        char data[128];
        long n = TAWC_RAW(TAWC_SYS_pread64, fd, (long)data, sizeof data, 0, 0, 0);
        if (n < 0) return n;
        long copy = tawc_memfd_create("tawcroot-map-read", 2 | ((flags & O_CLOEXEC) ? 1 : 0));
        if (copy < 0) return copy;
        long written = tawc_write((int)copy, data, (size_t)n);
        if (written != n) { tawc_close((int)copy); return written < 0 ? written : TAWC_EIO; }
        (void)tawc_lseek((int)copy, 0, SEEK_SET);
        (void)tawc_fcntl((int)copy, 1033, 8); /* F_ADD_SEALS, F_SEAL_WRITE */
        return copy;
    }
    if ((flags & O_ACCMODE) != O_WRONLY) return TAWC_EINVAL;
    (void)tawc_lseek(fd, 0, SEEK_SET);
    return tawc_fcntl(fd, (flags & O_CLOEXEC) ? F_DUPFD_CLOEXEC : F_DUPFD, 0);
}

/* Make the calling process the fake PID namespace init. Subreaper so
 * orphans reparent here (their getppid() then maps to 1) and wait()
 * reaps them, as under a real init. */
static long become_init(void)
{
    if (!tawcroot_namespace.init_pid) {
        long trapped = tawcroot_install_lazy_filter(pid_lazy, sizeof pid_lazy / sizeof pid_lazy[0]);
        if (trapped < 0) return trapped;
    }
    long r = tawc_prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
    if (r < 0) return r;
    tawcroot_namespace.init_pid = (int)tawc_getpid();
    return 0;
}

long tawcroot_namespace_init_session(void)
{
    long r = become_init();
    if (r) return r;
    /* A real init starts with every capability; services drop from
     * there. Emulated like a user namespace's (namespace_cap). */
    struct tawc_namespace *ns = &tawcroot_namespace;
    static const int init_lazy[] = { TAWC_SYS_fstatfs };
    r = tawcroot_install_lazy_filter(init_lazy, sizeof init_lazy / sizeof init_lazy[0]);
    if (r) return r;
    ns->session |= TAWC_NS_INIT_SESSION;
    if (!ns->user) {
        /* systemd-nspawn's default set: no SYS_MODULE, SYS_RAWIO,
         * SYS_TIME, MAC_*, ... so units needing those skip cleanly. */
        ns->capabilities[0] = ns->capabilities[1] = 0xfdecafffu;
        ns->capabilities[3] = ns->capabilities[4] = 0;
        ns->capabilities[2] = ns->capabilities[5] = 0;
        ns->capbset_dropped[0] = ~0xfdecafffu;
        ns->capbset_dropped[1] = ~0u;
    }
    return 0;
}

/* No pid namespace tears init's children down with it, so its direct
 * children (services) at least die with it. */
void tawcroot_namespace_tie_to_init(void)
{
    long init = tawcroot_namespace.init_pid;
    if (!(tawcroot_namespace.session & TAWC_NS_INIT_SESSION) || !init ||
        tawc_getppid() != init)
        return;
    if (tawc_prctl(PR_SET_PDEATHSIG, 9 /* SIGKILL */, 0, 0, 0) < 0 ||
        tawc_getppid() != init)
        tawc_exit_group(137);
}

static long clone_namespace(const tawcroot_syscall_args *a, ucontext_t *uc)
{
    unsigned long flags = (unsigned long)a->a;
    if (flags & (CLONE_VM | CLONE_THREAD)) return TAWC_EINVAL;
    if ((flags & CLONE_NEWUSER) && (flags & CLONE_FS)) return TAWC_EINVAL;
    if (flags & CLONE_FS) {
        long error = tawcroot_fs_share();
        if (error < 0) return error;
    }
    long parent = tawcroot_namespace.session & TAWC_NS_CGROUP ? tawc_getpid() : 0;
    long result = TAWC_RAW(TAWC_SYS_clone, flags & ~NS_FLAGS, 0, a->c, a->d, a->e, 0);
    if (!result) {
        tawc_usercopy_refresh_pid();
        tawcroot_cgroup_fork_child(parent);
        tawcroot_namespace_tie_to_init();
        if (!(flags & CLONE_FS)) tawcroot_fs_detach();
        if ((flags & CLONE_NEWPID) && become_init() < 0) tawc_exit_group(125);
        if ((flags & CLONE_NEWUSER) && new_user() < 0) tawc_exit_group(125);
        if (a->b) {
#if defined(__aarch64__)
            uc->uc_mcontext.sp = (unsigned long)a->b;
#else
            uc->uc_mcontext.gregs[REG_RSP] = a->b;
#endif
        }
    }
    return result;
}

static long unshare_namespace(const tawcroot_syscall_args *a, ucontext_t *uc)
{
    (void)uc;
    unsigned long flags = (unsigned long)a->a;
    /* An init's services expect EPERM for namespaces they can't get and
     * then run unsandboxed ("assuming containerized execution"). */
    if (flags & ~(CLONE_NEWUSER | CLONE_NEWNET | CLONE_FS))
        return tawcroot_namespace.session & TAWC_NS_INIT_SESSION ? TAWC_EPERM : TAWC_EINVAL;
    long result = (flags & CLONE_FS) ? TAWC_RAW(TAWC_SYS_unshare, CLONE_FS, 0, 0, 0, 0, 0) : 0;
    if (!result && (flags & CLONE_FS)) tawcroot_fs_detach();
    if (!result && (flags & CLONE_NEWUSER)) result = new_user();
    return result;
}

static long namespace_pid(const tawcroot_syscall_args *a, ucontext_t *uc)
{
    (void)uc;
    long pid = TAWC_RAW(a->nr, 0, 0, 0, 0, 0, 0);
    int init = tawcroot_namespace.init_pid;
    if (!init) return pid;
    if (a->nr == TAWC_SYS_getppid && tawc_getpid() == init) return 0;
    return pid == init ? 1 : pid;
}

static long namespace_cap(const tawcroot_syscall_args *a, ucontext_t *uc)
{
    (void)uc;
    if (!tawcroot_namespace.user && !(tawcroot_namespace.session & TAWC_NS_INIT_SESSION))
        return TAWC_RAW(a->nr, a->a, a->b, 0, 0, 0, 0);
    struct __user_cap_header_struct header;
    if (tawc_copy_from_guest(&header, sizeof header, (void *)a->a) < 0) return TAWC_EFAULT;
    if (header.version != _LINUX_CAPABILITY_VERSION_3 && header.version != _LINUX_CAPABILITY_VERSION_2)
        return TAWC_EINVAL;
    if (header.pid && header.pid != tawc_getpid() &&
        !(header.pid == 1 && tawcroot_namespace.init_pid == tawc_getpid())) return TAWC_ESRCH;
    if (a->nr == __NR_capget)
        return tawc_copy_to_guest((void *)a->b, tawcroot_namespace.capabilities, sizeof tawcroot_namespace.capabilities);
    uint32_t next[6];
    if (tawc_copy_from_guest(next, sizeof next, (void *)a->b) < 0) return TAWC_EFAULT;
    for (int i = 0; i < 6; i += 3)
        if ((next[i] & ~next[i+1]) || (next[i+1] & ~tawcroot_namespace.capabilities[i+1]) ||
            (next[i+2] & ~(tawcroot_namespace.capabilities[i+2] | tawcroot_namespace.capabilities[i+1])))
            return TAWC_EPERM;
    memcpy(tawcroot_namespace.capabilities, next, sizeof next);
    return 0;
}

static int valid_extent(const char *text)
{
    uint64_t values[3];
    for (int i = 0; i < 3; ++i) {
        while (*text == ' ' || *text == '\t' || *text == '\n') ++text;
        if (*text < '0' || *text > '9') return 0;
        uint64_t value = 0;
        while (*text >= '0' && *text <= '9') {
            value = value * 10 + *text++ - '0';
            if (value > UINT32_MAX) return 0;
        }
        values[i] = value;
    }
    while (*text == ' ' || *text == '\t' || *text == '\n') ++text;
    return !*text && values[2] && values[0] + values[2] <= UINT32_MAX &&
        values[1] + values[2] <= UINT32_MAX;
}

static long namespace_write(const tawcroot_syscall_args *a, ucontext_t *uc)
{
    (void)uc;
    long cg = tawcroot_cgroup_write((int)a->a, (const void *)a->b, (size_t)a->c);
    if (cg != TAWC_ENOSYS) return cg;
    if (tawcroot_namespace.user) {
        struct stat st;
        if (!TAWC_RAW(TAWC_SYS_fstat, a->a, (long)&st, 0, 0, 0, 0))
            for (int i = 0; i < 3; ++i) {
                struct stat other;
                if (TAWC_RAW(TAWC_SYS_fstat, tawcroot_namespace.maps[i], (long)&other, 0, 0, 0, 0) ||
                    st.st_dev != other.st_dev || st.st_ino != other.st_ino) continue;
                char text[128];
                if (a->c <= 0 || a->c >= (long)sizeof text) return TAWC_EINVAL;
                if (tawc_copy_from_guest(text, (size_t)a->c, (void *)a->b) < 0) return TAWC_EFAULT;
                text[a->c] = 0;
                if (strlen(text) != (size_t)a->c) return TAWC_EINVAL;
                if (i < 2) {
                    if (other.st_size) return TAWC_EPERM;
                    if (!valid_extent(text)) return TAWC_EINVAL;
                } else if (!tawc_streq(text, "deny") && !tawc_streq(text, "deny\n")) return TAWC_EINVAL;
                long result = tawc_write((int)a->a, text, (size_t)a->c);
                if (result > 0) (void)tawc_fcntl(tawcroot_namespace.maps[i], 1033, 8);
                return result;
            }
    }
    return TAWC_RAW(TAWC_SYS_write, a->a, a->b, a->c, 0, 0, 0);
}

/* Whether `pid` has a handler for `sig`, from /proc/<pid>/status SigCgt.
 * Guest handlers are real kernel handlers (only SIGSYS is ours). Read in
 * small chunks, carrying a tail so the field can straddle two reads. */
static int catches(int pid, int sig)
{
    char path[32] = "/proc/";
    size_t n = 6;
    if (tawc_str_append_dec(path, sizeof path, &n, pid) < 0 ||
        tawc_str_append(path, sizeof path, &n, "/status") < 0) return 1;
    long fd = tawc_openat(AT_FDCWD, path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return 1;
    enum { TAIL = 32 };
    char buf[TAIL + 256 + 1];
    size_t have = 0;
    const char *field = 0;
    for (;;) {
        long len = TAWC_RAW(TAWC_SYS_read, fd, (long)(buf + have), sizeof buf - 1 - have, 0, 0, 0);
        if (len <= 0) break;
        have += (size_t)len;
        buf[have] = 0;
        for (const char *p = buf; *p; ++p)
            if ((p == buf || p[-1] == '\n') && tawc_starts_with(p, "SigCgt:") &&
                tawc_strlen(p) >= TAIL - 1) { field = p + 7; break; }
        if (field) break;
        if (have > TAIL) {
            memmove(buf, buf + have - TAIL, TAIL);
            have = TAIL;
        }
    }
    tawc_close((int)fd);
    if (!field) return 1;
    while (*field == ' ' || *field == '\t') ++field;
    uint64_t mask = 0;
    for (;; ++field) {
        int d = *field >= '0' && *field <= '9' ? *field - '0' :
            *field >= 'a' && *field <= 'f' ? *field - 'a' + 10 : -1;
        if (d < 0) break;
        mask = mask << 4 | (uint64_t)d;
    }
    return (int)(mask >> (sig - 1) & 1);
}

/* ppid and pgrp from /proc/<pid>/stat. */
static long stat_ids(long pid, long *ppid, long *pgrp)
{
    char buf[256];
    size_t n = 0;
    if (tawc_str_append(buf, sizeof buf, &n, "/proc/") ||
        tawc_str_append_dec(buf, sizeof buf, &n, pid) ||
        tawc_str_append(buf, sizeof buf, &n, "/stat")) return TAWC_ESRCH;
    long fd = tawc_openat(AT_FDCWD, buf, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return TAWC_ESRCH;
    long len = tawc_read((int)fd, buf, sizeof buf - 1);
    tawc_close((int)fd);
    if (len <= 0) return TAWC_ESRCH;
    buf[len] = 0;
    const char *p = 0;
    for (long i = 0; i < len; ++i) if (buf[i] == ')') p = buf + i + 1;
    if (!p) return TAWC_ESRCH;
    /* " S ppid pgrp ..." */
    long v[2] = { 0, 0 };
    int field = 0;
    for (; *p && field < 4; ++p) {
        if (*p == ' ') { ++field; continue; }
        if (field >= 2 && *p >= '0' && *p <= '9') v[field - 2] = v[field - 2] * 10 + (*p - '0');
    }
    if (field < 3) return TAWC_ESRCH;
    *ppid = v[0];
    *pgrp = v[1];
    return 0;
}

/* The init or one of its descendants. Orphans reparent to the init (a
 * subreaper), so every guest process keeps a parent chain to it. */
int tawcroot_namespace_is_guest(long pid)
{
    long init = tawcroot_namespace.init_pid;
    if (!init) return 1;
    for (int depth = 0; depth < 128 && pid > 1; ++depth) {
        if (pid == init) return 1;
        long ppid, pgrp;
        if (stat_ids(pid, &ppid, &pgrp)) return 0;
        pid = ppid;
    }
    return 0;
}

/* A thread id of a guest process: a guest tgid, or one of init's
 * threads (whose /proc/<tid>/stat ppid is init's real parent). */
static int guest_tid(long tid)
{
    if (tawcroot_namespace_is_guest(tid)) return 1;
    char path[64] = "/proc/";
    size_t n = 6;
    if (tawc_str_append_dec(path, sizeof path, &n, tawcroot_namespace.init_pid) ||
        tawc_str_append(path, sizeof path, &n, "/task/") ||
        tawc_str_append_dec(path, sizeof path, &n, tid)) return 0;
    return TAWC_RAW(TAWC_SYS_faccessat, AT_FDCWD, (long)path, 0, 0, 0, 0) == 0;
}

/* Init drops signals it has no handler for, as a namespace init does
 * for senders inside; SIGKILL and SIGSTOP included. */
static int init_drops(long pid, long sig)
{
    int init = tawcroot_namespace.init_pid;
    return pid == init && sig > 0 && sig < 65 && !catches(init, (int)sig);
}

/* kill(0), kill(-pgrp) and kill(-1) over guest processes only: without
 * a pid namespace the kernel's versions reach the app's own processes. */
static long kill_many(long pgrp, long sig)
{
    long self = tawc_getpid(), init = tawcroot_namespace.init_pid;
    long dir = tawc_openat(AT_FDCWD, "/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (dir < 0) return dir;
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    char *buf = scratch->buf[0];
    long sent = 0, err = TAWC_ESRCH;
    for (;;) {
        long n = TAWC_RAW(TAWC_SYS_getdents64, dir, (long)buf, TAWCROOT_PATH_SCRATCH_SIZE, 0, 0, 0);
        if (n <= 0) break;
        for (long off = 0; off < n;) {
            unsigned short reclen;
            memcpy(&reclen, buf + off + 16, sizeof reclen);
            const char *name = buf + off + 19;
            off += reclen;
            long pid = 0;
            const char *c = name;
            for (; *c >= '0' && *c <= '9'; ++c) pid = pid * 10 + (*c - '0');
            if (*c || !pid) continue;
            long ppid, g;
            if (pgrp == -1) {
                if (pid == self || pid == init) continue;
            } else if (stat_ids(pid, &ppid, &g) || g != pgrp) continue;
            if (!tawcroot_namespace_is_guest(pid)) continue;
            if (init_drops(pid, sig)) { sent = 1; continue; }
            long r = TAWC_RAW(__NR_kill, pid, sig, 0, 0, 0, 0);
            if (!r) sent = 1;
            else if (r == TAWC_EPERM) err = r;
        }
    }
    tawc_close((int)dir);
    return sent ? 0 : err;
}

/* Signals from guests reach guests only, with pid 1 mapped to the fake
 * init. Like a real namespace init, the init only gets signals it has a
 * handler for. Signal 0 still probes. */
static long namespace_signal(const tawcroot_syscall_args *a, ucontext_t *uc)
{
    (void)uc;
    long init = tawcroot_namespace.init_pid;
    long pid = a->a, tid = a->b;
    if (init && pid == 1) pid = init;
    if (init && tid == 1 && (a->nr == __NR_tgkill || a->nr == __NR_rt_tgsigqueueinfo)) tid = init;
    if (!init) return TAWC_RAW(a->nr, a->a, a->b, a->c, a->d, 0, 0);
    switch (a->nr) {
    case __NR_kill:
        if (pid == -1) return kill_many(-1, a->b);
        if (pid <= 0) return kill_many(pid ? -pid : TAWC_RAW(__NR_getpgid, 0, 0, 0, 0, 0, 0), a->b);
        if (!tawcroot_namespace_is_guest(pid)) return TAWC_ESRCH;
        if (init_drops(pid, a->b)) {
            long probe = TAWC_RAW(__NR_kill, pid, 0, 0, 0, 0, 0);
            return probe < 0 ? probe : 0;
        }
        return TAWC_RAW(__NR_kill, pid, a->b, 0, 0, 0, 0);
    case __NR_tkill:
        if (!guest_tid(pid)) return TAWC_ESRCH;
        if (pid == init && init_drops(pid, a->b)) return 0;
        return TAWC_RAW(__NR_tkill, pid, a->b, 0, 0, 0, 0);
    case __NR_tgkill:
    case __NR_rt_tgsigqueueinfo:
        if (pid <= 0 || !tawcroot_namespace_is_guest(pid)) return pid <= 0 ? TAWC_EINVAL : TAWC_ESRCH;
        if (tid == init && init_drops(pid, a->c)) {
            long probe = TAWC_RAW(__NR_tgkill, pid, tid, 0, 0, 0, 0);
            return probe < 0 ? probe : 0;
        }
        return TAWC_RAW(a->nr, pid, tid, a->c, a->d, 0, 0);
    case __NR_rt_sigqueueinfo:
        if (pid <= 0 || !tawcroot_namespace_is_guest(pid)) return TAWC_ESRCH;
        if (init_drops(pid, a->b)) return 0;
        return TAWC_RAW(a->nr, pid, a->b, a->c, 0, 0, 0);
    default: /* pidfd_open */
        if (pid <= 0 || !tawcroot_namespace_is_guest(pid)) return pid <= 0 ? TAWC_EINVAL : TAWC_ESRCH;
        return TAWC_RAW(a->nr, pid, a->b, 0, 0, 0, 0);
    }
}

void tawcroot_namespace_register(void)
{
    tawcroot_dispatch_install(TAWC_SYS_clone, clone_namespace);
    tawcroot_dispatch_install(TAWC_SYS_unshare, unshare_namespace);
    tawcroot_dispatch_install_lazy(TAWC_SYS_getpid, namespace_pid);
    tawcroot_dispatch_install_lazy(TAWC_SYS_getppid, namespace_pid);
    tawcroot_dispatch_install(__NR_capget, namespace_cap);
    tawcroot_dispatch_install(__NR_capset, namespace_cap);
    tawcroot_dispatch_install_lazy(TAWC_SYS_write, namespace_write);
    tawcroot_dispatch_install_lazy(__NR_kill, namespace_signal);
    tawcroot_dispatch_install_lazy(__NR_tgkill, namespace_signal);
    tawcroot_dispatch_install_lazy(__NR_tkill, namespace_signal);
    tawcroot_dispatch_install_lazy(__NR_rt_sigqueueinfo, namespace_signal);
    tawcroot_dispatch_install_lazy(__NR_epoll_ctl, tawcroot_mounts_epoll_ctl);
    tawcroot_dispatch_install_lazy(TAWC_SYS_fstatfs, tawcroot_mounts_fstatfs);
    tawcroot_dispatch_install_lazy(__NR_rt_tgsigqueueinfo, namespace_signal);
    /* Kernels with a broken pidfd_open keep their base-filter deny. */
    if (!tawcroot_dispatch_get(TAWC_SYS_pidfd_open))
        tawcroot_dispatch_install_lazy(TAWC_SYS_pidfd_open, namespace_signal);
}
