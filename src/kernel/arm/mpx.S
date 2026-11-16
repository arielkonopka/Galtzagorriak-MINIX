/* Kernel entry and exit for MINIX on ARM Cortex-M (ARMv7-M).
 *
 * This replaces mpx88.s/mpx386.s.  The model:
 *
 *  - Every process (task, server, user) runs in Thread mode on the process
 *    stack pointer (PSP).  Tasks are privileged, servers and users are not
 *    (CONTROL.nPRIV, kept per process in p_reg.control).
 *  - The kernel proper runs in Handler mode on the main stack (MSP).  All
 *    exceptions that enter MINIX - SysTick, device IRQs (priority
 *    KERNEL_PRIO) and SVC (one level higher, see arm/klib.c) - can never
 *    preempt each other: IRQs cannot preempt SVC, and SVC is only ever
 *    executed from Thread mode.  So there is no nesting, k_reenter only
 *    goes from -1 to 0 and back, and the MSP is always at its top on entry.
 *  - On entry, r4-r11 and the hardware-stacked frame (r0-r3, r12, lr, pc,
 *    xPSR) are moved into proc_ptr->p_reg, which then holds the complete
 *    context, as on the PC.  restart() rebuilds a hardware frame on the
 *    process stack from p_reg and returns into it.
 *
 * System call ABI (see lib/arm/rts/_sendrec.S):
 *	r0 = src_dest, r1 = message pointer, r2 = SEND/RECEIVE/BOTH;
 *	the result is returned in r0.
 *
 * Offsets into struct stackframe_s (kernel/type.h), which is the first
 * member of struct proc:
 */
	.equ	P_R4,		0
	.equ	P_RETREG,	32
	.equ	P_R1,		36
	.equ	P_R2,		40
	.equ	P_PSW,		60
	.equ	P_SP,		64
	.equ	P_CONTROL,	68

	.equ	XPSR_STKALIGN,	0x200	/* frame was padded by 4 for alignment */
	.equ	EXC_RETURN_PSP,	0xFFFFFFFD /* Thread mode, PSP, no FP state */

	.syntax	unified
	.thumb
	.text

/*===========================================================================*
 *				SAVE					     *
 *===========================================================================*/
/* Move the interrupted process' context into proc_ptr->p_reg and bump
 * k_reenter.  Only valid when the exception was taken from Thread mode.
 * Clobbers r0-r12 and lr (the handlers leave through restart, which makes
 * its own EXC_RETURN).  The work is in save_ctx, once, so that the
 * unprivileged loads have one address range (see the fault handlers). */
	.macro	SAVE
	bl	save_ctx
	.endm

	.thumb_func
save_ctx:
	ldr	r12, =proc_ptr
	ldr	r12, [r12]		/* r12 = &proc_ptr->p_reg */
	mrs	r0, psp
	stmia	r12!, {r4-r11}		/* callee-saved registers */
	mrs	r1, control
	tst	r1, #1			/* an unprivileged process: read its */
	bne	1f			/* frame with its own rights (A1) */
	ldmia	r0!, {r4-r11}		/* hardware frame: r0-r3 r12 lr pc xPSR */
	b	2f
1:
	.global	user_ld_begin
