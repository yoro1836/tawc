/* No libc startup: isolate the runtime's work before guest entry. */
__attribute__((noreturn)) void _start(void)
{
#if defined(__aarch64__)
	__asm__ volatile("mov x0, #0\n"
	                 "mov x8, #94\n"
	                 "svc #0" ::: "x0", "x8", "memory");
#elif defined(__x86_64__)
	__asm__ volatile("xor %%edi, %%edi\n"
	                 "mov $231, %%eax\n"
	                 "syscall" ::: "rax", "rdi", "rcx", "r11", "memory");
#else
#error Unsupported architecture
#endif
	__builtin_unreachable();
}
