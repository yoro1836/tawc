#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Standalone exec/kernel-cost probe. All rows include rusage deltas;
 * page touches are not assumed to equal the number of minor faults. */
static size_t page_size;
static int file_fd;
static const char *file_path;
static unsigned char write_buffer[24576];
static volatile unsigned char sink;

static void fail(const char *what)
{
	perror(what);
	exit(1);
}

static uint64_t now_ns(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t)) fail("clock_gettime");
	return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}

static double cpu_us(struct timeval t)
{
	return t.tv_sec * 1e6 + t.tv_usec;
}

struct sample {
	struct rusage self, children;
	uint64_t time;
};

static struct sample sample(void)
{
	struct sample s;
	if (getrusage(RUSAGE_SELF, &s.self) ||
	    getrusage(RUSAGE_CHILDREN, &s.children)) fail("getrusage");
	s.time = now_ns();
	return s;
}

static void result(const char *name, long n, struct sample a, struct sample b)
{
	printf("RESULT\t%s\t%ld\t%.1f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\n",
	       name, n, (double)(b.time - a.time) / n,
	       (double)(b.self.ru_minflt - a.self.ru_minflt) / n,
	       (double)(b.children.ru_minflt - a.children.ru_minflt) / n,
	       (double)(b.self.ru_majflt - a.self.ru_majflt) / n,
	       (double)(b.children.ru_majflt - a.children.ru_majflt) / n,
	       (cpu_us(b.self.ru_utime) - cpu_us(a.self.ru_utime)) / n,
	       (cpu_us(b.self.ru_stime) - cpu_us(a.self.ru_stime)) / n,
	       (cpu_us(b.children.ru_utime) - cpu_us(a.children.ru_utime)) / n,
	       (cpu_us(b.children.ru_stime) - cpu_us(a.children.ru_stime)) / n);
}

static void *map_anon(size_t len)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
	               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) fail("mmap anon");
	return p;
}

static void unmap(void *p, size_t len)
{
	if (munmap(p, len)) fail("munmap");
}

static void close_fd(int fd)
{
	if (close(fd)) fail("close");
}

static void write_all(int fd, const void *buf, size_t size)
{
	const char *p = buf;
	while (size) {
		ssize_t n = write(fd, p, size);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) fail("write");
		p += n;
		size -= (size_t)n;
	}
}

static int memfd(void)
{
	int fd = (int)syscall(SYS_memfd_create, "exec-cost", 1 /*CLOEXEC*/);
	if (fd < 0) fail("memfd_create");
	return fd;
}

static void wait_child(pid_t pid)
{
	int status;
	pid_t r;
	do { r = waitpid(pid, &status, 0); } while (r < 0 && errno == EINTR);
	if (r != pid) fail("waitpid");
	if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		fprintf(stderr, "exec-cost: child failed, status=%d\n", status);
		exit(1);
	}
}

static void measure(const char *name, long n, void (*op)(void))
{
	/* Warm the code and resources before taking the baseline. */
	for (int i = 0; i < 5; i++) op();
	struct sample a = sample();
	for (long i = 0; i < n; i++) op();
	result(name, n, a, sample());
}

static void bare_syscall(void)
{
	if (syscall(SYS_getppid) < 0) fail("getppid");
}

static void mmap_page(void)
{
	void *p = map_anon(page_size);
	unmap(p, page_size);
}

static void mmap_touch(void)
{
	volatile unsigned char *p = map_anon(page_size);
	*p = 1;
	unmap((void *)p, page_size);
}

static void mmap_stack(void)
{
	size_t len = 8 * 1024 * 1024;
	volatile unsigned char *p = map_anon(len);
	if (mprotect((void *)p, page_size, PROT_NONE)) fail("mprotect");
	p[len - 1] = 1;
	unmap((void *)p, len);
}

static void memfd_empty(void)
{
	close_fd(memfd());
}

static void memfd_small(void)
{
	int fd = memfd();
	write_all(fd, write_buffer, 4096);
	close_fd(fd);
}

static void memfd_large(void)
{
	int fd = memfd();
	write_all(fd, write_buffer, sizeof write_buffer);
	close_fd(fd);
}

/* Current state staging: fresh anonymous pages, write, RO mmap. */
static void state_copy(void)
{
	size_t len = sizeof write_buffer;
	unsigned char *p = map_anon(len);
	memset(p, 1, len);
	int fd = memfd();
	write_all(fd, p, len);
	unmap(p, len);
	p = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED) fail("mmap state read");
	for (size_t i = 0; i < len; i += page_size) sink ^= p[i];
	unmap(p, len);
	close_fd(fd);
}

/* Alternative staging, measured separately before changing the runtime. */
static void state_shared(void)
{
	size_t len = sizeof write_buffer;
	int fd = memfd();
	if (ftruncate(fd, (off_t)len)) fail("ftruncate state");
	unsigned char *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) fail("mmap state shared");
	memset(p, 1, len);
	unmap(p, len);
	p = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED) fail("mmap state read");
	for (size_t i = 0; i < len; i += page_size) sink ^= p[i];
	unmap(p, len);
	close_fd(fd);
}

static void open_close(void)
{
	int fd = open(file_path, O_RDWR | O_CLOEXEC);
	if (fd < 0) fail("open file");
	close_fd(fd);
}

static void pathcache_map(void)
{
	size_t len = 589888;
	int fd = open(file_path, O_RDWR | O_CLOEXEC);
	if (fd < 0) fail("open cache");
	struct stat st;
	if (fstat(fd, &st)) fail("fstat cache");
	volatile unsigned char *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) fail("mmap cache");
	close_fd(fd);
	sink ^= *p;
	unmap((void *)p, len);
}

