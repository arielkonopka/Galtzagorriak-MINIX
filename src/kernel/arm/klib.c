/* Machine dependent kernel support for ARM Cortex-M: what klib88.s,
 * klib386.s, i8259.c, protect.c, start.c and exception.c do on the PC.
 */
#include "../kernel.h"
#include <signal.h>
#include <unistd.h>
#include <minix/boot.h>
#include <stdlib.h>
#include <minix/callnr.h>
#include <minix/com.h>
#include "../proc.h"
#include "armcpu.h"
#include "hal.h"
#include "fbcon.h"

extern const unsigned char splash_fb[];	/* flipper/hal/common/splash.c */

u32_t cpu_hz;
int k_booted;			/* set by the first SVC, see mpx.S */

PRIVATE u32_t kernel_stack[K_STACK_BYTES / sizeof(u32_t)]
				__attribute__((aligned(8)));

FORWARD _PROTOTYPE( void load_boot_images, (void) );

/*===========================================================================*
 *				arm_flash				     *
 *===========================================================================*/
PUBLIC int arm_flash(op, addr, src, len, basep, sizep)
int op;				/* FL_INFO, FL_ERASE or FL_WRITE */
u32_t addr;			/* flash address */
phys_bytes src;			/* data to program (FL_WRITE) */
unsigned len;			/* bytes to program */
u32_t *basep, *sizep;		/* region (all ops) */
{
/* Flash text cache operations for do_flash().  Only inside the region the
 * board sets aside for it.  The MPU keeps the flash read-only even for
 * privileged code, so it is off while the controller writes (only this
 * privileged task runs meanwhile: interrupts return to it).  Interrupts
 * stay enabled (R0.8c): during an erase every fetch from flash - handlers
 * included - simply waits for it; hal_flash_write masks them only around
 * the two stores of a double word.  MM calls this as idle work, one page
 * erase or 256 bytes at a time (fcache.c).
 */
  u32_t ctrl;
  int r;

  if (op == FL_INFO && (addr == FL_LAYOUT_INST || addr == FL_LAYOUT_SFSA)) {
	u32_t l[HAL_FL_N];		/* Phase 2b */

	hal_flash_layout(l);
	*basep = addr == FL_LAYOUT_INST ? l[HAL_FL_INST] : l[HAL_FL_SFSA];
	*sizep = addr == FL_LAYOUT_INST ? l[HAL_FL_INST_SIZE] : 0;
	return(OK);
  }
  hal_flash_region(basep, sizep);
  if (op == FL_INFO) return(OK);
  {	/* the cache or the installed programs (Phase 2b); only MM calls */
	u32_t l[HAL_FL_N], b = *basep, n = *sizep;

	if (addr - b >= n) {
		hal_flash_layout(l);
		b = l[HAL_FL_INST];
		n = l[HAL_FL_INST_SIZE];
	}
	if (addr < b || addr - b >= n) return(EINVAL);
	if (op == FL_WRITE && (len > n - (addr - b) || len % 8 != 0))
		return(EINVAL);
  }
  if (op != FL_ERASE && op != FL_WRITE) return(EINVAL);

#if LATTRACE
  lat_flash(0);
#endif
  ctrl = MPU_CTRL;
  MPU_CTRL = 0;
  *(volatile u32_t *) 0xE000E008 |= 2;	/* ACTLR.DISDEFWBUF: precise faults */
  __asm volatile ("dsb; isb");
  r = op == FL_ERASE ? hal_flash_erase(addr)
		     : hal_flash_write(addr, (const void *) src, len);
  MPU_CTRL = ctrl;
  *(volatile u32_t *) 0xE000E008 &= ~2;
  __asm volatile ("dsb; isb");
#if LATTRACE
  lat_flash(1);
#endif
  if (r != 0) {
	static int reported;
	if (reported++ < 4)
		printf("flash: %s 0x%x failed, SR 0x%x\n",
			op == FL_ERASE ? "erase" : "write", addr, hal_flash_sr);
	if (reported <= 4)
		printf("flash: now SR %x CR %x RCC_CR %x OPTR %x SFR %x\n",
			hal_flash_dbg[0], hal_flash_dbg[1], hal_flash_dbg[2],
			hal_flash_dbg[3], hal_flash_dbg[4]);
	return(EIO);
  }
  return(OK);
}


