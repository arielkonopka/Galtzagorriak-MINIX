/* vfork(): fork without a copy (R2c.1).  The child runs in the parent's
 * memory and on its stack until it calls exec or _exit, and the parent
 * waits in MM meanwhile.  So this must not return through a stack frame
 * the child may overwrite: no frame at all - the message lives below the
 * stack pointer only across the trap, the return address stays in lr
 * (the kernel keeps every register per process).  An error goes on to
 * __vfork_err, a tail call: its frame is made after the fork point.
 *
 * Between vfork and exec the child may only call exec*, _exit, dup2,
 * close, chdir, setuid/setgid and signal (R2c.3).
 */
	.equ	MM, 0
	.equ	VFORK, 57
	.equ	BOTH, 3
	.equ	MSG, 64			/* >= sizeof(message), 8-aligned */

	.syntax	unified
	.thumb
	.text

	.global	vfork
	.global	_vfork
	.type	vfork, %function
	.type	_vfork, %function
	.thumb_func
vfork:
	.thumb_func
_vfork:
	sub	sp, sp, #MSG
	movs	r3, #VFORK
	str	r3, [sp, #4]		/* m_type */
	movs	r0, #MM
	mov	r1, sp
	movs	r2, #BOTH
	svc	0
	cmp	r0, #0
	bne	1f			/* the trap failed: r0 < 0 */
	ldr	r0, [sp, #4]		/* the reply: pid, 0, or -errno */
1:	add	sp, sp, #MSG
	cmp	r0, #0
	it	ge
	bxge	lr
	b	__vfork_err
