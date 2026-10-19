/* `tawcroot --init` hostname and privileged ports: sethostname shows in
 * uname, /proc/sys/kernel/hostname and a child; UDP and TCP services on
 * ports below 1024 bind, and their addresses read back unshifted; a
 * unix peer's SO_PEERCRED shows its virtual uid; capget's version probe.
 *
 * Exit 42 on success; every other code names the failed step. */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <stddef.h>
#include <netinet/in.h>
#include <string.h>
#include <linux/capability.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

static struct sockaddr_in lo(int port)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	return a;
}

static int port_of(int fd, int peer)
{
	struct sockaddr_in a;
	socklen_t n = sizeof a;
	if ((peer ? getpeername : getsockname)(fd, (struct sockaddr *)&a, &n) != 0) return -1;
	return ntohs(a.sin_port);
}

int main(void)
{
	struct utsname u;
	char buf[128];
	if (sethostname("tawc-test", 9) != 0) return 1;
	if (uname(&u) != 0 || strcmp(u.nodename, "tawc-test") != 0) return 2;
	int fd = open("/proc/sys/kernel/hostname", O_RDONLY);
	ssize_t n = fd < 0 ? -1 : read(fd, buf, sizeof buf - 1);
	if (n != 10 || memcmp(buf, "tawc-test\n", 10) != 0) return 3;
	close(fd);
	pid_t c = fork();
	if (c == 0) _exit(gethostname(buf, sizeof buf) || strcmp(buf, "tawc-test"));
	int st;
	if (c < 0 || waitpid(c, &st, 0) != c || !WIFEXITED(st) || WEXITSTATUS(st)) return 4;

	/* UDP: a stub resolver's exchange on port 999. */
	int srv = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	int cli = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	struct sockaddr_in a = lo(999), from;
	socklen_t fl = sizeof from;
	if (srv < 0 || cli < 0) return 10;
	if (bind(srv, (struct sockaddr *)&a, sizeof a) != 0) return 11;
	if (port_of(srv, 0) != 999) return 12;
	if (sendto(cli, "q", 1, 0, (struct sockaddr *)&a, sizeof a) != 1) return 13;
	if (recvfrom(srv, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl) != 1) return 14;
	if (sendto(srv, "r", 1, 0, (struct sockaddr *)&from, fl) != 1) return 15;
	fl = sizeof from;
	if (recvfrom(cli, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl) != 1 ||
	    ntohs(from.sin_port) != 999) return 16;
	struct iovec iov = { buf, sizeof buf };
	struct msghdr mh = { .msg_name = &from, .msg_namelen = sizeof from,
			     .msg_iov = &iov, .msg_iovlen = 1 };
	if (connect(cli, (struct sockaddr *)&a, sizeof a) != 0 || port_of(cli, 1) != 999) return 17;
	if (send(cli, "q", 1, 0) != 1) return 18;
	fl = sizeof from;
	if (recvfrom(srv, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl) != 1) return 19;
	if (sendto(srv, "r", 1, 0, (struct sockaddr *)&from, fl) != 1) return 20;
	mh.msg_namelen = sizeof from;
	if (recvmsg(cli, &mh, 0) != 1 || ntohs(from.sin_port) != 999) return 21;

	/* TCP on port 998. */
	int ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	int on = 1;
	a = lo(998);
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
	if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 1) != 0) return 30;
	int tc = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (tc < 0 || connect(tc, (struct sockaddr *)&a, sizeof a) != 0) return 31;
	if (port_of(tc, 1) != 998) return 32;
	int ac = accept(ls, 0, 0);
	if (ac < 0 || port_of(ac, 0) != 998) return 33;

	/* dbus-daemon tells services apart by SO_PEERCRED. */
	struct sockaddr_un ua = { .sun_family = AF_UNIX };
	memcpy(ua.sun_path, "\0tawc-peer", 10);
	socklen_t ul = offsetof(struct sockaddr_un, sun_path) + 10;
	int us = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (us < 0 || bind(us, (struct sockaddr *)&ua, ul) != 0 || listen(us, 1) != 0) return 40;
	c = fork();
	if (c == 0) {
		int s2 = socket(AF_UNIX, SOCK_STREAM, 0);
		if (setresgid(997, 997, 997) || setresuid(997, 997, 997)) _exit(1);
		if (connect(s2, (struct sockaddr *)&ua, ul) || read(s2, buf, 1) != 1) _exit(2);
		_exit(0);
	}
	int pc = accept(us, 0, 0);
	struct ucred cr;
	socklen_t cl = sizeof cr;
	if (c < 0 || pc < 0 || getsockopt(pc, SOL_SOCKET, SO_PEERCRED, &cr, &cl) != 0) return 41;
	if (cr.uid != 997 || cr.gid != 997 || cr.pid != c) return 43;
	if (write(pc, "x", 1) != 1 || waitpid(c, &st, 0) != c || !WIFEXITED(st) ||
	    WEXITSTATUS(st)) return 44;

	struct __user_cap_header_struct h = { 0, 0 };
	if (syscall(SYS_capget, &h, NULL) == 0 || h.version != _LINUX_CAPABILITY_VERSION_3) return 50;
	return 42;
}