user_ld_begin:
	ldrt	r4, [r0, #0]
	ldrt	r5, [r0, #4]
	ldrt	r6, [r0, #8]
	ldrt	r7, [r0, #12]
	ldrt	r8, [r0, #16]
	ldrt	r9, [r0, #20]
	ldrt	r10, [r0, #24]
	ldrt	r11, [r0, #28]
	.global	user_ld_end
user_ld_end:
	adds	r0, r0, #32
2:	stmia	r12!, {r4-r11}		/* -> retreg r1 r2 r3 r12 lr pc psw */
	tst	r11, #XPSR_STKALIGN	/* undo alignment padding */
	it	ne
	addne	r0, r0, #4
	mrs	r1, control
	stmia	r12, {r0, r1}		/* -> sp, control */
	ldr	r0, =k_reenter
	ldrb	r1, [r0]
	adds	r1, r1, #1
	strb	r1, [r0]
	bx	lr
	.ltorg

/*===========================================================================*
 *				SVC_Handler				     *
 *===========================================================================*/
/* System call: sys_call(function, src_dest, m_ptr).  The very first SVC is
 * made by main() to drop into the first process. */
	.global	SVC_Handler
	.thumb_func
SVC_Handler:
	ldr	r0, =k_booted
	ldr	r1, [r0]
	cbz	r1, first_restart

	SAVE
	ldr	r4, =proc_ptr
	ldr	r4, [r4]		/* r4 = caller, survives the call */
	ldr	r0, [r4, #P_R2]		/* function */
	ldr	r1, [r4, #P_RETREG]	/* src_dest (r0) */
	ldr	r2, [r4, #P_R1]		/* m_ptr (r1) */
	bl	sys_call
	str	r0, [r4, #P_RETREG]	/* result into the caller's r0 */
	b	restart

first_restart:
	movs	r1, #1
	str	r1, [r0]		/* k_booted = 1 */
	ldr	r0, =kernel_sp_top
	ldr	r0, [r0]
	msr	msp, r0			/* discard main()'s stack */
	movs	r0, #0
	msr	basepri, r0		/* let interrupts in from now on */
	b	restart

/*===========================================================================*
 *				IRQ_Handler / SysTick_Handler		     *
 *===========================================================================*/
/* Device interrupts and the clock tick.  irq_dispatch(exception_number)
 * calls the handler registered with put_irq_handler(). */
	.global	IRQ_Handler
	.global	SysTick_Handler
	.global	PendSV_Handler
	.thumb_func
PendSV_Handler:
	.thumb_func
IRQ_Handler:
	.thumb_func
SysTick_Handler:
	SAVE
	mrs	r0, ipsr
	bl	irq_dispatch
	b	restart

/*===========================================================================*
 *				Fault handlers				     *
 *===========================================================================*/
/* A fault in a process becomes a signal (or a panic for tasks/servers), see
 * arm_exception() in arm/klib.c.  A fault in Handler mode is a kernel bug. */
	.global	HardFault_Handler
	.global	MemManage_Handler
	.global	BusFault_Handler
	.global	UsageFault_Handler
	.thumb_func
HardFault_Handler:
	.thumb_func
MemManage_Handler:
	.thumb_func
BusFault_Handler:
	.thumb_func
UsageFault_Handler:
	tst	lr, #4			/* came from Thread mode / PSP? */
	beq	handler_fault
	ldr	r0, =k_booted		/* faults before the first process */
	ldr	r0, [r0]		/* starts are kernel faults too */
	cbz	r0, kernel_fault
	SAVE
	mrs	r0, ipsr
	bl	arm_exception
	b	restart

/* A fault in Handler mode is a kernel bug, except in the unprivileged
 * loads and stores of a process frame (save_ctx, restart): there the
 * process's stack pointer was bad (A1, S1).  Then return into
 * frame_recover instead of the faulting instruction, in the handler that
 * was running. */
handler_fault:
	mrs	r0, msp
	ldr	r1, [r0, #24]		/* stacked pc */
	ldr	r2, =user_ld_begin
	cmp	r1, r2
	blo	3f
	ldr	r2, =user_ld_end
	cmp	r1, r2
	blo	4f
3:	ldr	r2, =user_st_begin
	cmp	r1, r2
	blo	kernel_fault
	ldr	r2, =user_st_end
	cmp	r1, r2
	bhs	kernel_fault
4:	ldr	r1, =frame_recover
	bic	r1, r1, #1
	str	r1, [r0, #24]
	ldr	r1, =0xE000ED28		/* CFSR, HFSR: write 1 to clear */
	ldr	r2, [r1]
	str	r2, [r1]
	ldr	r2, [r1, #4]
	str	r2, [r1, #4]
	bx	lr

kernel_fault:
	mrs	r0, msp
	mrs	r1, ipsr
	b	kernel_fault_report	/* does not return */

/* Back in the interrupted handler: start over on a clean kernel stack,
 * have the process killed, and return to whoever is to run now. */
	.thumb_func
frame_recover:
	ldr	r0, =kernel_sp_top
	ldr	r0, [r0]
	msr	msp, r0
	ldr	r0, =k_reenter
	movs	r1, #0
	strb	r1, [r0]
	bl	user_frame_fault
	b	restart

/*===========================================================================*
 *				restart					     *
 *===========================================================================*/
/* Return to the process proc_ptr points to. */
	.global	restart
	.thumb_func
restart:
	cpsid	i
	ldr	r0, =held_head		/* flush held-up interrupts, like */
	ldr	r0, [r0]		/* mpx386.s; k_reenter is still 0 */
	cbz	r0, 1f
	bl	unhold
	cpsid	i
1:
	bl	mpu_switch		/* user process: load its MPU regions */
	ldr	r0, =k_reenter
	ldrb	r1, [r0]
	subs	r1, r1, #1
	strb	r1, [r0]

	ldr	r12, =proc_ptr
	ldr	r12, [r12]		/* r12 = &proc_ptr->p_reg */
	ldr	r0, [r12, #P_SP]
	ldr	r11, [r12, #P_PSW]
	bic	r11, r11, #XPSR_STKALIGN
	tst	r0, #4			/* stack not 8-byte aligned: pad the */
	itt	ne			/* frame so the CPU undoes it again */
	subne	r0, r0, #4
	orrne	r11, r11, #XPSR_STKALIGN
	subs	r0, r0, #32		/* room for the hardware frame */
	add	r1, r12, #P_RETREG
	ldmia	r1, {r4-r10}		/* r0-r3 r12 lr pc */
	bic	r10, r10, #1		/* stacked pc must be halfword aligned;
					 * function pointers carry the Thumb bit */
	ldr	r1, [r12, #P_CONTROL]
	tst	r1, #1			/* unprivileged: write the frame with */
	bne	5f			/* the process's own rights (A1) */
	stmia	r0, {r4-r11}		/* + xPSR */
	b	6f
5:
	.global	user_st_begin
user_st_begin:
	strt	r4, [r0, #0]
	strt	r5, [r0, #4]
	strt	r6, [r0, #8]
	strt	r7, [r0, #12]
	strt	r8, [r0, #16]
	strt	r9, [r0, #20]
	strt	r10, [r0, #24]
	strt	r11, [r0, #28]
	.global	user_st_end
user_st_end:
6:	msr	psp, r0
	msr	control, r1
	isb
	ldmia	r12, {r4-r11}		/* callee-saved registers */
	ldr	lr, =EXC_RETURN_PSP
	movs	r0, #0			/* a process runs unlocked (R4a.1: */
	msr	basepri, r0		/* unhold() returns locked) */
	cpsie	i
	bx	lr

	.ltorg