/*===========================================================================*
 *				kstack_used				     *
 *===========================================================================*/
PUBLIC unsigned kstack_used(size)
unsigned *size;
{
/* High-water mark of the kernel (exception) stack, for do_kstacks(). */
  int i = 0;

  *size = sizeof kernel_stack;
  while (i < sizeof kernel_stack / sizeof kernel_stack[0]
					&& kernel_stack[i] == STACK_GUARD) i++;
  return sizeof kernel_stack - i * sizeof kernel_stack[0];
}
FORWARD _PROTOTYPE( long env_number, (char *name) );

extern struct bparam_s boot_parameters;		/* system.c */

PRIVATE long env_number(name)
char *name;
{
  char *v = k_getenv(name);

  return v == NIL_PTR ? 0 : strtol(v, (char **) NULL, 10);
}

/*===========================================================================*
 *				cstart					     *
 *===========================================================================*/
PUBLIC void cstart()
{
/* Early machine setup, before main() does the portable initialization.
 * Interrupts are masked with BASEPRI until the first process starts.
 */
  __asm volatile ("msr basepri, %0" : : "r" (KERNEL_PRIO));

  hal_init();			/* clocks, debug UART, GPIO */
  /* hal_init() starts a 1 ms SysTick, possibly at a priority BASEPRI does
   * not mask; stop it at once - the clock task programs it later.  A tick
   * now would enter the kernel with proc_ptr == NULL. */
  SYST_CSR = 0;
  SCB_ICSR = 1u << 25;		/* PENDSTCLR */
  cpu_hz = hal_cpu_hz();
#ifdef BOARD_FLIPPER
  /* Boot screen; it stays until the launcher takes over the display.
   * Boot messages go to the console (USB) instead. */
  hal_lcd_init();		/* also turns the backlight on */
  hal_lcd_flush(splash_fb);
#endif

  code_base = (phys_bytes) _stext;
  data_base = 0;		/* flat: kernel virtual == physical */
  k_reenter = 0;		/* main() -> restart() makes it -1 */
  kernel_sp_top = (reg_t) &kernel_stack[sizeof kernel_stack / sizeof
							kernel_stack[0]];
  {	/* still running on the reset stack: paint the kernel stack for
	 * the high-water mark (kstack_used()) */
	int i;
	for (i = 0; i < sizeof kernel_stack / sizeof kernel_stack[0]; i++)
		kernel_stack[i] = STACK_GUARD;
  }

  /* Trap divide by zero; fault handlers instead of escalation. */
  SCB_CCR |= CCR_DIV_0_TRP | CCR_STKALIGN;
  SCB_SHCSR |= SHCSR_ENABLES;

  /* MPU: code (flash) is read-only (and executable) for everyone, so stray
   * writes fault (on QEMU the "flash" is RAM and would silently be
   * overwritten).  Privileged code - kernel, tasks, MM, FS - uses the
   * default map for everything else.  An unprivileged (user) process only
   * gets its own memory, set up by mpu_switch() on every switch to it.
   */
  MPU_RNR = 0;
  /* All 1 MB: the installed programs and the text cache reach up to
   * CPU2's area (R2b.1); CPU2's own pages refuse CPU1 by themselves. */
  MPU_RBAR = (u32_t) _stext & ~0xFFFFFu;
  MPU_RASR = MPU_RASR_AP_RO_RO | (19 << 1) | MPU_RASR_ENABLE; /* 2^20 */
  MPU_CTRL = MPU_CTRL_PRIVDEFENA | MPU_CTRL_ENABLE;
  __asm volatile ("dsb; isb");

  load_boot_images();

  /* Boot parameters, as the boot monitor would pass them on the PC. */
  boot_parameters.bp_rootdev = env_number("rootdev");
  boot_parameters.bp_ramimagedev = env_number("ramimagedev");
  boot_parameters.bp_ramsize = env_number("ramsize");
  boot_parameters.bp_processor = 0;
}


