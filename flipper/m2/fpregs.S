/* FP register helpers for the R5.7 test in m2test.c (lazy FPU context
 * switch, arm/mpx.S).  The rest of m2test is soft-float and never touches
 * the FP registers, so what fp_fill puts there must still be there at
 * fp_check, whatever ran in between.
 *
 * fp_load(v)	S0-S31 = v[0..31], FPSCR = v[32]
 * fp_store(v)	v[0..31] = S0-S31, v[32] = FPSCR
 * fp_dig()	the stack into the break guard with live FP registers
 */
	.syntax	unified
	.thumb
	.fpu	fpv4-sp-d16
	.text

	.global	fp_load
	.type	fp_load, %function
	.thumb_func
fp_load:
	vldmia	r0!, {s0-s31}
	ldr	r1, [r0]
	vmsr	fpscr, r1
	bx	lr

	.global	fp_store
	.type	fp_store, %function
	.thumb_func
fp_store:
	vstmia	r0!, {s0-s31}
	vmrs	r1, fpscr
	str	r1, [r0]
	bx	lr

/* fp_dig(): push down the stack until it faults (#434).  With live FP
 * registers the fault stacks an FP frame (104 B); the push that faults
 * is in the 32 B guard at the break (R4a.2), so the frame's lowest 32 B
 * land in the heap and its lazily saved S0-S15/FPSCR part in the guard:
 * the lazy save fails later, in the kernel's fp_flush.  Never returns. */
	.global	fp_dig
	.type	fp_dig, %function
	.thumb_func
fp_dig:
	vmov	s0, r0			/* an FP context now */
1:	push	{r4-r7}
	b	1b
