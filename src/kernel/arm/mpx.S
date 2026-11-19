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
	.equ	EXC_RETURN_NOFP, 0x10	/* clear: the frame has S0-S15, FPSCR */
	.equ	CONTROL_FPCA,	0x4	/* in p_reg.control: an FP context */
	.equ	CONTROL_FPLIVE,	0x8	/* p_reg.control only: its FP registers
					 * are in the FPU (see fp_flush) */
	.equ	FP_SAVE,	64	/* S16-S31, below the hardware frame */
	.equ	FPCCR,		0xE000EF34
	.equ	FPCCR_LSPACT,	0x1	/* a lazy S0-S15 save is pending */

	.syntax	unified
	.thumb
	.fpu	fpv4-sp-d16		/* only fp_flush, fp_zero and restart */
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
	mov	r2, lr			/* EXC_RETURN: FP frame or not */
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
	mrs	r1, control
	bic	r1, r1, #CONTROL_FPCA
	tst	r2, #EXC_RETURN_NOFP
	beq	3f
	tst	r11, #XPSR_STKALIGN	/* undo alignment padding */
	it	ne
	addne	r0, r0, #4
	stmia	r12, {r0, r1}		/* -> sp, control */
	b	4f
3:	/* R5.7: an FP process (it used the FPU since its last kernel
	 * entry or restore).  Its frame has S0-S15 and FPSCR too, maybe
	 * not written yet (lazy stacking, FPCCR.LSPEN).  Its FP registers
	 * stay in the FPU: fp_flush saves them only when another process
	 * is entered.  sp is where S16-S31 then go, FP_SAVE bytes below
	 * the frame; 4 lower still if the CPU padded the frame (sp & 4
	 * says so: the frame itself is 8-byte aligned).  Below sp is
	 * free for the kernel (signal frames). */
	subs	r0, r0, #(32 + FP_SAVE)
	tst	r11, #XPSR_STKALIGN
	it	ne
	subne	r0, r0, #4
	orr	r1, r1, #(CONTROL_FPCA | CONTROL_FPLIVE)
	stmia	r12, {r0, r1}		/* -> sp, control */
4:	ldr	r0, =k_reenter
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
	.global	DebugMon_Handler
	.thumb_func
HardFault_Handler:
	.thumb_func