/*===========================================================================*
 *				load_boot_images			     *
 *===========================================================================*/
PRIVATE struct boot_image *boot_image[NR_BOOT_SERVERS];

PRIVATE void load_boot_images()
{
/* Find the MM, FS and INIT images that follow the kernel in flash, copy
 * their initialised data to RAM and clear their bss.
 */
  struct boot_image *bi;
  phys_bytes p;
  int i;

  p = (phys_bytes) _boot_images;
  for (i = 0; i < NR_BOOT_SERVERS; i++) {
	bi = (struct boot_image *) p;
	if (bi->magic != BOOT_IMAGE_MAGIC)
		panic("boot image missing for server", i);
	if ((bi->text_start & (CLICK_SIZE - 1)) != 0
	    || (bi->data_start & (CLICK_SIZE - 1)) != 0)
		panic("boot image not click aligned", i);
	memcpy((void *) bi->data_start, (void *) bi->data_load, bi->data_size);
	memset((char *) bi->data_start + bi->data_size, 0,
					bi->bss_size + bi->stack_size);
	boot_image[i] = bi;
	p = bi->text_start + bi->text_size;
	p = (p + CLICK_SIZE - 1) & ~(phys_bytes) (CLICK_SIZE - 1);
  }
}


/*===========================================================================*
 *				arm_proc_layout				     *
 *===========================================================================*/
PUBLIC void arm_proc_layout(rp, t)
struct proc *rp;
int t;
{
/* Set up the memory map, stack pointer, PSW and CONTROL of task or boot
 * server t (called from main()).  Tasks share the kernel: text in flash,
 * data/bss/stacks in RAM.  On this machine virtual addresses are physical
 * addresses, so mem_vir == mem_phys for every segment.
 */
  struct boot_image *bi;
  phys_clicks tbase, tlen, dbase, dlen;

  if (t < 0) {
	tbase = (phys_bytes) _stext >> CLICK_SHIFT;
	tlen = (((phys_bytes) _etext + CLICK_SIZE - 1) >> CLICK_SHIFT) - tbase;
	dbase = (phys_bytes) _ram_start >> CLICK_SHIFT;
	dlen = (((phys_bytes) _ebss + CLICK_SIZE - 1) >> CLICK_SHIFT) - dbase;
	rp->p_reg.control = INIT_TASK_CONTROL;
	/* Tasks run inside the kernel: the stack is part of D. */
	rp->p_map[S].mem_phys = rp->p_map[S].mem_vir = dbase + dlen;
	rp->p_map[S].mem_len = 0;
  } else {
	/* A server: data and bss, then a gap for brk(), then the stack at the
	 * top, one click to start with.  (On the PC the stack lived inside D
	 * and MM did no checking for these processes, so INIT's heap could
	 * grow into its stack unnoticed.)  bi->stack_size covers gap + stack.
	 */
	phys_clicks total;

	bi = boot_image[t];
	tbase = bi->text_start >> CLICK_SHIFT;
	tlen = (bi->text_size + CLICK_SIZE - 1) >> CLICK_SHIFT;
	dbase = bi->data_start >> CLICK_SHIFT;
	dlen = (bi->data_size + bi->bss_size + CLICK_SIZE - 1) >> CLICK_SHIFT;
	total = (bi->data_size + bi->bss_size + bi->stack_size
					+ CLICK_SIZE - 1) >> CLICK_SHIFT;
	if (total < dlen + 2) panic("server stack too small", t);
	rp->p_map[S].mem_phys = rp->p_map[S].mem_vir = dbase + total - 1;
	rp->p_map[S].mem_len = 1;
	/* FS keeps its buffer cache in SRAM2 (fs/buf.h): its data segment
	 * must reach that far, or drivers would refuse (numap) to copy into
	 * the buffers.  Nothing else is affected: MM's free memory comes from
	 * mem_init() and FS never forks or calls brk. */
	if (t == FS_PROC_NR) dlen = (SRAM2_END >> CLICK_SHIFT) - dbase;
	rp->p_reg.pc = bi->entry;
	/* One word left for crtso's argc (0) as on the PC. */
	rp->p_reg.sp = ((dbase + total) << CLICK_SHIFT) - 2 * sizeof(reg_t);
	/* MM and FS are trusted like the tasks and run privileged (MM writes
	 * relocations straight into new process images); INIT and its
	 * descendants are ordinary, MPU-confined user processes. */
	rp->p_reg.control = (t < LOW_USER) ? INIT_TASK_CONTROL : INIT_CONTROL;
	memcpy(rp->p_name, bi->name, sizeof bi->name);
	rp->p_name[sizeof bi->name - 1] = 0;
  }
  rp->p_map[T].mem_phys = rp->p_map[T].mem_vir = tbase;
  rp->p_map[T].mem_len = tlen;
  rp->p_map[D].mem_phys = rp->p_map[D].mem_vir = dbase;
  rp->p_map[D].mem_len = dlen;
  rp->p_reg.psw = INIT_PSW;
}


