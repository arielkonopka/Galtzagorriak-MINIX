/* _send(), _receive(), _sendrec(): the MINIX message passing traps for ARM
 * Cortex-M.  See src/kernel/arm/mpx.S for the kernel side:
 *	r0 = src_dest, r1 = message pointer, r2 = SEND/RECEIVE/BOTH;
 *	result in r0.  All other registers are preserved by the kernel.
 */
	.equ	SEND, 1
	.equ	RECEIVE, 2
	.equ	BOTH, 3

	.syntax	unified
	.thumb
	.text

	.global	_send
	.type	_send, %function
	.thumb_func
_send:
	movs	r2, #SEND
	svc	0
	bx	lr

	.global	_receive
	.type	_receive, %function
	.thumb_func
_receive:
	movs	r2, #RECEIVE
	svc	0
	bx	lr

	.global	_sendrec
	.type	_sendrec, %function
	.thumb_func
_sendrec:
	movs	r2, #BOTH
	svc	0
	bx	lr
