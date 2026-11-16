/* FP register helpers for the R5.7 test in m2test.c (lazy FPU context
 * switch, arm/mpx.S).  The rest of m2test is soft-float and never touches
 * the FP registers, so what fp_fill puts there must still be there at
 * fp_check, whatever ran in between.
 *
 * fp_fill(seed)	S0-S31 = seed << 8 | n, FPSCR = RMode (seed & 3), IOC
 * fp_check(seed)	the number of registers that differ from that
 * fp_nonzero()		the number of nonzero registers (S0-S31, FPSCR)
 */
	.syntax	unified
	.thumb
	.fpu	fpv4-sp-d16
	.text

	.global	fp_fill
	.type	fp_fill, %function
	.thumb_func
fp_fill:
	lsls	r1, r0, #8
	vmov	s0, r1
	adds	r1, r1, #1
	vmov	s1, r1
	adds	r1, r1, #1
	vmov	s2, r1
	adds	r1, r1, #1
	vmov	s3, r1
	adds	r1, r1, #1
	vmov	s4, r1
	adds	r1, r1, #1
	vmov	s5, r1
	adds	r1, r1, #1
	vmov	s6, r1
	adds	r1, r1, #1
	vmov	s7, r1
	adds	r1, r1, #1
	vmov	s8, r1
	adds	r1, r1, #1
	vmov	s9, r1
	adds	r1, r1, #1
	vmov	s10, r1
	adds	r1, r1, #1
	vmov	s11, r1
	adds	r1, r1, #1
	vmov	s12, r1
	adds	r1, r1, #1
	vmov	s13, r1
	adds	r1, r1, #1
	vmov	s14, r1
	adds	r1, r1, #1
	vmov	s15, r1
	adds	r1, r1, #1
	vmov	s16, r1
	adds	r1, r1, #1
	vmov	s17, r1
	adds	r1, r1, #1
	vmov	s18, r1
	adds	r1, r1, #1
	vmov	s19, r1
	adds	r1, r1, #1
	vmov	s20, r1
	adds	r1, r1, #1
	vmov	s21, r1
	adds	r1, r1, #1
	vmov	s22, r1
	adds	r1, r1, #1
	vmov	s23, r1
	adds	r1, r1, #1
	vmov	s24, r1
	adds	r1, r1, #1
	vmov	s25, r1
	adds	r1, r1, #1
	vmov	s26, r1
	adds	r1, r1, #1
	vmov	s27, r1
	adds	r1, r1, #1
	vmov	s28, r1
	adds	r1, r1, #1
	vmov	s29, r1
	adds	r1, r1, #1
	vmov	s30, r1
	adds	r1, r1, #1
	vmov	s31, r1
	adds	r1, r1, #1
	and	r2, r0, #3
	lsls	r2, r2, #22
	orr	r2, r2, #1
	vmsr	fpscr, r2
	bx	lr

	.global	fp_check
	.type	fp_check, %function
	.thumb_func
fp_check:
	lsls	r1, r0, #8
	and	r2, r0, #3
	lsls	r2, r2, #22
	orr	r2, r2, #1
	movs	r0, #0
	vmrs	r3, fpscr
	cmp	r3, r2
	it	ne
	addne	r0, r0, #1
	vmov	r3, s0
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s1
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s2
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s3
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s4
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s5
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s6
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s7
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s8
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s9
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s10
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s11
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s12
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s13
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s14
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s15
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s16
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s17
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s18
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s19
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s20
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s21
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s22
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s23
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s24
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s25
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s26
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s27
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s28
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s29
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s30
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	vmov	r3, s31
	cmp	r3, r1
	it	ne
	addne	r0, r0, #1
	adds	r1, r1, #1
	bx	lr

	.global	fp_nonzero
	.type	fp_nonzero, %function
	.thumb_func
fp_nonzero:
	movs	r0, #0
	vmrs	r3, fpscr
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s0
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s1
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s2
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s3
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s4
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s5
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s6
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s7
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s8
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s9
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s10
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s11
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s12
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s13
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s14
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s15
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s16
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s17
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s18
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s19
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s20
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s21
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s22
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s23
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s24
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s25
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s26
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s27
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s28
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s29
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s30
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	vmov	r3, s31
	cmp	r3, #0
	it	ne
	addne	r0, r0, #1
	bx	lr