/*===========================================================================*
 *				mpu_switch				     *
 *===========================================================================*/
#define MPU_FIRST	1		/* region 0 is the flash */
#define MPU_REGIONS	8
PUBLIC struct proc *mpu_proc;		/* whose regions are loaded */

#include <minix/mpucover.h>

PUBLIC void mpu_switch()
{
/* Called by restart() before entering proc_ptr.  Give an unprivileged
 * process access to its own memory only: text (if in RAM) up to the end of
 * the stack, which exec and fork keep contiguous.  Privileged processes run
 * with the default map; their regions are left as they are.
 */
  struct proc *rp = proc_ptr;
  unsigned long rbar[MPU_REGIONS], rasr[MPU_REGIONS];
  u32_t lo, hi;
  int i, n;
  static int warned;

#if LATTRACE
  lat_run(rp);
#endif
  if (rp == mpu_proc || !(rp->p_reg.control & CONTROL_NPRIV)) return;
  mpu_proc = rp;

  lo = (u32_t) rp->p_map[T].mem_phys << CLICK_SHIFT;
  if (lo < (u32_t) _ram_start || rp->p_map[T].mem_len == 0
	|| rp->p_map[T].mem_phys + rp->p_map[T].mem_len != rp->p_map[D].mem_phys)
	lo = (u32_t) rp->p_map[D].mem_phys << CLICK_SHIFT; /* text in flash */
  hi = (u32_t) (rp->p_map[S].mem_phys + rp->p_map[S].mem_len) << CLICK_SHIFT;

  n = mpu_cover(lo, hi, rbar, rasr, MPU_REGIONS - MPU_FIRST);
  if (n < 0) {
	/* Cannot be described (MM should have refused the layout): fail
	 * closed with no regions at all.  restart's unprivileged frame
	 * store then faults and the process gets SIGSEGV (S3, R1.6). */
	if (!warned++) printf("mpu: %s cannot be isolated, stopped\n",
							rp->p_name);
	n = 0;
  }
  for (i = 0; i < MPU_REGIONS - MPU_FIRST; i++) {
	MPU_RNR = MPU_FIRST + i;
	if (i < n) {
		MPU_RBAR = rbar[i];
		MPU_RASR = rasr[i];
	} else {
		MPU_RASR = 0;
	}
  }
}


/*===========================================================================*
 *				mem_init				     *
 *===========================================================================*/
PUBLIC void mem_init()
{
/* Free memory is everything in RAM above the last boot server. */
  struct boot_image *bi = boot_image[NR_BOOT_SERVERS - 1];
  phys_bytes lo, hi;

  lo = bi->data_start + bi->data_size + bi->bss_size + bi->stack_size;
  lo = (lo + CLICK_SIZE - 1) & ~(phys_bytes) (CLICK_SIZE - 1);
  hi = (phys_bytes) _ram_end & ~(phys_bytes) (CLICK_SIZE - 1);
  mem[0].base = lo >> CLICK_SHIFT;
  mem[0].size = hi > lo ? (hi - lo) >> CLICK_SHIFT : 0;
  mem[1].base = mem[1].size = 0;
  tot_mem_size = ((phys_bytes) _ram_end - (phys_bytes) _ram_start)
							>> CLICK_SHIFT;
}


