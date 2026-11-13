/* C run-time start-off for MINIX on ARM Cortex-M.
 *
 * On entry sp points to argc, followed by the argv and envp pointer arrays,
 * as built by exec (mm/exec.c) - or to a zero argc word for the boot
 * servers.  Calls main(argc, argv, envp) and exit()s with its result.
 */
	.syntax	unified
	.thumb
	.text

	.global	_start
	.type	_start, %function
	.thumb_func
_start:
	ldr	r0, [sp]		/* argc */
	add	r1, sp, #4		/* argv */
	add	r2, r1, r0, lsl #2
	adds	r2, r2, #4		/* envp = argv + argc + 1 */
	ldr	r3, =environ
	str	r2, [r3]
	ldr	r3, =_penvp		/* getenv() looks here */
	str	r2, [r3]
	mov	r4, sp
	bic	r4, r4, #7		/* AAPCS: 8-byte aligned stack */
	mov	sp, r4
	bl	main
	bl	exit
1:	b	1b

	.data
	.p2align 2
	.global	environ
	.weak	environ
environ:
	.word	0
	.global	_penvp
_penvp:
	.word	0
