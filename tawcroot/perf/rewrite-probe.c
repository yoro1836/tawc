#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef __aarch64__
#error This probe requires AArch64.
#endif

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define MARKER 0x5a7e
#define MAX_ROUNDS 31

struct capture {
	uint64_t regs[31];
	uint64_t nzcv, sp, expected_sp;
};

_Static_assert(sizeof(struct capture) == 272, "assembly capture layout");

extern long raw_getuid(void);
extern long rewrite_getuid(void);
extern long constant_getuid(void);
extern uint32_t rewrite_svc_site;
extern char rewrite_check_return;
extern void rewrite_check(struct capture *);

static size_t page_size;
static volatile uint64_t sink;

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

static int check_registers(long expected)
{
	struct capture c;
	rewrite_check(&c);
	for (unsigned i = 0; i < 31; i++) {
		uint64_t want = i == 0 ? (uint64_t)expected : 0x100 + i;
		if (i == 8) want = 174;
		if (i == 30) want = (uintptr_t)&rewrite_check_return;
		if (c.regs[i] != want) {
			fprintf(stderr, "FAIL register=x%u got=%#" PRIx64 " expected=%#" PRIx64 "\n",
			        i, c.regs[i], want);
			return -1;
		}
	}
	if (c.nzcv != 0x60000000 || c.sp != c.expected_sp || (c.sp & 15)) {
		fprintf(stderr, "FAIL flags_or_sp nzcv=%#" PRIx64 " sp=%#" PRIx64 " expected_sp=%#" PRIx64 "\n",
		        c.nzcv, c.sp, c.expected_sp);
		return -1;
	}
	return 0;
}

static void mapping(const char *name, const void *address)
{
	FILE *f = fopen("/proc/self/maps", "r");
	char line[1024];
	if (!f) fail("open /proc/self/maps");
	while (fgets(line, sizeof(line), f)) {
		unsigned long start, end;
		if (sscanf(line, "%lx-%lx", &start, &end) == 2 &&
		    (uintptr_t)address >= start && (uintptr_t)address < end) {
			printf("MAP name=%s %s", name, line);
			fclose(f);
			return;
		}
	}
	fclose(f);
	fprintf(stderr, "FAIL mapping_missing name=%s address=%p\n", name, address);
	exit(1);
}

static int branch(uintptr_t from, uintptr_t to, uint32_t *instruction)
{
	int64_t delta = (int64_t)to - (int64_t)from;
	if ((delta & 3) || delta < -(1LL << 27) || delta >= (1LL << 27)) return -1;
	*instruction = 0x14000000 | ((uint32_t)(delta / 4) & 0x03ffffff);
	return 0;
}

