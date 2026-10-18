/* x86_64 twin of static_svc_regs_preserved_aarch64.S: exit 42 iff
 * getuid, 64 times from one site, preserves every register the syscall
 * ABI keeps (all but rax, rcx, r11). Failure: 100+i for the i-th
 * register in rbx rdx rsi rdi rbp r8 r9 r10 r12 r13 r14.
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	movl    $64, %r15d
.Lloop:
	movabsq $0xa5a5000000001000, %rbx
	movabsq $0xa5a5000000001001, %rdx
	movabsq $0xa5a5000000001002, %rsi
	movabsq $0xa5a5000000001003, %rdi
	movabsq $0xa5a5000000001004, %rbp
	movabsq $0xa5a5000000001005, %r8
	movabsq $0xa5a5000000001006, %r9
	movabsq $0xa5a5000000001007, %r10
	movabsq $0xa5a5000000001008, %r12
	movabsq $0xa5a5000000001009, %r13
	movabsq $0xa5a500000000100a, %r14
	movl    $102, %eax               /* __NR_getuid */
	syscall
	movabsq $0xa5a5000000001000, %rax
	cmpq    %rax, %rbx
	jne     .Lr0
	movabsq $0xa5a5000000001001, %rax
	cmpq    %rax, %rdx
	jne     .Lr1
	movabsq $0xa5a5000000001002, %rax
	cmpq    %rax, %rsi
	jne     .Lr2
	movabsq $0xa5a5000000001003, %rax
	cmpq    %rax, %rdi
	jne     .Lr3
	movabsq $0xa5a5000000001004, %rax
	cmpq    %rax, %rbp
	jne     .Lr4
	movabsq $0xa5a5000000001005, %rax
	cmpq    %rax, %r8
	jne     .Lr5
	movabsq $0xa5a5000000001006, %rax
	cmpq    %rax, %r9
	jne     .Lr6
	movabsq $0xa5a5000000001007, %rax
	cmpq    %rax, %r10
	jne     .Lr7
	movabsq $0xa5a5000000001008, %rax
	cmpq    %rax, %r12
	jne     .Lr8
	movabsq $0xa5a5000000001009, %rax
	cmpq    %rax, %r13
	jne     .Lr9
	movabsq $0xa5a500000000100a, %rax
	cmpq    %rax, %r14
	jne     .Lr10
	decl    %r15d
	jnz     .Lloop
	movl    $42, %edi
	jmp     .Lexit
.Lr0:
	movl    $100, %edi
	jmp     .Lexit
.Lr1:
	movl    $101, %edi
	jmp     .Lexit
.Lr2:
	movl    $102, %edi
	jmp     .Lexit
.Lr3:
	movl    $103, %edi
	jmp     .Lexit
.Lr4:
	movl    $104, %edi
	jmp     .Lexit
.Lr5:
	movl    $105, %edi
	jmp     .Lexit
.Lr6:
	movl    $106, %edi
	jmp     .Lexit
.Lr7:
	movl    $107, %edi
	jmp     .Lexit
.Lr8:
	movl    $108, %edi
	jmp     .Lexit
.Lr9:
	movl    $109, %edi
	jmp     .Lexit
.Lr10:
	movl    $110, %edi
	jmp     .Lexit
.Lexit:
	movl    $231, %eax               /* __NR_exit_group */
	syscall

	.section .note.GNU-stack,"",@progbits
