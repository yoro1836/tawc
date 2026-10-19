/* Privileged ports for `--init` sessions. An app can't bind ports below
 * 1024, but services do (systemd-resolved's stub on 127.0.0.53:53).
 * With no network namespace, guest port p < 1024 maps to host port
 * p + TAWCROOT_PORT_SHIFT: binds shift on any address, connects and
 * sends shift for loopback destinations, and names coming back
 * (getsockname, getpeername, accept, recvfrom/recvmsg sources) shift
 * back. Datagram sockets live above the reserved fd floor, where a
 * stacked filter traps their receives; stream receives carry no
 * address and stay untrapped. */
#include <stdint.h>
#include <fcntl.h>
#include <asm/unistd.h>
#include "ports.h"
#include "errno_neg.h"
#include "fdtab.h"
#include "filter.h"
#include "io.h"
#include "namespace.h"
#include "raw_sys.h"
#include "sysnr.h"
#include "tawc_string.h"
#include "usercopy.h"

#define INET 2
#define INET6 10
#define DGRAM 2
#define CLOEXEC 02000000

/* struct msghdr / mmsghdr, 64-bit. */
struct mhdr {
    uint64_t name;
    uint32_t namelen, pad;
    uint64_t iov, iovlen, control, controllen;
    int32_t flags;
    uint32_t pad2;
};

static int session(void)
{
    return (tawcroot_namespace.session & TAWC_NS_INIT_SESSION) != 0;
}

static int loopback(const unsigned char *sa, long len)
{
    if (sa[0] == INET && len >= 8) return sa[4] == 127;
    if (len < 24) return 0;
    static const unsigned char v4mapped[12] = { 0,0,0,0, 0,0,0,0, 0,0,0xff,0xff };
    const unsigned char *a = sa + 8;
    if (!memcmp(a, v4mapped, 12)) return a[12] == 127;
    for (int i = 0; i < 15; ++i) if (a[i]) return 0;
    return a[15] == 1;
}

int tawcroot_ports_rewrite(void *p, long len, int dir)
{
    unsigned char *sa = p;
    if (!session() || len < 4 || sa[1] != 0 || (sa[0] != INET && sa[0] != INET6)) return 0;
    unsigned port = (unsigned)sa[2] << 8 | sa[3];
    if (dir == TAWC_PORTS_BIND || dir == TAWC_PORTS_TO) {
        if (!port || port >= 1024 || (dir == TAWC_PORTS_TO && !loopback(sa, len))) return 0;
        port += TAWCROOT_PORT_SHIFT;
    } else {
        if (port <= TAWCROOT_PORT_SHIFT || port >= TAWCROOT_PORT_SHIFT + 1024 ||
            (dir == TAWC_PORTS_PEER && !loopback(sa, len))) return 0;
        port -= TAWCROOT_PORT_SHIFT;
    }
    sa[2] = (unsigned char)(port >> 8);
    sa[3] = (unsigned char)port;
    return 1;
}

/* Copy a guest sockaddr and shift it; 1 if shifted. */
static long fetch(unsigned char *buf, uint64_t gaddr, long len, int dir)
{
    if (!gaddr || len < 4 || len > 128) return 0;
    if (tawc_copy_from_guest(buf, (size_t)len, (const void *)(uintptr_t)gaddr) < 0) return 0;
    return tawcroot_ports_rewrite(buf, len, dir);
}

