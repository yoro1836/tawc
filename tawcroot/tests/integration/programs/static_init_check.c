/* `tawcroot --init`: the first guest process is a fake pid 1. Checks
 * getpid/getppid on both sides, orphan adoption (subreaper), and that
 * init only receives signals it has a handler for, as a real namespace
 * init does. With a host pid as argv[1], also checks that the guest
 * can neither see nor signal it, kill(-1) included. With "solo", checks
 * that /proc lists the init as 1 and nothing else (no --console relay).
 *
 * Exit 42 on success; every other code names the failed step. pids go
 * through raw syscalls so libc caching can't hide the translation. */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static long raw_getpid(void) { return syscall(SYS_getpid); }
static long raw_getppid(void) { return syscall(SYS_getppid); }

static volatile sig_atomic_t got_usr1;
static void on_usr1(int sig) { (void)sig; got_usr1 = 1; }

/* Fork, run `fn` in the child and return its exit status (-1 on error). */
static int in_child(int (*fn)(void))
{
	pid_t c = fork();
	if (c < 0) return -1;
	if (c == 0) _exit(fn());
	int status = 0;
	if (waitpid(c, &status, 0) != c || !WIFEXITED(status)) return -1;
	return WEXITSTATUS(status);
}

static int child_ids(void)
{
	if (raw_getppid() != 1) return 1;
	if (raw_getpid() == 1) return 2;
	return 0;
}

static int child_signals_init(void)
{
	if (kill(1, 0) != 0) return 1;
	if (kill(1, SIGTERM) != 0) return 2;
	if (kill(1, SIGKILL) != 0) return 3;
	if (syscall(SYS_tgkill, 1, 1, SIGTERM) != 0) return 4;
	if (kill(1, SIGUSR1) != 0) return 5;
	return 0;
}

static int listed(long pid)
{
	DIR *d = opendir("/proc");
	if (!d) return -1;
	int found = 0;
	for (struct dirent *e; (e = readdir(d));)
		if (atol(e->d_name) == pid) found = 1;
	closedir(d);
	return found;
}

/* Numeric /proc entries other than "1", or -1 when "1" is missing. */
static int others_listed(void)
{
	DIR *d = opendir("/proc");
	if (!d) return -1;
	int others = 0, init = 0;
	for (struct dirent *e; (e = readdir(d));) {
		if (strspn(e->d_name, "0123456789") != strlen(e->d_name)) continue;
		if (!strcmp(e->d_name, "1")) init = 1;
		else others++;
	}
	closedir(d);
	return init ? others : -1;
}

/* The host process `host` stays out of reach; kill(-1) still reaches
 * the guest's own processes. */
static int isolation(long host)
{
	if (kill((pid_t)host, 0) == 0 || errno != ESRCH) return 1;
	if (syscall(SYS_pidfd_open, host, 0) >= 0 || errno != ESRCH) return 2;
	if (syscall(SYS_tgkill, host, host, 0) == 0 || errno != ESRCH) return 3;
	if (listed(host) != 0) return 4;
	if (listed(1) != 1) return 9;

	pid_t c = fork();
	if (c < 0) return 5;
	if (c == 0) { for (;;) pause(); }
	if (listed(c) != 1) return 6;
	if (kill(-1, SIGTERM) != 0) return 7;
	int status = 0;
	if (waitpid(c, &status, 0) != c || !WIFSIGNALED(status) ||
	    WTERMSIG(status) != SIGTERM) return 8;
	return 0;
}

int main(int argc, char **argv)
{
	if (raw_getpid() != 1) return 1;
	if (raw_getppid() != 0) return 2;
	if (in_child(child_ids) != 0) return 3;

	/* A grandchild orphaned by its parent's exit reparents to us. */
	int p[2];
	if (pipe(p) != 0) return 10;
	pid_t c = fork();
	if (c < 0) return 11;
	if (c == 0) {
		if (fork() == 0) {
			for (int i = 0; i < 500 && raw_getppid() != 1; i++)
				usleep(10000);
			char ok = raw_getppid() == 1 ? 'o' : 'x';
			(void)!write(p[1], &ok, 1);
			_exit(7);
		}
		_exit(0);
	}
	int status = 0;
	if (waitpid(c, &status, 0) != c) return 12;
	char ok = 0;
	if (read(p[0], &ok, 1) != 1 || ok != 'o') return 13;
	pid_t orphan = wait(&status);
	if (orphan <= 0 || orphan == c) return 14;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 7) return 15;

	/* SIGTERM/SIGKILL with no handler are dropped; SIGUSR1 has one. */
	struct sigaction sa = { .sa_handler = on_usr1, .sa_flags = SA_RESTART };
	if (sigaction(SIGUSR1, &sa, 0) != 0) return 20;
	int r = in_child(child_signals_init);
	if (r != 0) return 20 + r;
	for (int i = 0; i < 500 && !got_usr1; i++) usleep(10000);
	if (!got_usr1) return 30;
	if (argc > 1 && !strcmp(argv[1], "solo"))
		return others_listed() == 0 ? 42 : 50;
	if (argc > 1) {
		int e = isolation(atol(argv[1]));
		if (e) return 40 + e;
	}
	return 42;
}