/*===========================================================================*
 *				Interrupt control			     *
 *===========================================================================*/
PUBLIC void intr_init(mine)
int mine;
{
/* Set exception priorities so MINIX exceptions never nest (see mpx.S), and
 * disable every device interrupt until a driver asks for it.
 */
  int i;

  for (i = 0; i < NR_IRQ_VECTORS - 1; i++) {
	NVIC_ICER(i >> 5) = 1u << (i & 31);
	NVIC_IPR8(i) = KERNEL_PRIO;
	irq_slot[i] = 0;
  }
  irq_slot[CLOCK_IRQ] = 0;
  SCB_SHPR(EXC_SVCALL) = KERNEL_PRIO - 0x10;
  SCB_SHPR(EXC_PENDSV) = KERNEL_PRIO;
  SCB_SHPR(EXC_SYSTICK) = KERNEL_PRIO;
  SCB_SHPR(EXC_MEMMANAGE) = 0;
  SCB_SHPR(EXC_BUSFAULT) = 0;
  SCB_SHPR(EXC_USAGEFAULT) = 0;
}

PUBLIC void put_irq_handler(irq, handler)
int irq;
irq_handler_t handler;
{
  int s;

  if ((unsigned) irq >= NR_IRQ_VECTORS)
	panic("invalid call to put_irq_handler", irq);
  if (irq_slot[irq] != 0) {
	if (irq_handlers[irq_slot[irq] - 1] == handler) return; /* again */
	panic("attempt to register second irq handler for irq", irq);
  }
  for (s = 0; s < NR_IRQ_HANDLERS && irq_handlers[s] != 0; s++) ;
  if (s == NR_IRQ_HANDLERS) panic("too many irq handlers", irq);
  irq_handlers[s] = handler;
  irq_slot[irq] = s + 1;
}

PUBLIC void enable_irq(irq)
unsigned irq;
{
  if (irq >= NR_IRQ_VECTORS - 1) return;	/* CLOCK_IRQ: SysTick */
  NVIC_ISER(irq >> 5) = 1u << (irq & 31);
}

PUBLIC int disable_irq(irq)
unsigned irq;
{
/* Disable an interrupt; return true if it was enabled. */
  u32_t bit;
  int was;

  if (irq >= NR_IRQ_VECTORS - 1) return 0;
  bit = 1u << (irq & 31);
  was = (NVIC_ISER(irq >> 5) & bit) != 0;
  NVIC_ICER(irq >> 5) = bit;
  return was;
}

/*===========================================================================*
 *				bell					     *
 *===========================================================================*/
PUBLIC unsigned bell_ticks;	/* ticks until the speaker goes quiet */

PUBLIC void bell()
{
/* ^G on the console: a short beep (stopped from the clock tick). */
  hal_tone(2000);
  bell_ticks = HZ / 6;
}


PUBLIC void irq_dispatch(exc)
unsigned exc;
{
/* Called from mpx.S for SysTick and device interrupts.  As on the PC, a
 * handler returning zero leaves its interrupt disabled.
 */
  int irq;
  irq_handler_t handler;

  if (exc == EXC_SYSTICK) {
	hal_wdt_kick();			/* the clock still ticks: alive */
	if (bell_ticks != 0 && --bell_ticks == 0) hal_tone(0);
  }
  if (exc == EXC_PENDSV) {		/* raised by the idle task */
	shadow_kick();
	return;
  }
  irq = (exc == EXC_SYSTICK) ? CLOCK_IRQ : (int) exc - EXC_IRQ0;
  if ((unsigned) irq >= NR_IRQ_VECTORS) return;
  handler = irq_slot[irq] ? irq_handlers[irq_slot[irq] - 1] : 0;
  if (handler == 0) {
	(void) disable_irq(irq);	/* spurious: shut it up */
	return;
  }
  if ((*handler)(irq) == 0) (void) disable_irq(irq);
}

PUBLIC void lock()
{
  __asm volatile ("cpsid i" : : : "memory");
#if LATTRACE
  lat_lock();
#endif
}

PUBLIC void unlock()
{
#if LATTRACE
  lat_unlock();
#endif
  __asm volatile ("cpsie i" : : : "memory");
}


