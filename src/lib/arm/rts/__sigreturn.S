/* __sigreturn: where a signal handler returns to (the kernel sets lr to it,
 * see do_sendsig() in src/kernel/system.c).  sp points at the struct
 * sigframe below the saved struct sigcontext; hand that to _sigreturn(),
 * which asks MM to restore the interrupted context.  It does not return.
 */
	.equ	SF_SCPCOPY, 24		/* offsetof(struct sigframe, sf_scpcopy) */

	.syntax	unified
	.thumb
	.text

	.global	__sigreturn
	.type	__sigreturn, %function
	.thumb_func
__sigreturn:
	ldr	r0, [sp, #SF_SCPCOPY]
	bl	_sigreturn
1:	b	1b
