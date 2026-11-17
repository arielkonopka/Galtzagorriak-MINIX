/* regstart.S - regchild's entry (issue #160, security review U1): save
 * every register exactly as exec left it, before any code runs, and hand
 * the copy to regcheck().  No crtso: nothing touches them first.  r9 is
 * the GOT base the kernel sets for the program (do_exec). */
	.syntax	unified
	.thumb
	.text
	.global	_start
	.type	_start, %function
	.thumb_func
_start:
	push	{r0-r12, lr}		/* sp -> r0 ... r12, lr */
	mov	r0, sp
	bl	regcheck
1:	b	1b
