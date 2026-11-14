/* memcpy, memmove, memset for the Cortex-M4 (Galtzagorriak MINIX).
 *
 * The C versions (src/lib/ansi) move one byte per iteration; these move
 * 32 bytes per LDM/STM pair once the destination is word aligned, and use
 * single (possibly unaligned: the M4 allows it on normal memory, and
 * CCR.UNALIGN_TRP stays off) word loads and stores otherwise.  Used by the
 * kernel (phys_copy, shadow copies), the servers and the user programs.
 * Position independent: no data, only relative branches.
 */
	.syntax	unified
	.thumb
	.text

/*===========================================================================*
 *				memcpy					     *
 *===========================================================================*/
/* void *memcpy(void *d, const void *s, size_t n) */
	.global	memcpy
	.type	memcpy, %function
	.thumb_func
	.align	2
memcpy:
	push	{r0, r4-r9, lr}		@ r0: the return value
	cmp	r2, #16
	blo	.Lc_tail
	ands	r3, r0, #3		@ align the destination
	beq	.Lc_dalign
	rsb	r3, r3, #4
	sub	r2, r2, r3
1:	ldrb	r4, [r1], #1
	strb	r4, [r0], #1
	subs	r3, r3, #1
	bne	1b
.Lc_dalign:
	tst	r1, #3			@ LDM needs an aligned source
	bne	.Lc_words
	subs	r2, r2, #32
	blo	2f
3:	ldmia	r1!, {r3-r9, ip}
	stmia	r0!, {r3-r9, ip}
	subs	r2, r2, #32
	bhs	3b
2:	adds	r2, r2, #32
.Lc_words:
	subs	r2, r2, #4
	blo	4f
5:	ldr	r3, [r1], #4
	str	r3, [r0], #4
	subs	r2, r2, #4
	bhs	5b
4:	adds	r2, r2, #4
.Lc_tail:
	cbz	r2, 7f
6:	ldrb	r3, [r1], #1
	strb	r3, [r0], #1
	subs	r2, r2, #1
	bne	6b
7:	pop	{r0, r4-r9, pc}
	.size	memcpy, . - memcpy

/*===========================================================================*
 *				memmove					     *
 *===========================================================================*/
/* void *memmove(void *d, const void *s, size_t n)
 * Forward (memcpy) unless d lies inside the source; memcpy reads each
 * block before writing it, so d < s may overlap. */
	.global	memmove
	.type	memmove, %function
	.thumb_func
	.align	2
memmove:
	cmp	r0, r1
	bls	memcpy
	adds	r3, r1, r2
	cmp	r0, r3
	bhs	memcpy
	push	{r0, lr}
	adds	r0, r0, r2		@ copy downwards from the ends
	adds	r1, r1, r2
	orr	r3, r0, r1
	tst	r3, #3
	bne	.Lm_tail		@ not both word aligned: bytes
1:	subs	r2, r2, #4
	blo	2f
	ldr	r3, [r1, #-4]!
	str	r3, [r0, #-4]!
	b	1b
2:	adds	r2, r2, #4
.Lm_tail:
	cbz	r2, 4f
3:	ldrb	r3, [r1, #-1]!
	strb	r3, [r0, #-1]!
	subs	r2, r2, #1
	bne	3b
4:	pop	{r0, pc}
	.size	memmove, . - memmove

/*===========================================================================*
 *				memset					     *
 *===========================================================================*/
/* void *memset(void *s, int c, size_t n) */
	.global	memset
	.type	memset, %function
	.thumb_func
	.align	2
memset:
	push	{r0, r4, lr}
	and	r1, r1, #0xFF		@ the byte in all four lanes
	orr	r1, r1, r1, lsl #8
	orr	r1, r1, r1, lsl #16
	cmp	r2, #16
	blo	.Ls_tail
	ands	r3, r0, #3		@ align the destination
	beq	.Ls_align
	rsb	r3, r3, #4
	sub	r2, r2, r3
1:	strb	r1, [r0], #1
	subs	r3, r3, #1
	bne	1b
.Ls_align:
	mov	r3, r1
	mov	r4, r1
	mov	ip, r1
	subs	r2, r2, #16
	blo	2f
3:	stmia	r0!, {r1, r3, r4, ip}
	subs	r2, r2, #16
	bhs	3b
2:	adds	r2, r2, #16
	subs	r2, r2, #4
	blo	4f
5:	str	r1, [r0], #4
	subs	r2, r2, #4
	bhs	5b
4:	adds	r2, r2, #4
.Ls_tail:
	cbz	r2, 7f
6:	strb	r1, [r0], #1
	subs	r2, r2, #1
	bne	6b
7:	pop	{r0, r4, pc}
	.size	memset, . - memset
