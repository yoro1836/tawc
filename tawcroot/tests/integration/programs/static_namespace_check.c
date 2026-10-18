/* Fake user/PID namespaces under production seccomp. write, getpid,
 * getppid, kill and tgkill stay out of the base filter and are trapped
 * by a stacked filter once a namespace exists, so this checks both
 * sides of that switch:
 *
 *   - before unshare, write and getpid are the plain kernel calls
 *   - uid_map writes after unshare(CLONE_NEWUSER) are emulated
 *     (a second write is EPERM, like the kernel's)
 *   - a clone(CLONE_NEWPID) child sees itself as pid 1, ppid 0
 *
 * Exit 42 on success; every other code names the failed step. pids go
 * through raw syscalls: bionic caches getpid() in its thread struct,
 * and the raw clone below skips the bookkeeping that would refresh it. */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static long raw_getpid(void) { return syscall(SYS_getpid); }
static long raw_getppid(void) { return syscall(SYS_getppid); }

static int write_str(int fd, const char *s)
{
	size_t n = strlen(s);
	return write(fd, s, n) == (ssize_t)n;
}

int main(void)
{
	int p[2];
	if (pipe(p) != 0) return 1;
	if (!write_str(p[1], "x")) return 2;
	long self = raw_getpid();
	if (self <= 1) return 3;

	if (unshare(CLONE_NEWUSER) != 0) return 10;
	int fd = open("/proc/self/uid_map", O_WRONLY);
	if (fd < 0) return 11;
	if (!write_str(fd, "0 1000 1\n")) return 12;
	errno = 0;
	if (write(fd, "0 1000 1\n", 9) != -1 || errno != EPERM) return 13;
	close(fd);
	if (!write_str(p[1], "y")) return 14;   /* trapped now, still forwarded */
	if (raw_getpid() != self) return 15;

	long child = syscall(SYS_clone, CLONE_NEWPID | SIGCHLD, 0, 0, 0, 0);
	if (child < 0) return 20;
	if (child == 0) {
		if (raw_getpid() != 1) _exit(21);
		if (raw_getppid() != 0) _exit(22);
		_exit(0);
	}
	int status = 0;
	if (waitpid((pid_t)child, &status, 0) != child) return 23;
	if (!WIFEXITED(status)) return 24;
	if (WEXITSTATUS(status) != 0) return WEXITSTATUS(status);
	if (raw_getpid() != self) return 25;
	return 42;
}
