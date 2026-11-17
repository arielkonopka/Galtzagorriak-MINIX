/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri (spec R5.1, D.1): the cooperative thread switch for Cortex-M4
 * in user space (Thumb-2, soft-float ABI: no FPU registers to keep; the
 * opt-in FPU, R5.7, will add s16-s31 here).
 *
 * void fport_ctx_swap_sp(void **save_sp, void *new_sp);
 *	push the callee-saved registers (r4-r11, lr) on the running stack,
 *	store sp in *save_sp, load new_sp, pop the other thread's registers
 *	and return into it. r9 (the PIC base, -msingle-pic-base) is the same
 *	in every thread of a process and travels in the frame.
 *
 * A new thread's frame (port_minix.c) holds r4 = 0, r5 = the entry
 * function, r9 = the PIC base, lr = fport_ctx_start. */
	.syntax	unified
	.thumb
	.text

	.global	fport_ctx_swap_sp
	.type	fport_ctx_swap_sp, %function
	.thumb_func
fport_ctx_swap_sp:
	push	{r4-r11, lr}
	mov	r2, sp
	str	r2, [r0]
	mov	sp, r1
	pop	{r4-r11, pc}
	.size	fport_ctx_swap_sp, . - fport_ctx_swap_sp

	.global	fport_ctx_start
	.type	fport_ctx_start, %function
	.thumb_func
fport_ctx_start:
	blx	r5		/* the entry never returns */
	udf	#0
	.size	fport_ctx_start, . - fport_ctx_start

/* void *fport_pic_base(void): this process's r9 */
	.global	fport_pic_base
	.type	fport_pic_base, %function
	.thumb_func
fport_pic_base:
	mov	r0, r9
	bx	lr
	.size	fport_pic_base, . - fport_pic_base

/* void *fport_sp(void): the stack pointer */
	.global	fport_sp
	.type	fport_sp, %function
	.thumb_func
fport_sp:
	mov	r0, sp
	bx	lr
	.size	fport_sp, . - fport_sp
