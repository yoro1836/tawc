/* `tawcroot --init --cgroup`: the fake cgroup v2 tree at /sys/fs/cgroup
 * (statfs magic, /proc/self/cgroup, mkdir/rmdir, cgroup.procs moves,
 * cgroup.events, cgroup.kill) and the init session's mount/umount.
 *
 * Exit 42 on success; every other code names the failed step. */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>

#define CG "/sys/fs/cgroup"

static int slurp(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return -1;
	ssize_t n = read(fd, buf, cap - 1);
	close(fd);
	if (n < 0) return -1;
	buf[n] = 0;
	return (int)n;
}

static int put(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) return -1;
	ssize_t n = write(fd, s, strlen(s));
	close(fd);
	return n == (ssize_t)strlen(s) ? 0 : -1;
}

int main(void)
{
	char buf[256];
	struct statfs sf;
	if (statfs(CG, &sf) != 0 || sf.f_type != 0x63677270) return 1;
	if (slurp("/proc/self/cgroup", buf, sizeof buf) < 0 ||
	    strcmp(buf, "0::/\n") != 0) return 2;

	if (mkdir(CG "/a", 0755) != 0) return 3;
	if (access(CG "/a/cgroup.procs", F_OK) != 0) return 4;

	/* A child moves itself into a/, then waits to be killed. */
	int p[2];
	if (pipe(p) != 0) return 10;
	pid_t c = fork();
	if (c < 0) return 11;
	if (c == 0) {
		char ok = 'x';
		if (put(CG "/a/cgroup.procs", "0") == 0 &&
		    slurp("/proc/self/cgroup", buf, sizeof buf) > 0 &&
		    strcmp(buf, "0::/a\n") == 0)
			ok = 'o';
		(void)!write(p[1], &ok, 1);
		for (;;) pause();
	}
	char ok = 0;
	if (read(p[0], &ok, 1) != 1 || ok != 'o') return 12;

	char want[32];
	snprintf(want, sizeof want, "%d\n", (int)c);
	if (slurp(CG "/a/cgroup.procs", buf, sizeof buf) < 0 ||
	    strcmp(buf, want) != 0) return 13;
	if (slurp(CG "/a/cgroup.events", buf, sizeof buf) < 0 ||
	    !strstr(buf, "populated 1")) return 14;
	snprintf(buf, sizeof buf, "/proc/%d/cgroup", (int)c);
	if (slurp(buf, buf, sizeof buf) < 0 || strcmp(buf, "0::/a\n") != 0)
		return 15;
	if (rmdir(CG "/a") == 0 || errno != EBUSY) return 16;

	if (put(CG "/a/cgroup.kill", "1") != 0) return 20;
	int status = 0;
	if (waitpid(c, &status, 0) != c || !WIFSIGNALED(status) ||
	    WTERMSIG(status) != SIGKILL) return 21;
	if (rmdir(CG "/a") != 0) return 22;
	if (access(CG "/a", F_OK) == 0) return 23;

	/* Init session mounts: API points pretend, unknown filesystems
	 * refuse, and umount of a non-mount says "not a mount point" so
	 * loops terminate. */
	if (mount("tmpfs", "/run", "tmpfs", 0, 0) != 0) return 30;
	if (mount("none", "/bin", "debugfs", 0, 0) == 0 || errno != EPERM)
		return 31;
	if (umount2("/run", 0) == 0 || errno != EINVAL) return 32;
	return 42;
}
