/* `tawcroot --init` mount emulation: tmpfs, bind, move and umount
 * through the session's mount table, seen by an older process, an
 * exec'd image and /proc/self/mountinfo; tmpfs through the new mount
 * API (systemd credentials); plus the container-style uid_map and
 * read-only /proc/sys and /sys.
 *
 * Exit 42 on success; every other code names the failed step. */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>

static int exists(const char *p) { return access(p, F_OK) == 0; }

static int slurp(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return -1;
	size_t have = 0;
	ssize_t n;
	while (have + 1 < cap && (n = read(fd, buf + have, cap - 1 - have)) > 0)
		have += (size_t)n;
	close(fd);
	buf[have] = 0;
	return (int)have;
}

static int touch(const char *p)
{
	int fd = open(p, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
	if (fd < 0) return -1;
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "vfork")) return 0;
	if (argc > 1) return exists("/mnt/t/x") ? 0 : 1;   /* after exec */

	char buf[8192];
	mkdir("/mnt", 0755);
	if (mkdir("/mnt/t", 0755) != 0 || mkdir("/mnt/b", 0755) != 0 ||
	    mkdir("/mnt/c", 0755) != 0) return 1;
	if (touch("/mnt/t/under") != 0) return 2;

	/* A process started before the mount sees it afterwards. */
	int p[2];
	if (pipe(p) != 0) return 3;
	pid_t old = fork();
	if (old < 0) return 4;
	if (old == 0) {
		char c;
		if (read(p[0], &c, 1) != 1) _exit(1);
		_exit(exists("/mnt/t/x") && !exists("/mnt/t/under") ? 0 : 2);
	}

	/* libmount-style watch: epoll on a mountinfo fd wakes on changes. */
	int ep = epoll_create1(EPOLL_CLOEXEC);
	int mi = open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
	struct epoll_event ev = { .events = EPOLLPRI | EPOLLIN | EPOLLET }, got;
	if (ep < 0 || mi < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, mi, &ev) != 0) return 5;
	if (epoll_wait(ep, &got, 1, 0) != 0) return 6;

	if (mount("tmpfs", "/mnt/t", "tmpfs", 0, "mode=0755") != 0) return 10;
	if (epoll_wait(ep, &got, 1, 2000) != 1) return 7;
	close(open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC));   /* drains */
	if (epoll_wait(ep, &got, 1, 0) != 0) return 8;
	if (exists("/mnt/t/under")) return 11;
	struct statfs sf;
	if (statfs("/mnt/t", &sf) != 0 || sf.f_type != 0x01021994) return 12;
	struct stat st;
	if (stat("/mnt/t", &st) != 0 || (st.st_mode & 07777) != 0755) return 13;
	if (touch("/mnt/t/x") != 0) return 14;
	if (slurp("/proc/self/mountinfo", buf, sizeof buf) < 0 ||
	    !strstr(buf, " /mnt/t rw,relatime - tmpfs tmpfs ")) return 15;
	if (slurp("/proc/mounts", buf, sizeof buf) < 0 ||
	    !strstr(buf, "tmpfs /mnt/t tmpfs rw")) return 16;

	int status = 0;
	if (write(p[1], "g", 1) != 1 || waitpid(old, &status, 0) != old ||
	    !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 20;
	pid_t c = fork();
	if (c == 0) {
		execl("/bin/static_mount_check", "static_mount_check", "after-exec", (char *)0);
		_exit(9);
	}
	if (c < 0 || waitpid(c, &status, 0) != c || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0) return 21;

	if (mount("/mnt/t", "/mnt/b", 0, MS_BIND, 0) != 0) return 30;
	if (!exists("/mnt/b/x")) return 31;
	if (mount("/mnt/b", "/mnt/c", 0, MS_MOVE, 0) != 0) return 32;
	if (exists("/mnt/b/x") || !exists("/mnt/c/x")) return 33;
	if (umount2("/mnt/c", 0) != 0 || exists("/mnt/c/x")) return 34;

	close(open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC));
	if (umount2("/mnt/t", 0) != 0) return 40;
	if (epoll_wait(ep, &got, 1, 2000) != 1) return 9;
	if (!exists("/mnt/t/under") || exists("/mnt/t/x")) return 41;
	if (umount2("/mnt/t", 0) == 0 || errno != EINVAL) return 45;
	if (mount("none", "/mnt/t", "debugfs", 0, 0) == 0 || errno != EPERM) return 43;
	if (mount("tmpfs", "/run", "tmpfs", 0, 0) != 0) return 44;

	/* Another process mounts; a vfork child (posix_spawn) is the first
	 * to see it, in memory it shares with us but with its own fds. */
	if (mkdir("/mnt/v", 0755) != 0) return 60;
	c = fork();
	if (c == 0) _exit(mount("tmpfs", "/mnt/v", "tmpfs", 0, 0) || touch("/mnt/v/y"));
	if (c < 0 || waitpid(c, &status, 0) != c || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0) return 61;
	c = vfork();
	if (c == 0) {
		execl("/bin/static_mount_check", "static_mount_check", "vfork", (char *)0);
		_exit(9);
	}
	if (c < 0 || waitpid(c, &status, 0) != c || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0) return 62;
	if (!exists("/mnt/v/y")) return 63;

	/* Looks like a user-namespaced container. */
	if (slurp("/proc/self/uid_map", buf, sizeof buf) < 0 ||
	    strncmp(buf, "         0 ", 11) != 0) return 50;
	struct statvfs sv;
	if (statvfs("/proc/sys", &sv) != 0 || !(sv.f_flag & ST_RDONLY)) return 51;
	struct statfs sys;
	if (statfs("/sys", &sys) == 0 && sys.f_type == 0x62656572 &&
	    !(sys.f_flags & ST_RDONLY)) return 52;

	/* fsopen/fsconfig/fsmount/move_mount, as systemd mounts credentials
	 * (numbers are the same on every arch). */
	int fs = (int)syscall(430 /*fsopen*/, "tmpfs", 1 /*FSOPEN_CLOEXEC*/);
	if (fs < 0) return 70;
	if (read(fs, buf, sizeof buf) != -1) return 71;   /* no messages */
	if (syscall(431 /*fsconfig*/, fs, 1 /*SET_STRING*/, "mode", "0700", 0) != 0 ||
	    syscall(431, fs, 6 /*CMD_CREATE*/, 0, 0, 0) != 0) return 72;
	int mfd = (int)syscall(432 /*fsmount*/, fs, 1 /*FSMOUNT_CLOEXEC*/, 0);
	if (mfd < 0) return 73;
	char link[64];
	snprintf(link, sizeof link, "/proc/self/fd/%d", mfd);
	int dir = open(link, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dir < 0) return 74;
	int f = openat(dir, "cred", O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
	if (f < 0) return 75;
	close(f);
	if (syscall(431, fs, 0 /*SET_FLAG*/, "ro", 0, 0) != 0 ||
	    syscall(431, fs, 7 /*CMD_RECONFIGURE*/, 0, 0, 0) != 0) return 76;
	if (mkdir("/mnt/n", 0755) != 0 ||
	    syscall(429 /*move_mount*/, mfd, "", AT_FDCWD, "/mnt/n", 4 /*F_EMPTY_PATH*/) != 0)
		return 77;
	if (!exists("/mnt/n/cred")) return 78;
	if (stat("/mnt/n", &st) != 0 || (st.st_mode & 07777) != 0700) return 79;
	if (slurp("/proc/self/mountinfo", buf, sizeof buf) < 0 ||
	    !strstr(buf, " /mnt/n ro,relatime - tmpfs tmpfs ")) return 80;
	if (syscall(430, "ext4", 1) != -1 || errno != ENOSYS) return 81;
	return 42;
}