long tawcroot_ports_call(const tawcroot_syscall_args *a)
{
    if (!session()) return TAWC_ENOSYS;
    unsigned char sa[128];
    switch (a->nr) {
    case TAWC_SYS_bind:
        if (!fetch(sa, (uint64_t)a->b, a->c, TAWC_PORTS_BIND)) return TAWC_ENOSYS;
        return TAWC_RAW(a->nr, a->a, (long)sa, a->c, 0, 0, 0);
    case TAWC_SYS_connect:
        if (!fetch(sa, (uint64_t)a->b, a->c, TAWC_PORTS_TO)) return TAWC_ENOSYS;
        return TAWC_RAW(a->nr, a->a, (long)sa, a->c, 0, 0, 0);
    case TAWC_SYS_sendto:
        if (!fetch(sa, (uint64_t)a->e, a->f, TAWC_PORTS_TO)) return TAWC_ENOSYS;
        return TAWC_RAW(a->nr, a->a, a->b, a->c, a->d, (long)sa, a->f);
    case TAWC_SYS_sendmsg: {
        struct mhdr mh;
        if (!a->b || tawc_copy_from_guest(&mh, sizeof mh, (const void *)(uintptr_t)a->b) < 0 ||
            !fetch(sa, mh.name, mh.namelen, TAWC_PORTS_TO))
            return TAWC_ENOSYS;
        mh.name = (uint64_t)(uintptr_t)sa;
        return TAWC_RAW(a->nr, a->a, (long)&mh, a->c, 0, 0, 0);
    }
    }
    return TAWC_ENOSYS;
}

long tawcroot_ports_socket(long fd, long domain, long type)
{
    if (fd < 0 || !session() || (domain != INET && domain != INET6) || (type & 0xf) != DGRAM)
        return fd;
    if (!(tawcroot_namespace.session & TAWC_NS_RECV_TRAP)) {
        if (tawcroot_install_fd_floor_filter(TAWC_SYS_recvfrom, 0) ||
            tawcroot_install_fd_floor_filter(TAWC_SYS_recvmsg, 0) ||
            tawcroot_install_fd_floor_filter(__NR_recvmmsg, 0))
            return fd;
        tawcroot_namespace.session |= TAWC_NS_RECV_TRAP;
    }
    long hi = tawc_fcntl((int)fd, (type & CLOEXEC) ? F_DUPFD_CLOEXEC : F_DUPFD,
                         TAWCROOT_RESERVED_FD_BASE);
    if (hi < 0) return fd;
    tawc_close((int)fd);
    return hi;
}

/* Unshift a source address the kernel wrote into guest memory. */
static void unshift(uint64_t gaddr, uint64_t glenp, uint32_t len)
{
    unsigned char sa[128];
    if (!gaddr || len < 4) return;
    if (glenp) {
        uint32_t got;
        if (tawc_copy_from_guest(&got, sizeof got, (const void *)(uintptr_t)glenp) < 0) return;
        if (got < len) len = got;
    }
    if (len > sizeof sa) len = sizeof sa;
    if (tawc_copy_from_guest(sa, len, (const void *)(uintptr_t)gaddr) < 0) return;
    if (tawcroot_ports_rewrite(sa, len, TAWC_PORTS_PEER))
        (void)tawc_copy_to_guest((void *)(uintptr_t)gaddr, sa, 4);
}

static void unshift_msg(uint64_t gmsg)
{
    struct mhdr mh;
    if (tawc_copy_from_guest(&mh, sizeof mh, (const void *)(uintptr_t)gmsg) == 0)
        unshift(mh.name, 0, mh.namelen);
}

long tawcroot_ports_recv(const tawcroot_syscall_args *a, ucontext_t *uc)
{
    (void)uc;
    if (tawcroot_fd_is_reserved((int)a->a)) return TAWC_EBADF;
    long rv = TAWC_RAW(a->nr, a->a, a->b, a->c, a->d, a->e, a->f);
    if (rv < 0 || !session()) return rv;
    if (a->nr == TAWC_SYS_recvfrom) {
        unshift((uint64_t)a->e, (uint64_t)a->f, 128);
    } else if (a->nr == TAWC_SYS_recvmsg) {
        if (a->b) unshift_msg((uint64_t)a->b);
    } else if (a->b) {
        for (long i = 0; i < rv; ++i)
            unshift_msg((uint64_t)a->b + (uint64_t)i * (sizeof(struct mhdr) + 8));
    }
    return rv;
}