/*===========================================================================*
 *				phys_copy				     *
 *===========================================================================*/
PUBLIC void phys_copy(source, dest, count)
phys_bytes source, dest, count;
{
/* Physical addresses are just addresses here. */
  memcpy((void *) dest, (void *) source, (size_t) count);
}


/*===========================================================================*
 *				idle_task				     *
 *===========================================================================*/
PUBLIC void idle_task()
{
/* Nothing to run.  But a shadowed process may be ready, waiting for the
 * clock task to swap it in: PendSV wakes the clock task now rather than
 * at the next tick (a fork()ed pair taking turns: 16 ms per switch). */
  for (;;) {
	/* Check and sleep with interrupts masked (wfi still wakes up on a
	 * pending one): idle may be switched out anywhere, and must not come
	 * back straight into wfi with a shadowed process waiting. */
	__asm volatile ("cpsid i" : : : "memory");
	if (rdy_head[SHADOW_Q] != NIL_PROC) {
		SCB_ICSR = ICSR_PENDSVSET;
		__asm volatile ("dsb; cpsie i; isb" : : : "memory");
		continue;		/* PendSV ran; look again */
	}
	__asm volatile ("wfi; cpsie i; isb" : : : "memory");
  }
}


/*===========================================================================*
 *				Faults					     *
 *===========================================================================*/
PRIVATE void fault_regs(cfsr, hfsr)
u32_t cfsr, hfsr;
{
  printf("CFSR=%08lx HFSR=%08lx MMFAR=%08lx BFAR=%08lx\n",
	(unsigned long) cfsr, (unsigned long) hfsr,
	(unsigned long) SCB_MMFAR, (unsigned long) SCB_BFAR);
}

PUBLIC void arm_exception(exc)
unsigned exc;
{
/* A process caused a fault.  Users get a signal, like exception.c does on
 * the PC; a task or server faulting is fatal.
 */
  struct proc *rp = proc_ptr;
  u32_t cfsr = SCB_CFSR, hfsr = SCB_HFSR;
  int sig;

  SCB_CFSR = cfsr;		/* write 1 to clear */
  SCB_HFSR = hfsr;

  if (cfsr & 0x02000000) sig = SIGFPE;		/* DIVBYZERO */
  else if (cfsr & 0x000F0000) sig = SIGILL;	/* UNDEFINSTR..NOCP */
  else if (cfsr & 0x01000000) sig = SIGBUS;	/* UNALIGNED */
  else sig = SIGSEGV;

  if (!isuserp(rp) || k_reenter != 0) {
	printf("\n%s (exception %u) in %s, pc=%08lx lr=%08lx sp=%08lx\n",
		sig == SIGSEGV ? "memory fault" : "fault", exc, rp->p_name,
		(unsigned long) rp->p_reg.pc, (unsigned long) rp->p_reg.lr,
		(unsigned long) rp->p_reg.sp);
	fault_regs(cfsr, hfsr);
	panic("exception in system code", proc_number(rp));
  }
  /* Report it (the PC kernel is silent here, but a one-line note costs
   * nothing and saves a lot of guessing on a machine without an MMU). */
  printf("%s[%d]: signal %d, pc=%08lx lr=%08lx sp=%08lx cfsr=%08lx addr=%08lx\n",
	rp->p_name, proc_number(rp), sig, (unsigned long) rp->p_reg.pc,
	(unsigned long) rp->p_reg.lr, (unsigned long) rp->p_reg.sp,
	(unsigned long) cfsr,
	(unsigned long) ((cfsr & 0x80) ? SCB_MMFAR :
			 (cfsr & 0x8000) ? SCB_BFAR : 0));
  printf("    text %08lx+%lx data %08lx shadow %x\n",
	(unsigned long) rp->p_map[T].mem_phys << CLICK_SHIFT,
	(unsigned long) rp->p_map[T].mem_len << CLICK_SHIFT,
	(unsigned long) rp->p_map[D].mem_phys << CLICK_SHIFT, rp->p_shadow);
  cause_sig(proc_number(rp), sig);
}

