/* setjmp(), _setjmp(), longjmp(), _longjmp() for ARM Cortex-M (Thumb-2).
 *
 * jmp_buf layout (<setjmp.h>, _SETJMP_SAVES_REGS == 1):
 *	 0	__flags		nonzero if the signal mask was saved
 *	 4	__mask		saved signal mask
 *	 8	__regs[]	r4-r11, sp, lr
 * setjmp() and longjmp() save and restore the signal mask (POSIX
 * sigsetjmp(env, 1)); _setjmp() and _longjmp() do not.
 */
	.equ	JB_FLAGS, 0
	.equ	JB_MASK, 4
	.equ	JB_REGS, 8
	.equ	SIG_SETMASK, 2		/* <signal.h> */

	.syntax	unified
	.thumb
	.text

	.global	_setjmp
	.type	_setjmp, %function
	.thumb_func
_setjmp:
	movs	r1, #0
	str	r1, [r0, #JB_FLAGS]
save_regs:
	add	r1, r0, #JB_REGS
	stmia	r1!, {r4-r11}
	mov	r2, sp
	stmia	r1, {r2, lr}
	movs	r0, #0
	bx	lr

	.global	setjmp
	.type	setjmp, %function
	.thumb_func
setjmp:
	push	{r0, lr}
	movs	r1, #1
	str	r1, [r0, #JB_FLAGS]
	add	r2, r0, #JB_MASK	/* sigprocmask(SIG_SETMASK, NULL, &mask) */
	movs	r1, #0			/* only queries the current mask */
	movs	r0, #SIG_SETMASK
	bl	sigprocmask
	pop	{r0, lr}
	b	save_regs		/* sp and lr are the caller's again */

	.global	_longjmp
	.type	_longjmp, %function
	.thumb_func
_longjmp:
	add	r2, r0, #JB_REGS
	ldmia	r2!, {r4-r11}
	ldmia	r2, {r2, lr}
	mov	sp, r2
	movs	r0, r1			/* longjmp(env, 0) returns 1 */
	it	eq
	moveq	r0, #1
	bx	lr

	.global	longjmp
	.type	longjmp, %function
	.thumb_func
longjmp:
	ldr	r2, [r0, #JB_FLAGS]
	cmp	r2, #0
	beq	_longjmp
	push	{r0, r1}
	add	r1, r0, #JB_MASK	/* sigprocmask(SIG_SETMASK, &mask, NULL) */
	movs	r2, #0
	movs	r0, #SIG_SETMASK
	bl	sigprocmask
	pop	{r0, r1}
	b	_longjmp