DebugMon_Handler:			/* R4a.6: a stack guard was written */
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
	ldr	r1, [r0, #24]		/* stacked pc (also in an FP frame) */
	ldr	r3, =frame_recover
	ldr	r2, =user_ld_begin
	cmp	r1, r2
	blo	3f
	ldr	r2, =user_ld_end
	cmp	r1, r2
	blo	4f
3:	ldr	r2, =user_st_begin
	cmp	r1, r2
	blo	5f
	ldr	r2, =user_st_end
	cmp	r1, r2
	blo	4f
5:	ldr	r2, =fp_st_begin	/* fp_flush: mpu_proc's stack */
	cmp	r1, r2
	blo	kernel_fault
	ldr	r2, =fp_st_end
	cmp	r1, r2
	bhs	kernel_fault
	ldr	r3, =fp_frame_recover
4:	bic	r1, r3, #1
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
	ldr	r0, =proc_ptr
	ldr	r0, [r0]
	bl	user_frame_fault
	b	restart

/* The same for fp_flush: mpu_proc's stack was bad (the lazy S0-S15 save
 * or the S16-S31 stores).  It gets the signal; the FPU is cleared. */
	.thumb_func
fp_frame_recover:
	ldr	r0, =kernel_sp_top
	ldr	r0, [r0]
	msr	msp, r0
	ldr	r0, =k_reenter
	movs	r1, #0
	strb	r1, [r0]
	ldr	r0, =mpu_proc
	ldr	r4, [r0]
	bl	fp_drop
	mov	r0, r4
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
	ldr	r1, =mpu_proc		/* R5.7: another process's FP */
	ldr	r1, [r1]		/* registers are in the FPU: save */
	cbz	r1, 2f			/* them before its MPU regions go */
	ldr	r0, =proc_ptr		/* (only the last user process run, */
	ldr	r0, [r0]		/* mpu_proc, can have them) */
	cmp	r0, r1
	beq	2f
	ldr	r0, [r1, #P_CONTROL]
	tst	r0, #CONTROL_FPLIVE
	it	ne
	blne	fp_flush
2:
	bl	mpu_switch		/* user process: load its MPU regions */
	ldr	r0, =k_reenter
	ldrb	r1, [r0]
	subs	r1, r1, #1
	strb	r1, [r0]

	ldr	r12, =proc_ptr
	ldr	r12, [r12]		/* r12 = &proc_ptr->p_reg */
	ldr	r0, [r12, #P_SP]
	ldr	r11, [r12, #P_PSW]
	ldr	r3, [r12, #P_CONTROL]
	bic	r11, r11, #XPSR_STKALIGN
	tst	r0, #4			/* stack not 8-byte aligned: pad the */
	beq	7f			/* frame so the CPU undoes it again */
	orr	r11, r11, #XPSR_STKALIGN
	tst	r3, #CONTROL_FPCA	/* FP: sp is the S16-S31 area, 4 */
	ite	eq			/* below it (save_ctx) */
	subeq	r0, r0, #4
	addne	r0, r0, #4
7:	tst	r3, #CONTROL_FPCA
	ite	eq
	subeq	r0, r0, #32		/* room for the hardware frame */
	addne	r0, r0, #FP_SAVE	/* FP: the frame is still there */
	add	r1, r12, #P_RETREG
	ldmia	r1, {r4-r10}		/* r0-r3 r12 lr pc */
	bic	r10, r10, #1		/* stacked pc must be halfword aligned;
					 * function pointers carry the Thumb bit */
	tst	r3, #1			/* unprivileged: write the frame with */
	bne	5f			/* the process's own rights (A1) */
	stmia	r0, {r4-r11}		/* + xPSR; tasks never use the FPU */
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
	/* R5.7: an FP process whose registers were saved (fp_flush):
	 * S16-S31 back from its stack; the CPU reloads S0-S15 and FPSCR
	 * from the frame on the way out.  Still in the FPU (FPLIVE,
	 * nothing else ran): nothing to do, even the lazy save is skipped. */
	tst	r3, #CONTROL_FPCA
	beq	6f
	tst	r3, #CONTROL_FPLIVE
	bne	6f
	orr	r3, r3, #CONTROL_FPLIVE	/* its registers from here on (also */
	str	r3, [r12, #P_CONTROL]	/* half loaded: fp_flush clears them) */
	/* Where fp_flush put them: at p_reg.sp, which is 4 below
	 * frame - FP_SAVE when the CPU padded the frame (save_ctx).
	 * frame - FP_SAVE read them 4 bytes off: S16 got S17's value,
	 * S31 the padding word (#331, fptest time-sliced). */
	ldr	r2, [r12, #P_SP]
	ldrt	r4, [r2, #0]
	ldrt	r5, [r2, #4]
	ldrt	r6, [r2, #8]
	ldrt	r7, [r2, #12]
	ldrt	r8, [r2, #16]
	ldrt	r9, [r2, #20]
	ldrt	r10, [r2, #24]
	ldrt	r11, [r2, #28]
	vmov	s16, s17, r4, r5
	vmov	s18, s19, r6, r7
	vmov	s20, s21, r8, r9
	vmov	s22, s23, r10, r11
	ldrt	r4, [r2, #32]
	ldrt	r5, [r2, #36]
	ldrt	r6, [r2, #40]
	ldrt	r7, [r2, #44]
	ldrt	r8, [r2, #48]
	ldrt	r9, [r2, #52]
	ldrt	r10, [r2, #56]
	ldrt	r11, [r2, #60]
	vmov	s24, s25, r4, r5
	vmov	s26, s27, r6, r7
	vmov	s28, s29, r8, r9
	vmov	s30, s31, r10, r11
	.global	user_st_end
user_st_end:
6:	msr	psp, r0
	bic	r1, r3, #(CONTROL_FPCA | CONTROL_FPLIVE) /* not CONTROL bits */
	msr	control, r1
	isb
	ldmia	r12, {r4-r11}		/* callee-saved registers */
	ldr	lr, =EXC_RETURN_PSP
	tst	r3, #CONTROL_FPCA
	it	ne
	bicne	lr, lr, #EXC_RETURN_NOFP /* 0xFFFFFFED: FP frame */
	movs	r0, #0			/* a process runs unlocked (R4a.1: */
	msr	basepri, r0		/* unhold() returns locked) */
	cpsie	i
	bx	lr

	.ltorg

/*===========================================================================*
 *				fp_flush, fp_drop			     *
 *===========================================================================*/
/* R5.7, lazy FP context switch.  The FPU holds the registers of the
 * process r1 points to (CONTROL_FPLIVE in its p_reg.control; it can only
 * be mpu_proc, the last user process run, so no other variable is
 * needed), and another process is about to be entered.  Save S16-S31 at
 * its sp (save_ctx put it FP_SAVE bytes below the frame); the first
 * FP instruction here also makes the CPU do the pending lazy save of
 * S0-S15 and FPSCR into the frame, with its rights and MPU regions
 * (still loaded: called before mpu_switch).  Then clear the FPU, so that
 * no process sees another's FP registers.  Stores with the process's
 * own rights (A1); a fault goes to fp_frame_recover.
 * Clobbers r0, r2-r11.  A process without the FPU never gets here: it
 * costs restart one load and a branch. */
	.thumb_func
fp_flush:
	ldr	r0, [r1, #P_SP]
	ldr	r3, [r1, #P_CONTROL]
	bic	r3, r3, #CONTROL_FPLIVE
	str	r3, [r1, #P_CONTROL]
	tst	r3, #CONTROL_FPCA	/* the kernel dropped its FP context */
	beq	fp_drop			/* (exec, bad stack): just clear */
	.global	fp_st_begin
fp_st_begin:
	vmov	r4, r5, s16, s17	/* takes the lazy save first */
	vmov	r6, r7, s18, s19
	vmov	r8, r9, s20, s21
	vmov	r10, r11, s22, s23
	strt	r4, [r0, #0]
	strt	r5, [r0, #4]
	strt	r6, [r0, #8]
	strt	r7, [r0, #12]
	strt	r8, [r0, #16]
	strt	r9, [r0, #20]
	strt	r10, [r0, #24]
	strt	r11, [r0, #28]
	vmov	r4, r5, s24, s25
	vmov	r6, r7, s26, s27
	vmov	r8, r9, s28, s29
	vmov	r10, r11, s30, s31
	strt	r4, [r0, #32]
	strt	r5, [r0, #36]
	strt	r6, [r0, #40]
	strt	r7, [r0, #44]
	strt	r8, [r0, #48]
	strt	r9, [r0, #52]
	strt	r10, [r0, #56]
	strt	r11, [r0, #60]
	.global	fp_st_end
fp_st_end:
	b	fp_zero

/* fp_save(rp): fp_flush for C, with interrupts off (unshadow: the image
 * of rp, mpu_proc, is about to move; its FP registers must be saved into
 * it first, not into whatever lies at its addresses later). */
	.global	fp_save
	.thumb_func
fp_save:
	push	{r3-r11, lr}
	mov	r1, r0
	bl	fp_flush
	pop	{r3-r11, pc}

/* Its FP context is not to be saved: cancel a pending lazy save
 * (its frame may be gone), then clear the FPU. */
	.global	fp_drop
	.thumb_func
fp_drop:
	ldr	r2, =FPCCR
	ldr	r3, [r2]
	bic	r3, r3, #FPCCR_LSPACT
	str	r3, [r2]
fp_zero:
	movs	r2, #0
	movs	r3, #0
	vmov	d0, r2, r3
	vmov	d1, r2, r3
	vmov	d2, r2, r3
	vmov	d3, r2, r3
	vmov	d4, r2, r3
	vmov	d5, r2, r3
	vmov	d6, r2, r3
	vmov	d7, r2, r3
	vmov	d8, r2, r3
	vmov	d9, r2, r3
	vmov	d10, r2, r3
	vmov	d11, r2, r3
	vmov	d12, r2, r3
	vmov	d13, r2, r3
	vmov	d14, r2, r3
	vmov	d15, r2, r3
	vmsr	fpscr, r2
	mrs	r0, control		/* the vmovs made the kernel an FP */
	bic	r0, r0, #CONTROL_FPCA	/* context: undo, so that a fault */
	msr	control, r0		/* in it stacks no FP frame */
	bx	lr

	.ltorg
