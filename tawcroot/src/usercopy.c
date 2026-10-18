/* See include/usercopy.h. */

#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/uio.h>

#include "errno_neg.h"
#include "usercopy.h"
#include "raw_sys.h"
#include "sysnr.h"

int tawc_usercopy_works = 0;

#define TAWC_MADV_WIPEONFORK 18

/* process_vm_{read,write}v address our VM through any task in our
 * thread group, so the tgid works for every thread. It is cached on a
 * MADV_WIPEONFORK page: a forked child (new VM) reads 0 and falls back
 * to a per-call gettid, which is always right, and only writers that
 * are known to own a fresh VM refill it (init, the trapped-clone
 * child). Never refill lazily on a 0 read: a vfork child shares this
 * page with its parent and would leave its own pid behind after its
 * execve. */
static volatile long *g_self_pid;

static long current_task_id(void)
{
	long t = TAWC_RAW(TAWC_SYS_gettid, 0, 0, 0, 0, 0, 0);
	return t > 0 ? t : 0;
}

static long self_task(void)
{
	long p = g_self_pid ? *g_self_pid : 0;
	return p ? p : current_task_id();
}

void tawc_usercopy_refresh_pid(void)
{
	if (g_self_pid) *g_self_pid = TAWC_RAW(TAWC_SYS_getpid, 0, 0, 0, 0, 0, 0);
}

static void pid_cache_init(void)
{
	if (g_self_pid) {
		tawc_usercopy_refresh_pid();
		return;
	}
	long m = TAWC_RAW(TAWC_SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m < 0 && m > -4096) return;
	if (TAWC_RAW(TAWC_SYS_madvise, m, 4096, TAWC_MADV_WIPEONFORK, 0, 0, 0) < 0) {
		(void)TAWC_RAW(TAWC_SYS_munmap, m, 4096, 0, 0, 0, 0);
		return;
	}
	g_self_pid = (volatile long *)m;
	tawc_usercopy_refresh_pid();
}

/* One process_vm_{read,write}v. A cached tgid whose leader thread has
 * exited (zombie leader, live siblings) has no mm: ESRCH. Retry those
 * by tid. */
static long vm_rw(long nr, struct iovec *liov, struct iovec *riov)
{
	long rv = TAWC_RAW(nr, self_task(), (long)liov, 1, (long)riov, 1, 0);
	if (rv == TAWC_ESRCH)
		rv = TAWC_RAW(nr, current_task_id(), (long)liov, 1,
			      (long)riov, 1, 0);
	return rv;
}

long tawc_usercopy_init(void)
{
	/* Probe: copy 1 byte from a known-good local address into a local
	 * buffer through process_vm_readv. If the kernel honors it, mark
	 * the helpers usable. */
	char src = 'x';
	char dst = 0;
	struct iovec liov = { .iov_base = &dst, .iov_len = 1 };
	struct iovec riov = { .iov_base = &src, .iov_len = 1 };
	pid_cache_init();
	long rv = vm_rw(TAWC_SYS_process_vm_readv, &liov, &riov);
	if (rv == 1 && dst == 'x') {
		tawc_usercopy_works = 1;
		return 0;
	}
	return rv < 0 ? rv : TAWC_ENOSYS;
}

/* Read up to `len` bytes from a guest address. Returns bytes read or
 * -errno.
 *
 * If process_vm_readv was unreachable at init (probe failed) we return
 * -EFAULT loudly rather than fall back to a bare memcpy. The probe must
 * succeed on every target we ship to (Linux 3.2+ has the syscall; SELinux
 * lets a process readv itself); a probe failure is a deployment problem,
 * not a runtime fallback condition. (Review finding B7.) */
static long readv_guest(void *dst, size_t len, const void *src)
{
	if (!tawc_usercopy_works) return TAWC_EFAULT;
	struct iovec liov = { .iov_base = dst,        .iov_len = len };
	struct iovec riov = { .iov_base = (void *)src, .iov_len = len };
	return vm_rw(TAWC_SYS_process_vm_readv, &liov, &riov);
}

/* Chunk for string reads. Paths are almost always shorter, so one
 * syscall usually finds the NUL. */
#define STR_CHUNK 512u
#define STR_PAGE  4096u