static void fork_only(void)
{
	pid_t p = fork();
	if (p < 0) fail("fork");
	if (!p) _exit(0);
	wait_child(p);
}

/* No glibc atfork/mask setup. The child only calls _exit. */
static void raw_fork_only(void)
{
	pid_t p = (pid_t)syscall(SYS_clone, SIGCHLD, 0, 0, 0, 0);
	if (p < 0) fail("clone");
	if (!p) _exit(0);
	wait_child(p);
}

static void kernel(long n)
{
	measure("getppid", n, bare_syscall);
	measure("mmap_page", n, mmap_page);
	measure("mmap_touch", n, mmap_touch);
	measure("mmap_stack_guard_touch", n, mmap_stack);
	measure("memfd_empty", n, memfd_empty);
	measure("memfd_write_4k", n, memfd_small);
	measure("memfd_write_24k", n, memfd_large);
	measure("state_copy_24k", n, state_copy);
	measure("state_shared_24k", n, state_shared);
	measure("open_close", n, open_close);
	measure("pathcache_open_map", n, pathcache_map);
	measure("fork_wait", n / 10 + 1, fork_only);
}

static void touch_pages(volatile unsigned char *p, size_t pages)
{
	for (size_t i = 0; i < pages; i++) p[i * page_size] = 1;
}

static void read_pages(const volatile unsigned char *p, size_t pages)
{
	unsigned char s = 0;
	for (size_t i = 0; i < pages; i++) s ^= p[i * page_size];
	sink ^= s;
}

static void faults(long pages)
{
	size_t len = (size_t)pages * page_size;
	volatile unsigned char *p = map_anon(len);
	struct sample a = sample();
	touch_pages(p, (size_t)pages);
	result("anon_first_write_per_page", pages, a, sample());
	a = sample();
	touch_pages(p, (size_t)pages);
	result("anon_repeat_write_per_page", pages, a, sample());
	pid_t child = fork();
	if (child < 0) fail("fork cow");
	if (!child) {
		a = sample();
		touch_pages(p, (size_t)pages);
		result("cow_write_per_page", pages, a, sample());
		_exit(0);
	}
	wait_child(child);
	unmap((void *)p, len);
	if (ftruncate(file_fd, (off_t)len)) fail("ftruncate fault file");
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, file_fd, 0);
	if (p == MAP_FAILED) fail("mmap fault file");
	touch_pages(p, (size_t)pages);
	unmap((void *)p, len);
	p = mmap(NULL, len, PROT_READ, MAP_PRIVATE, file_fd, 0);
	if (p == MAP_FAILED) fail("mmap file read");
	a = sample();
	read_pages(p, (size_t)pages);
	result("file_read_per_page", pages, a, sample());
	child = fork();
	if (child < 0) fail("fork file");
	if (!child) {
		a = sample();
		read_pages(p, (size_t)pages);
		result("file_refault_per_page", pages, a, sample());
		_exit(0);
	}
	wait_child(child);
	unmap((void *)p, len);
}

static char **spawn_argv;

static void spawn_once(void)
{
	pid_t p = fork();
	if (p < 0) fail("fork exec");
	if (!p) {
		execv(spawn_argv[0], spawn_argv);
		perror("execv");
		_exit(127);
	}
	wait_child(p);
}

static long number(const char *s)
{
	char *end;
	errno = 0;
	long n = strtol(s, &end, 10);
	if (errno || !*s || *end || n <= 0 || n > 1000000) {
		fprintf(stderr, "exec-cost: invalid positive count: %s\n", s);
		exit(2);
	}
	return n;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	long ps = sysconf(_SC_PAGESIZE);
	if (ps <= 0) fail("page size");
	page_size = (size_t)ps;
	if (argc < 3) {
		fprintf(stderr, "usage: exec-cost kernel|faults COUNT FILE\n"
		                "       exec-cost fork|raw-fork COUNT [HIGH_FD]\n"
		                "       exec-cost spawn COUNT PROGRAM [ARGS...]\n");
		return 2;
	}
	long n = number(argv[2]);
	printf("# page_size=%zu pid=%ld\n", page_size, (long)getpid());
	puts("# RESULT name count ns_per_op self_minflt child_minflt self_majflt child_majflt self_user_us self_sys_us child_user_us child_sys_us");
	if (!strcmp(argv[1], "spawn") && argc >= 4) {
		spawn_argv = argv + 3;
		measure("fork_exec_wait", n, spawn_once);
	} else if ((!strcmp(argv[1], "fork") || !strcmp(argv[1], "raw-fork")) &&
	           (argc == 3 || argc == 4)) {
		int high_fd = -1;
		if (argc == 4) {
			int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
			if (fd < 0) fail("open /dev/null");
			high_fd = fcntl(fd, F_DUPFD_CLOEXEC, (int)number(argv[3]));
			if (high_fd < 0) fail("reserve high fd");
			close_fd(fd);
		}
		if (!strcmp(argv[1], "fork")) measure("fork_wait", n, fork_only);
		else measure("raw_fork_wait", n, raw_fork_only);
		if (high_fd >= 0) close_fd(high_fd);
	} else if ((!strcmp(argv[1], "kernel") || !strcmp(argv[1], "faults")) && argc == 4) {
		file_path = argv[3];
		file_fd = open(file_path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
		if (file_fd < 0) fail("create scratch file");
		if (ftruncate(file_fd, 589888)) fail("ftruncate scratch file");
		memset(write_buffer, 1, sizeof write_buffer);
		if (!strcmp(argv[1], "kernel")) kernel(n);
		else faults(n);
		close_fd(file_fd);
		if (unlink(file_path)) fail("unlink scratch file");
	} else {
		fprintf(stderr, "exec-cost: invalid arguments\n");
		return 2;
	}
	return 0;
}