/*===========================================================================*
 *				user_frame_fault			     *
 *===========================================================================*/
PUBLIC void user_frame_fault()
{
/* The unprivileged load or store of proc_ptr's exception frame faulted
 * (mpx.S, frame_recover): its stack pointer is not in its memory.  It gets
 * SIGSEGV (a kernel SIGKILL would mean "everybody", do_ksig in MM); until
 * MM has handled that it is not runnable (cause_sig unreadies it).  Its
 * stack pointer is set to the top of its own stack, so the signal frame
 * and any later restart are inside its memory.
 */
  struct proc *rp = proc_ptr;

  printf("%s[%d]: bad stack pointer %08lx, signal 11\n", rp->p_name,
		proc_number(rp), (unsigned long) rp->p_reg.sp);
  rp->p_reg.sp = (reg_t) ((rp->p_map[S].mem_vir + rp->p_map[S].mem_len)
							<< CLICK_SHIFT);
  /* The frame could not be stacked, so an SVC this process made is still
   * pending: it would be taken later in whatever process runs next. */
  SCB_SHCSR &= ~SHCSR_SVCALLPENDED;
  cause_sig(proc_number(rp), SIGSEGV);

}

PUBLIC void kernel_fault_report(frame, exc)
u32_t *frame;
unsigned exc;
{
/* Fault in Handler mode (or before the first process ran): kernel bug. */
  printf("\nKernel fault, exception %u: pc=%08lx lr=%08lx psr=%08lx\n",
	exc, (unsigned long) frame[6], (unsigned long) frame[5],
	(unsigned long) frame[7]);
  printf("r0=%08lx r1=%08lx r2=%08lx r3=%08lx r12=%08lx\n",
	(unsigned long) frame[0], (unsigned long) frame[1],
	(unsigned long) frame[2], (unsigned long) frame[3],
	(unsigned long) frame[4]);
  fault_regs(SCB_CFSR, SCB_HFSR);
  if (k_booted) {
	struct proc *rp = proc_ptr;

	printf("current process %d (%s): r0=%08lx r1=%08lx r2=%08lx "
		"pc=%08lx lr=%08lx\n", proc_number(rp), rp->p_name,
		(unsigned long) rp->p_reg.retreg, (unsigned long) rp->p_reg.r1,
		(unsigned long) rp->p_reg.r2, (unsigned long) rp->p_reg.pc,
		(unsigned long) rp->p_reg.lr);
  }
  wreboot(RBT_PANIC);
}


/*===========================================================================*
 *				wreboot					     *
 *===========================================================================*/
PUBLIC void wreboot(how)
int how;
{
/* Halt or reboot.  There is no boot monitor to return to. */

  lock();
  clock_stop();
  if (how == RBT_PANIC) printf("System halted after panic.\n");
  else if (how == RBT_HALT) printf("System halted.\n");
  if (how == RBT_MONITOR) {
	/* No boot monitor: "return to the monitor" means ST's ROM DFU
	 * loader, so a new image can be flashed without the buttons. */
	printf("Rebooting to DFU.\n");
	hal_dfu_request();
  }
  if (how == RBT_REBOOT || how == RBT_RESET) {
	SCB_AIRCR = AIRCR_VECTKEY | AIRCR_SYSRESETREQ;
	for (;;) ;
  }
  hal_halt(how == RBT_PANIC ? 1 : 0);
}


/*===========================================================================*
 *				k_getenv				     *
 *===========================================================================*/
/* Boot parameters.  The root file system is the first partition of the SD
 * card: /dev/hd1 = (major 3, minor 1) = 0x0301 = 769.  No RAM disk. */
PRIVATE char k_environ[] = "rootdev=769\0ramimagedev=769\0ramsize=0\0";

PUBLIC char *k_getenv(name)
char *name;
{
  register char *namep;
  register char *envp;

  for (envp = k_environ; *envp != 0;) {
	for (namep = name; *namep != 0 && *namep == *envp; namep++, envp++)
		;
	if (*namep == '\0' && *envp == '=') return(envp + 1);
	while (*envp++ != 0)
		;
  }
  return(NIL_PTR);
}
