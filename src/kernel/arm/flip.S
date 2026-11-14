/* flipwords: exchange two non-overlapping, word aligned memory ranges whose
 * size is a multiple of 16 bytes (the shadow images, whole clicks).  Four
 * words from each side per LDM/STM pair instead of one word at a time:
 * every switch between members of a shadow group goes through here
 * (arm/shadow.c).
 *
 * void flipwords(u32_t *a, u32_t *b, size_t bytes)
 */
	.syntax	unified
	.thumb
	.text
	.global	flipwords
	.type	flipwords, %function
	.thumb_func
	.align	2
flipwords:
	push	{r4-r10}
	lsrs	r2, r2, #4		@ 16-byte blocks
	beq	2f
1:	ldmia	r0, {r3-r6}
	ldmia	r1, {r7-r10}
	stmia	r0!, {r7-r10}
	stmia	r1!, {r3-r6}
	subs	r2, r2, #1
	bne	1b
2:	pop	{r4-r10}
	bx	lr
	.size	flipwords, . - flipwords
