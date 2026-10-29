/* Waits that install a caller mask blocking everything (sigsuspend,
 * ppoll, pselect6, epoll_pwait{,2}): a signal handler run during the
 * wait must still reach translated syscalls, including its rt_sigreturn.
 * Before tawcroot stripped SIGSYS from those masks the kernel killed
 * the guest with SIGSYS here (JavaScriptCore thread suspension).
 *
 * Exit 42 on success; every other code names the failed step. */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_hits;

static void on_usr1(int sig)
{
	(void)sig;
	/* Path syscalls trap; so does the return. */
	int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd >= 0) {
		close(fd);
		g_hits++;
	}
}

/* Queue SIGUSR1 (blocked) so the wait delivers it at once. */
static void arm(void)
{
	kill(getpid(), SIGUSR1);
}

static int check(int rc, int hits_before)
{
	if (rc != -1 || errno != EINTR) return 0;
	if (g_hits != hits_before + 1) return 0;
	sigset_t cur;
	sigprocmask(SIG_SETMASK, NULL, &cur);
	return sigismember(&cur, SIGUSR1) && !sigismember(&cur, SIGTERM);
}

int main(void)
{
	struct sigaction sa = { .sa_handler = on_usr1 };
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGUSR1, &sa, NULL)) return 1;
	sigset_t usr1, all_but;
	sigemptyset(&usr1);
	sigaddset(&usr1, SIGUSR1);
	if (sigprocmask(SIG_BLOCK, &usr1, NULL)) return 2;
	sigfillset(&all_but);
	sigdelset(&all_but, SIGUSR1);
	struct timespec ts = { .tv_sec = 5 };

	int h = g_hits;
	arm();
	if (!check(sigsuspend(&all_but), h)) return 3;

	h = g_hits;
	arm();
	if (!check(ppoll(NULL, 0, &ts, &all_but), h)) return 4;

	h = g_hits;
	arm();
	if (!check(pselect(0, NULL, NULL, NULL, &ts, &all_but), h)) return 5;

	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep < 0) return 6;
	struct epoll_event ev;
	h = g_hits;
	arm();
	if (!check(epoll_pwait(ep, &ev, 1, 5000, &all_but), h)) return 7;

	h = g_hits;
	arm();
	long rc = syscall(441 /* epoll_pwait2 */, ep, &ev, 1, &ts, &all_but, 8);
	if (!(rc == -1 && errno == ENOSYS) && !check((int)rc, h)) return 8;
	return 42;
}
