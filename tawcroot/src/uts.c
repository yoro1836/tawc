/* Emulated hostname for `--init` sessions. Android gives the app no
 * UTS namespace and no sethostname(2), but an init sets the hostname
 * from /etc/hostname. The session keeps its own names in <store>/uts,
 * shared by every guest process: sethostname/setdomainname write it,
 * uname and /proc/sys/kernel/{hostname,domainname} read it. */
#include <stdint.h>
#include <fcntl.h>
#include <asm/unistd.h>
#include "uts.h"
#include "errno_neg.h"
#include "io.h"
#include "linkstore.h"
#include "mounts.h"
#include "namespace.h"
#include "path_scratch.h"
#include "raw_sys.h"
#include "sysnr.h"
#include "tawc_string.h"
#include "usercopy.h"

#define LEN 65   /* __NEW_UTS_LEN + 1 */

/* struct new_utsname: sysname, nodename, release, version, machine,
 * domainname. */
struct names { char host[LEN], domain[LEN]; };

static const char STORE[] = "/uts";

static long open_store(int flags)
{
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    char *path = scratch->buf[0];
    size_t n = 0;
    if (!tawcroot_store_host_path_len) return TAWC_ENOENT;
    if (tawc_str_append(path, TAWCROOT_PATH_SCRATCH_SIZE, &n, tawcroot_store_host_path) ||
        tawc_str_append(path, TAWCROOT_PATH_SCRATCH_SIZE, &n, STORE))
        return TAWC_ENAMETOOLONG;
    return tawc_openat(AT_FDCWD, path, flags | O_CLOEXEC | O_NOFOLLOW, 0600);
}

static int load(struct names *n)
{
    long fd = open_store(O_RDONLY);
    if (fd < 0) return 0;
    long got = tawc_pread64((int)fd, n, sizeof *n, 0);
    tawc_close((int)fd);
    n->host[LEN - 1] = n->domain[LEN - 1] = 0;
    return got == (long)sizeof *n;
}

static long save(const struct names *n)
{
    long fd = open_store(O_WRONLY | O_CREAT);
    if (fd < 0) return fd;
    long put = TAWC_RAW(__NR_pwrite64, fd, (long)n, sizeof *n, 0, 0, 0);
    tawc_close((int)fd);
    return put == (long)sizeof *n ? 0 : TAWC_EIO;
}

void tawcroot_uts_setup(void)
{
    char uts[6][LEN];
    struct names n;
    memset(&n, 0, sizeof n);
    if (TAWC_RAW(TAWC_SYS_uname, (long)uts, 0, 0, 0, 0, 0) == 0) {
        (void)tawc_str_copy(n.host, LEN, uts[1]);
        (void)tawc_str_copy(n.domain, LEN, uts[5]);
    }
    (void)save(&n);
}

static int session(void)
{
    return (tawcroot_namespace.session & TAWC_NS_INIT_SESSION) != 0;
}

long tawcroot_uts_syscall(const tawcroot_syscall_args *a, ucontext_t *uc)
{
    (void)uc;
    struct names n;
    if (a->nr == TAWC_SYS_uname) {
        tawcroot_mounts_drain(STORE);
        long rv = TAWC_RAW(TAWC_SYS_uname, a->a, 0, 0, 0, 0, 0);
        if (rv || !session() || !load(&n)) return rv;
        char *uts = (char *)(uintptr_t)a->a;   /* kernel-validated */
        if (tawc_copy_to_guest(uts + LEN, n.host, LEN) < 0 ||
            tawc_copy_to_guest(uts + 5 * LEN, n.domain, LEN) < 0)
            return TAWC_EFAULT;
        return 0;
    }
    /* sethostname / setdomainname */
    if (!session()) return TAWC_EPERM;
    long len = (long)a->b;
    if (len < 0 || len >= LEN) return TAWC_EINVAL;
    char name[LEN];
    memset(name, 0, sizeof name);
    if (len && tawc_copy_from_guest(name, (size_t)len, (const void *)(uintptr_t)a->a) < 0)
        return TAWC_EFAULT;
    if (!load(&n)) memset(&n, 0, sizeof n);
    memcpy(a->nr == __NR_sethostname ? n.host : n.domain, name, LEN);
    return save(&n);
}

long tawcroot_uts_proc_open(const char *path, int flags)
{
    int host;
    if (tawc_streq(path, "/proc/sys/kernel/hostname")) host = 1;
    else if (tawc_streq(path, "/proc/sys/kernel/domainname")) host = 0;
    else return TAWC_ENOSYS;
    struct names n;
    if ((flags & O_ACCMODE) != O_RDONLY || !load(&n)) return TAWC_ENOSYS;
    long fd = tawc_memfd_create("tawcroot-uts", (flags & O_CLOEXEC) ? 1 : 0);
    if (fd < 0) return fd;
    const char *s = host ? n.host : n.domain;
    size_t len = tawc_strlen(s);
    if (tawc_write((int)fd, s, len) != (long)len || tawc_write((int)fd, "\n", 1) != 1) {
        tawc_close((int)fd);
        return TAWC_EIO;
    }
    (void)tawc_lseek((int)fd, 0, 0 /* SEEK_SET */);
    /* journald polls this fd for hostname changes. */
    return tawcroot_mounts_watchable(fd, (flags & O_CLOEXEC) != 0, STORE);
}
