/* FP register helpers for the R5.7 test in m2test.c (lazy FPU context
 * switch, arm/mpx.S).  The rest of m2test is soft-float and never touches
 * the FP registers, so what fp_fill puts there must still be there at
 * fp_check, whatever ran in between.
 *
 * fp_load(v)	S0-S31 = v[0..31], FPSCR = v[32]
 * fp_store(v)	v[0..31] = S0-S31, v[32] = FPSCR
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
