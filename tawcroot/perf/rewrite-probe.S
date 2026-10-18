	.text
	.p2align 12
	.global rewrite_getuid
	.type rewrite_getuid, %function
rewrite_getuid:
	mov x8, #174
	.global rewrite_svc_site
rewrite_svc_site:
	svc #0
	ret
	.size rewrite_getuid, .-rewrite_getuid

	.p2align 4
	.global raw_getuid
	.type raw_getuid, %function
raw_getuid:
	mov x8, #174
	svc #0
	ret
	.size raw_getuid, .-raw_getuid

	.p2align 4
	.global constant_getuid
	.type constant_getuid, %function
constant_getuid:
	mov x8, #174
	mov x0, #0x5a7e
	ret
	.size constant_getuid, .-constant_getuid

/* Capture x0..x30, NZCV and SP around the controlled site. The caller's
 * callee-saved registers are restored after the capture is copied out. */
	.p2align 4
	.global rewrite_check
	.type rewrite_check, %function
rewrite_check:
	stp x19, x20, [sp, #-96]!
	stp x21, x22, [sp, #16]
	stp x23, x24, [sp, #32]
	stp x25, x26, [sp, #48]
	stp x27, x28, [sp, #64]
	stp x29, x30, [sp, #80]
	sub sp, sp, #288
	str x0, [sp, #272]
	mov x0, sp
	str x0, [sp, #264]
	cmp xzr, xzr
	mov x0, #0x100
	mov x1, #0x101
	mov x2, #0x102
	mov x3, #0x103
	mov x4, #0x104
	mov x5, #0x105
	mov x6, #0x106
	mov x7, #0x107
	mov x8, #0x108
	mov x9, #0x109
	mov x10, #0x10a
	mov x11, #0x10b
	mov x12, #0x10c
	mov x13, #0x10d
	mov x14, #0x10e
	mov x15, #0x10f
	mov x16, #0x110
	mov x17, #0x111
	mov x18, #0x112
	mov x19, #0x113
	mov x20, #0x114
	mov x21, #0x115
	mov x22, #0x116
	mov x23, #0x117
	mov x24, #0x118
	mov x25, #0x119
	mov x26, #0x11a
	mov x27, #0x11b
	mov x28, #0x11c
	mov x29, #0x11d
	bl rewrite_getuid
	.global rewrite_check_return
rewrite_check_return:
	stp x0, x1, [sp, #0]
	stp x2, x3, [sp, #16]
	stp x4, x5, [sp, #32]
	stp x6, x7, [sp, #48]
	stp x8, x9, [sp, #64]
	stp x10, x11, [sp, #80]
	stp x12, x13, [sp, #96]
	stp x14, x15, [sp, #112]
	stp x16, x17, [sp, #128]
	stp x18, x19, [sp, #144]
	stp x20, x21, [sp, #160]
	stp x22, x23, [sp, #176]
	stp x24, x25, [sp, #192]
	stp x26, x27, [sp, #208]
	stp x28, x29, [sp, #224]
	str x30, [sp, #240]
	mrs x0, nzcv
	str x0, [sp, #248]
	mov x0, sp
	str x0, [sp, #256]
	ldr x0, [sp, #272]
	mov x1, sp
	mov x2, #272
	bl memcpy
	add sp, sp, #288
	ldp x21, x22, [sp, #16]
	ldp x23, x24, [sp, #32]
	ldp x25, x26, [sp, #48]
	ldp x27, x28, [sp, #64]
	ldp x29, x30, [sp, #80]
	ldp x19, x20, [sp], #96
	ret
	.size rewrite_check, .-rewrite_check

	.section .note.GNU-stack,"",%progbits
