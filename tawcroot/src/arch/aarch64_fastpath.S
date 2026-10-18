/* aarch64 SIGSYS-bypass glue (TAWCROOT_FASTPATH builds; src/fastpath.c).
 *
 * A patched site `b stub` lands in a copy of tawcroot_fast_stub_tmpl,
 * whose `b` (word 4) fastpath.c points back at site+4 and whose literal
 * (bytes 24..31) holds tawcroot_fast_common. Like the kernel, the pair
 * preserves every register except x0: GPRs, NZCV and the caller-saved
 * SIMD registers (the C code is not built general-regs-only).
 */

#include "fastpath_config.h"

#if TAWCROOT_FASTPATH

	.text

	.global tawcroot_fast_stub_tmpl
	.hidden tawcroot_fast_stub_tmpl
	.balign 8
tawcroot_fast_stub_tmpl:
	stp     x16, x30, [sp, #-16]!
	ldr     x16, 1f
	blr     x16
	ldp     x16, x30, [sp], #16
	b       .                    /* patched: b site+4 */
	nop
1:	.quad   0                    /* patched: tawcroot_fast_common */

#define FRAME 560

	.global tawcroot_fast_common
	.hidden tawcroot_fast_common
	.type   tawcroot_fast_common, %function
tawcroot_fast_common:
	sub     sp, sp, #FRAME
	stp     x1, x2, [sp, #0]
	stp     x3, x4, [sp, #16]
	stp     x5, x6, [sp, #32]
	stp     x7, x8, [sp, #48]
	stp     x9, x10, [sp, #64]
	stp     x11, x12, [sp, #80]
	stp     x13, x14, [sp, #96]
	stp     x15, x17, [sp, #112]
	stp     x18, x29, [sp, #128]
	mrs     x9, nzcv
	stp     x30, x9, [sp, #144]
	stp     q0, q1, [sp, #160]
	stp     q2, q3, [sp, #192]
	stp     q4, q5, [sp, #224]
	stp     q6, q7, [sp, #256]
	stp     q16, q17, [sp, #288]
	stp     q18, q19, [sp, #320]
	stp     q20, q21, [sp, #352]
	stp     q22, q23, [sp, #384]
	stp     q24, q25, [sp, #416]
	stp     q26, q27, [sp, #448]
	stp     q28, q29, [sp, #480]
	stp     q30, q31, [sp, #512]
	mov     x29, xzr             /* terminate the unwind chain here */
	mov     x6, x8
	bl      tawcroot_fast_entry
	ldp     q30, q31, [sp, #512]
	ldp     q28, q29, [sp, #480]
	ldp     q26, q27, [sp, #448]
	ldp     q24, q25, [sp, #416]
	ldp     q22, q23, [sp, #384]
	ldp     q20, q21, [sp, #352]
	ldp     q18, q19, [sp, #320]
	ldp     q16, q17, [sp, #288]
	ldp     q6, q7, [sp, #256]
	ldp     q4, q5, [sp, #224]
	ldp     q2, q3, [sp, #192]
	ldp     q0, q1, [sp, #160]
	ldp     x30, x9, [sp, #144]
	msr     nzcv, x9
	ldp     x18, x29, [sp, #128]
	ldp     x15, x17, [sp, #112]
	ldp     x13, x14, [sp, #96]
	ldp     x11, x12, [sp, #80]
	ldp     x9, x10, [sp, #64]
	ldp     x7, x8, [sp, #48]
	ldp     x5, x6, [sp, #32]
	ldp     x3, x4, [sp, #16]
	ldp     x1, x2, [sp, #0]
	add     sp, sp, #FRAME
	ret
	.size tawcroot_fast_common, .-tawcroot_fast_common

#endif

	.section .note.GNU-stack, "", %progbits