static uint32_t *near_mapping(uintptr_t site)
{
	uintptr_t base = site & ~(uintptr_t)(page_size - 1);
	int last_errno = ENOMEM;
	/* Neither an arbitrary hint nor MAP_FIXED may replace a live mapping. */
	for (uintptr_t distance = 1U << 20; distance < (127U << 20); distance += 1U << 20) {
		for (int direction = 1; direction >= -1; direction -= 2) {
			if (direction < 0 && base <= distance + page_size) continue;
			uintptr_t address = direction > 0 ? base + distance : base - distance;
			void *p = mmap((void *)address, page_size, PROT_READ | PROT_WRITE,
			               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
			if (p == MAP_FAILED) {
				last_errno = errno;
				continue;
			}
			if ((uintptr_t)p != address) {
				if (munmap(p, page_size)) fail("munmap ignored fixed_noreplace");
				last_errno = ENOTSUP;
				continue;
			}
			return p;
		}
	}
	errno = last_errno;
	return MAP_FAILED;
}

static void flush_code(void *start, size_t size)
{
	__builtin___clear_cache(start, (char *)start + size);
}

static void restore_site(void *page, uint32_t original)
{
	if (mprotect(page, page_size, PROT_READ | PROT_WRITE | PROT_EXEC)) fail("restore RWX");
	*(volatile uint32_t *)&rewrite_svc_site = original;
	flush_code(&rewrite_svc_site, sizeof(rewrite_svc_site));
	if (mprotect(page, page_size, PROT_READ | PROT_EXEC)) fail("restore RX");
}

static double measure(long (*fn)(void), long n)
{
	uint64_t value = 0;
	uint64_t start = now_ns();
	for (long i = 0; i < n; i++) value += (uint64_t)fn();
	uint64_t end = now_ns();
	sink ^= value;
	return (double)(end - start) / n;
}

static int compare_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static void summarize(const char *name, double *samples, int rounds)
{
	qsort(samples, rounds, sizeof(*samples), compare_double);
	printf("SUMMARY name=%s rounds=%d min_ns=%.3f median_ns=%.3f\n",
	       name, rounds, samples[0], samples[rounds / 2]);
}

static void *thread_check(void *unused)
{
	(void)unused;
	for (unsigned i = 0; i < 10000; i++)
		if (rewrite_getuid() != MARKER) return (void *)(uintptr_t)1;
	return NULL;
}

static int concurrent_check(void)
{
	pthread_t threads[4];
	unsigned started = 0;
	for (; started < 4; started++) {
		int error = pthread_create(&threads[started], NULL, thread_check, NULL);
		if (error) {
			fprintf(stderr, "FAIL pthread_create error=%d\n", error);
			break;
		}
	}
	int failed = started != 4;
	for (unsigned i = 0; i < started; i++) {
		void *result;
		int error = pthread_join(threads[i], &result);
		if (error || result) {
			fprintf(stderr, "FAIL pthread_join index=%u error=%d\n", i, error);
			failed = 1;
		}
	}
	return failed ? -1 : 0;
}

static int run(long iterations, int rounds)
{
	alarm(90);
	page_size = (size_t)sysconf(_SC_PAGESIZE);
	if (!page_size || (page_size & (page_size - 1))) {
		fprintf(stderr, "FAIL invalid_page_size=%zu\n", page_size);
		return 1;
	}
	long uid = raw_getuid();
	printf("PROBE arch=aarch64 uid=%ld pagesize=%zu iterations=%ld rounds=%d hook=fixed_return_%#x dispatch=none patching=single_threaded\n",
	       uid, page_size, iterations, rounds, MARKER);
	if (rewrite_getuid() != uid || check_registers(uid)) return 1;
	printf("CHECK original_registers=pass\n");
	uint32_t original = rewrite_svc_site;
	if (original != 0xd4000001) {
		fprintf(stderr, "FAIL unexpected_instruction=%#x\n", original);
		return 1;
	}
	void *page = (void *)((uintptr_t)&rewrite_svc_site & ~(uintptr_t)(page_size - 1));
	mapping("site_before", &rewrite_svc_site);
	uint32_t *trampoline = near_mapping((uintptr_t)&rewrite_svc_site);
	if (trampoline == MAP_FAILED) {
		int error = errno;
		printf("UNAVAILABLE stage=near_mapping errno=%d error=%s original_callable=%s\n",
		       error, strerror(error), rewrite_getuid() == uid ? "yes" : "no");
		return 77;
	}
	uint32_t outbound, inbound;
	if (branch((uintptr_t)&rewrite_svc_site, (uintptr_t)trampoline, &outbound) ||
	    branch((uintptr_t)&trampoline[1], (uintptr_t)&rewrite_svc_site + 4, &inbound)) {
		fprintf(stderr, "FAIL branch_out_of_range\n");
		return 1;
	}
	trampoline[0] = 0xd2800000 | (MARKER << 5); /* movz x0, #MARKER */
	trampoline[1] = inbound;
	flush_code(trampoline, 8);
	if (mprotect(trampoline, page_size, PROT_READ | PROT_EXEC)) {
		int error = errno;
		printf("UNAVAILABLE stage=trampoline_rx errno=%d error=%s original_callable=%s\n",
		       error, strerror(error), rewrite_getuid() == uid ? "yes" : "no");
		if (munmap(trampoline, page_size)) fail("munmap trampoline");
		return 77;
	}
	mapping("trampoline_rx", trampoline);
	printf("BRANCH site=%p trampoline=%p distance=%" PRIdPTR " outbound=%#x inbound=%#x\n",
	       (void *)&rewrite_svc_site, (void *)trampoline,
	       (intptr_t)trampoline - (intptr_t)&rewrite_svc_site, outbound, inbound);
	if (mprotect(page, page_size, PROT_READ | PROT_WRITE | PROT_EXEC)) {
		int error = errno;
		printf("UNAVAILABLE stage=site_rwx errno=%d error=%s original_callable=%s\n",
		       error, strerror(error), rewrite_getuid() == uid ? "yes" : "no");
		if (munmap(trampoline, page_size)) fail("munmap trampoline");
		return 77;
	}
	*(volatile uint32_t *)&rewrite_svc_site = outbound;
	flush_code(&rewrite_svc_site, sizeof(rewrite_svc_site));
	if (mprotect(page, page_size, PROT_READ | PROT_EXEC)) {
		int error = errno;
		restore_site(page, original);
		printf("UNAVAILABLE stage=site_rx errno=%d error=%s original_callable=%s\n",
		       error, strerror(error), rewrite_getuid() == uid ? "yes" : "no");
		if (munmap(trampoline, page_size)) fail("munmap trampoline");
		return 77;
	}
	mapping("site_after", &rewrite_svc_site);
	if (rewrite_getuid() != MARKER || check_registers(MARKER)) {
		restore_site(page, original);
		fprintf(stderr, "FAIL patched_semantics original_callable=%s\n",
		        rewrite_getuid() == uid ? "yes" : "no");
		return 1;
	}
	printf("CHECK patched_registers=pass x1_x30_sp_nzcv=preserved\n");
	if (concurrent_check()) {
		restore_site(page, original);
		return 1;
	}
	printf("CHECK concurrent_execution=pass threads=4 calls_per_thread=10000 concurrent_patch=untested\n");
	measure(raw_getuid, 10000);
	measure(constant_getuid, 10000);
	measure(rewrite_getuid, 10000);
	double samples[3][MAX_ROUNDS];
	long (*functions[3])(void) = { constant_getuid, raw_getuid, rewrite_getuid };
	const char *names[3] = { "constant_call", "raw_svc", "patched_fixed_return" };
	for (int round = 0; round < rounds; round++) {
		for (int step = 0; step < 3; step++) {
			int index = round & 1 ? 2 - step : step;
			samples[index][round] = measure(functions[index], iterations);
			printf("RESULT round=%d name=%s ns_per_call=%.3f\n",
			       round + 1, names[index], samples[index][round]);
		}
	}
	for (unsigned i = 0; i < 3; i++) summarize(names[i], samples[i], rounds);
	restore_site(page, original);
	if (rewrite_getuid() != uid || check_registers(uid)) {
		fprintf(stderr, "FAIL restore_semantics\n");
		return 1;
	}
	if (munmap(trampoline, page_size)) fail("munmap trampoline");
	printf("CHECK restored_original=pass\n");
	printf("PASS scope=controlled_getuid_site_only real_dispatch=unimplemented\n");
	return 0;
}

int main(int argc, char **argv)
{
	long iterations = 1000000;
	int rounds = 7;
	if (argc > 3) {
		fprintf(stderr, "usage: %s [ITERATIONS [ODD_ROUNDS]]\n", argv[0]);
		return 2;
	}
	for (int i = 1; i < argc; i++) {
		char *end;
		errno = 0;
		long value = strtol(argv[i], &end, 10);
		if (errno || !*argv[i] || *end || value < 1 ||
		    (i == 1 && value > 100000000) ||
		    (i == 2 && (value > MAX_ROUNDS || !(value & 1)))) {
			fprintf(stderr, "invalid argument: %s\n", argv[i]);
			return 2;
		}
		if (i == 1) iterations = value;
		else rounds = (int)value;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	pid_t pid = fork();
	if (pid < 0) fail("fork isolation");
	if (!pid) exit(run(iterations, rounds));
	int status;
	pid_t waited;
	do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
	if (waited != pid) fail("waitpid isolation");
	if (WIFSIGNALED(status)) {
		fprintf(stderr, "FAIL isolated_child_signal=%d name=%s\n",
		        WTERMSIG(status), strsignal(WTERMSIG(status)));
		return 1;
	}
	if (!WIFEXITED(status)) {
		fprintf(stderr, "FAIL unexpected_wait_status=%d\n", status);
		return 1;
	}
	return WEXITSTATUS(status);
}
