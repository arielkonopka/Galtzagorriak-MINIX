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
  MPU_RBAR = (u32_t) _stext & ~0x7FFFFu;
  MPU_RASR = MPU_RASR_AP_RO_RO | (18 << 1) | MPU_RASR_ENABLE; /* 2^19 */
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

PRIVATE int mpu_cover(lo, hi, rbar, rasr, max)
u32_t lo, hi;			/* [lo, hi), multiples of 256 */
u32_t *rbar, *rasr;
int max;
{
/* Cover [lo, hi) exactly with at most max MPU regions, using subregions:
 * each step takes the largest aligned power-of-two region whose subregion
 * boundaries match lo and the covered end.  Returns the number used, or
 * -1 if max is not enough.
 */
  int n = 0, k, s0, s1;
  u32_t size, base, sub, end;

  while (lo < hi) {
	if (n == max) return -1;
	for (k = 31; k >= 8; k--) {
		size = (u32_t) 1 << k;
		base = lo & ~(size - 1);
		sub = size >> 3;
		if (((lo - base) & (sub - 1)) != 0) continue;
		end = hi - base > size ? base + size : hi;
		if (((end - base) & (sub - 1)) != 0 || end <= lo) continue;
		s0 = (lo - base) / sub;
		s1 = (end - base) / sub;
		rbar[n] = base;
		rasr[n] = MPU_RASR_AP_RW_RW | ((u32_t) (k - 1) << 1)
			| ((~(((1u << s1) - 1) & ~((1u << s0) - 1)) & 0xFF) << 8)
			| MPU_RASR_ENABLE;
		n++;
		lo = end;
		break;
	}
	if (k < 8) return -1;
  }
  return n;
}

PUBLIC void mpu_switch()
{
/* Called by restart() before entering proc_ptr.  Give an unprivileged
 * process access to its own memory only: text (if in RAM) up to the end of
 * the stack, which exec and fork keep contiguous.  Privileged processes run
 * with the default map; their regions are left as they are.
 */
  struct proc *rp = proc_ptr;
  u32_t rbar[MPU_REGIONS], rasr[MPU_REGIONS], lo, hi;
  int i, n;
  static int warned;

  if (rp == mpu_proc || !(rp->p_reg.control & CONTROL_NPRIV)) return;
  mpu_proc = rp;

  lo = (u32_t) rp->p_map[T].mem_phys << CLICK_SHIFT;
  if (lo < (u32_t) _ram_start || rp->p_map[T].mem_len == 0
	|| rp->p_map[T].mem_phys + rp->p_map[T].mem_len != rp->p_map[D].mem_phys)
	lo = (u32_t) rp->p_map[D].mem_phys << CLICK_SHIFT; /* text in flash */
  hi = (u32_t) (rp->p_map[S].mem_phys + rp->p_map[S].mem_len) << CLICK_SHIFT;

  n = mpu_cover(lo, hi, rbar, rasr, MPU_REGIONS - MPU_FIRST);
  if (n < 0) {
	/* Cannot be described: fall back to all of RAM (no isolation). */
	if (!warned++) printf("mpu: %s too fragmented, not isolated\n",
							rp->p_name);
	n = mpu_cover((u32_t) _ram_start & ~0x3FFFFu,
			((u32_t) _ram_start & ~0x3FFFFu) + 0x40000,
			rbar, rasr, MPU_REGIONS - MPU_FIRST);
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
	irq_table[i] = 0;
  }
  irq_table[CLOCK_IRQ] = 0;
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
  if ((unsigned) irq >= NR_IRQ_VECTORS)
	panic("invalid call to put_irq_handler", irq);
  if (irq_table[irq] == handler) return;	/* extra initialization */
  if (irq_table[irq] != 0)
	panic("attempt to register second irq handler for irq", irq);
  irq_table[irq] = handler;
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

PUBLIC void irq_dispatch(exc)
unsigned exc;
{
/* Called from mpx.S for SysTick and device interrupts.  As on the PC, a
 * handler returning zero leaves its interrupt disabled.
 */
  int irq;
  irq_handler_t handler;

  irq = (exc == EXC_SYSTICK) ? CLOCK_IRQ : (int) exc - EXC_IRQ0;
  if ((unsigned) irq >= NR_IRQ_VECTORS) return;
  handler = irq_table[irq];
  if (handler == 0) {
	(void) disable_irq(irq);	/* spurious: shut it up */
	return;
  }
  if ((*handler)(irq) == 0) (void) disable_irq(irq);
}

PUBLIC void lock()
{
  __asm volatile ("cpsid i" : : : "memory");
}

PUBLIC void unlock()
{
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
  for (;;) __asm volatile ("wfi");
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