long tawc_copy_string_from_guest(char *dst, size_t cap,
				 const char *guest_src)
{
	if (!dst || cap == 0) return TAWC_EFAULT;
	if (!guest_src) return TAWC_EFAULT;
	if (!tawc_usercopy_works) return TAWC_EFAULT;

	/* The kernel only completes whole iovec elements, so a chunk that
	 * runs into an unmapped page must be split at the page boundary
	 * for the mapped head to come back as a partial read instead of
	 * EFAULT for the whole chunk. 4 KiB is the smallest page size we
	 * run on; splitting finer on 16 KiB kernels is harmless. */
	size_t off = 0;
	while (off < cap) {
		size_t want = cap - off;
		if (want > STR_CHUNK) want = STR_CHUNK;
		uintptr_t at = (uintptr_t)guest_src + off;
		size_t head = STR_PAGE - (at & (STR_PAGE - 1));
		if (head > want) head = want;
		struct iovec liov = { .iov_base = dst + off, .iov_len = want };
		struct iovec riov[2] = {
			{ .iov_base = (void *)at,          .iov_len = head },
			{ .iov_base = (void *)(at + head), .iov_len = want - head },
		};
		long got = TAWC_RAW(TAWC_SYS_process_vm_readv, self_task(),
				    (long)&liov, 1, (long)riov,
				    head < want ? 2 : 1, 0);
		if (got == TAWC_ESRCH)
			got = TAWC_RAW(TAWC_SYS_process_vm_readv,
				       current_task_id(), (long)&liov, 1,
				       (long)riov, head < want ? 2 : 1, 0);
		if (got <= 0) return TAWC_EFAULT;
		for (long i = 0; i < got; i++) {
			if (dst[off + i] == 0) {
				return (long)(off + i);  /* len excl NUL */
			}
		}
		off += (size_t)got;
		if ((size_t)got < want) return TAWC_EFAULT;  /* ran off the mapping */
	}
	return TAWC_ENAMETOOLONG;
}

long tawc_copy_from_guest(void *dst, size_t n, const void *guest_src)
{
	if (!dst || !guest_src) return TAWC_EFAULT;
	long got = readv_guest(dst, n, guest_src);
	if (got < 0) return TAWC_EFAULT;
	if ((size_t)got != n) return TAWC_EFAULT;
	return 0;
}

long tawc_copy_to_guest(void *guest_dst, const void *src, size_t n)
{
	/* Even under the shared-VM model (guest manual-loaded into our own
	 * address space), a bare deref is unsafe: the guest may pass a NULL
	 * or unmapped output pointer and the kernel's contract is to return
	 * -EFAULT, not deliver SIGSEGV synchronously into our SIGSYS handler.
	 * Route through process_vm_writev against our own task id — the kernel
	 * validates the destination and reports -EFAULT cleanly. (Reviewed
	 * findings B1+B6.) */
	if (!guest_dst || !src) return TAWC_EFAULT;
	if (n == 0) return 0;
	if (!tawc_usercopy_works) return TAWC_EFAULT;
	struct iovec liov = { .iov_base = (void *)src,    .iov_len = n };
	struct iovec riov = { .iov_base = guest_dst,      .iov_len = n };
	long rv = vm_rw(TAWC_SYS_process_vm_writev, &liov, &riov);
	if (rv < 0)        return TAWC_EFAULT;
	if ((size_t)rv != n) return TAWC_EFAULT;
	return 0;
}

long tawc_readv_from_guest(void *dst, const struct iovec *riov, int n)
{
	if (!dst || !riov || n <= 0) return TAWC_EFAULT;
	if (!tawc_usercopy_works) return TAWC_EFAULT;
	size_t total = 0;
	for (int i = 0; i < n; i++) total += riov[i].iov_len;
	struct iovec liov = { .iov_base = dst, .iov_len = total };
	long rv = TAWC_RAW(TAWC_SYS_process_vm_readv, self_task(),
			   (long)&liov, 1, (long)riov, n, 0);
	if (rv == TAWC_ESRCH)
		rv = TAWC_RAW(TAWC_SYS_process_vm_readv, current_task_id(),
			      (long)&liov, 1, (long)riov, n, 0);
	return rv;
}
