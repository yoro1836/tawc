/* `tawcroot --join`. Mode "init" (under --init --cgroup): set a
 * hostname, mount a tmpfs on /run with a marker, touch /ready and wait
 * for SIGTERM. Mode "join": see that session from outside it.
 *
 * Exit 42 on success; every other code names the failed step. */

#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t done;
static void on_term(int s) { (void)s; done = 1; }

static int has_line(const char *path, long want)
{
	char buf[4096];
	int fd = open(path, O_RDONLY);
	ssize_t n = fd < 0 ? -1 : read(fd, buf, sizeof buf - 1);
	if (fd >= 0) close(fd);
	if (n <= 0) return 0;
	buf[n] = 0;
	for (char *p = buf; *p; ) {
		if (strtol(p, &p, 10) == want) return 1;
		while (*p == '\n') ++p;
	}
	return 0;
}

static int init_mode(void)
{
	signal(SIGTERM, on_term);
	if (getpid() != 1) return 1;
	if (sethostname("joined", 6) != 0) return 2;
	if (mount("tmpfs", "/run", "tmpfs", 0, 0) != 0) return 3;
	int fd = open("/run/marker", O_WRONLY | O_CREAT, 0644);
	if (fd < 0) return 4;
	close(fd);
	fd = open("/ready", O_WRONLY | O_CREAT, 0644);
	if (fd < 0) return 5;
	close(fd);
	while (!done) pause();
	return 42;
}

static int join_mode(void)
{
	struct utsname u;
	pid_t self = getpid();
	if (self == 1 || kill(1, 0) != 0) return 10;
	if (uname(&u) != 0 || strcmp(u.nodename, "joined") != 0) return 11;
	if (access("/run/marker", F_OK) != 0) return 12;
	char buf[64];
	int fd = open("/proc/1/stat", O_RDONLY);
	ssize_t n = fd < 0 ? -1 : read(fd, buf, sizeof buf - 1);
	if (fd >= 0) close(fd);
	if (n < 3 || memcmp(buf, "1 (", 3) != 0) return 13;
	/* Our tree is part of the session: listed in /proc, signalable. */
	DIR *d = opendir("/proc");
	struct dirent *e;
	int seen = 0;
	while (d && (e = readdir(d))) seen |= atoi(e->d_name) == self;
	if (d) closedir(d);
	if (!seen) return 14;
	pid_t c = fork();
	if (c == 0) { for (;;) pause(); }
	int st;
	if (c < 0 || kill(c, SIGKILL) != 0 || waitpid(c, &st, 0) != c) return 15;
	if (!has_line("/sys/fs/cgroup/cgroup.procs", self)) return 16;
	return 42;
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "init") == 0) return init_mode();
	if (argc > 1 && strcmp(argv[1], "join") == 0) return join_mode();
	return 99;
}
