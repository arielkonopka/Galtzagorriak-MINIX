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
#define MPU_TOP		7	/* the highest MPU region: wins over the rest */

PRIVATE u32_t kernel_stack[K_STACK_BYTES / sizeof(u32_t)]
				__attribute__((aligned(8)));

FORWARD _PROTOTYPE( void load_boot_images, (void) );

/*===========================================================================*
 *				arm_flash				     *
 *===========================================================================*/
/* Days of uptime: the erase budget's day when the RTC is not running. */
#define UPDAY	((unsigned) ((unsigned long) get_uptime() / (HZ * 86400UL)))

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
  u32_t rbar7, rasr7;
  int r;

  if (op == FL_INFO && (addr == FL_LAYOUT_INST || addr == FL_LAYOUT_SFSA
						|| addr == FL_LAYOUT_ROM)) {
	u32_t l[HAL_FL_N];		/* Phase 2b */

	hal_flash_layout(l);
	*basep = addr == FL_LAYOUT_INST ? l[HAL_FL_INST]
		: addr == FL_LAYOUT_ROM ? l[HAL_FL_ROM] : l[HAL_FL_SFSA];
	*sizep = addr == FL_LAYOUT_INST ? l[HAL_FL_INST_SIZE]
		: addr == FL_LAYOUT_ROM ? l[HAL_FL_ROM_SIZE] : 0;
	return(OK);
  }
  if (op == FL_INFO && (addr == FL_COUNTS || addr == FL_BUDGET)) {
	u32_t c[4];			/* R2b.11: backup registers */

	if (addr == FL_BUDGET && len != 0) hal_fc_budget(len - 1, UPDAY);
	hal_fc_counts(c, UPDAY);
	*basep = addr == FL_COUNTS ? c[0] : c[3];
	*sizep = addr == FL_COUNTS ? c[1] : c[2];
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
		/* the settings image is written by DFU only (L.2) */
		if (addr - l[HAL_FL_ROM] < l[HAL_FL_ROM_SIZE]
		    || (op == FL_WRITE && addr < l[HAL_FL_ROM]
				&& addr + len > l[HAL_FL_ROM]))
			return(EROFS);
	}
	if (addr < b || addr - b >= n) return(EINVAL);
	if (op == FL_WRITE && (len > n - (addr - b) || len % 8 != 0))
		return(EINVAL);
	/* every erase counted; the cache's within its daily budget
	 * (R2b.11): when spent, MM keeps the text in RAM */
	if (op == FL_ERASE && !hal_fc_erase_ok(b == *basep, UPDAY))
		return(ENOSPC);
	if (op == FL_ERASE && (pm_busy & PM_LOWBAT))	/* R6.1 */
		return(ENOSPC);
  }
  if (op != FL_ERASE && op != FL_WRITE) return(EINVAL);

#if LATTRACE
  lat_flash(0);
#endif
  /* R4a.3: the MPU stays on.  The highest region (it wins over the
   * read-only flash region 0 and a process's regions) opens the target
   * page - or the smallest aligned block holding the range - privileged
   * read/write for the operation, then gets back what it held. */
  {
	u32_t lo = op == FL_ERASE ? addr & ~(u32_t) 4095 : addr;
	u32_t hi = op == FL_ERASE ? lo + 4096 : addr + len;
	int k = 12;

	while (k < 20 && (lo >> k) != ((hi - 1) >> k)) k++;
	MPU_RNR = MPU_TOP;
	rbar7 = MPU_RBAR;
	rasr7 = MPU_RASR;
	MPU_RBAR = lo & ~((1u << k) - 1);
	/* no XN: a range across a large boundary widens the block (up to
	 * 1 MB, all of flash), and handlers run from flash meanwhile */
	MPU_RASR = MPU_RASR_AP_RW_NO | (u32_t) (k - 1) << 1
							| MPU_RASR_ENABLE;
  }
  *(volatile u32_t *) 0xE000E008 |= 2;	/* ACTLR.DISDEFWBUF: precise faults */
  __asm volatile ("dsb; isb");
  r = op == FL_ERASE ? hal_flash_erase(addr)
		     : hal_flash_write(addr, (const void *) src, len);
  MPU_RNR = MPU_TOP;
  MPU_RBAR = rbar7;
  MPU_RASR = rasr7;
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
	if (i < 2) {		/* MM, FS: no heap; mark the stack (free -k) */
		reg_t *w = (reg_t *) (bi->data_start + bi->data_size
							+ bi->bss_size);
		reg_t *e = (reg_t *) ((char *) w + bi->stack_size);

		w = (reg_t *) (((phys_bytes) w + 3) & ~(phys_bytes) 3);
		while (w < e) *w++ = STACK_GUARD;
	}
	boot_image[i] = bi;
	p = bi->text_start + bi->text_size;
	p = (p + CLICK_SIZE - 1) & ~(phys_bytes) (CLICK_SIZE - 1);
  }
}


/*===========================================================================*
 *				server_stack_used			     *
 *===========================================================================*/
PUBLIC unsigned server_stack_used(t, sizep)
int t;				/* MM or FS (marked in arm_load) */
unsigned *sizep;
{
/* The high-water mark of a server's stack, gap included (free -k). */
  struct boot_image *bi = boot_image[t];
  reg_t *w = (reg_t *) ((bi->data_start + bi->data_size + bi->bss_size + 3)
							& ~(phys_bytes) 3);
  reg_t *end = (reg_t *) (bi->data_start + bi->data_size + bi->bss_size
							+ bi->stack_size);

  *sizep = bi->stack_size;
  while (w < end && *w == STACK_GUARD) w++;
  return((char *) end - (char *) w);
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
  u32_t lo, hi, dend, sbase, xn;
  int i, n;
  static int warned;

#if LATTRACE
  lat_run(rp);
#endif
  DWT_COMP(0) = (u32_t) &kernel_stack[0];		/* R4a.6 */
  if (istaskp(rp) && rp->p_stguard != NULL) DWT_COMP(1) = (u32_t) rp->p_stguard;
  if (rp == mpu_proc || !(rp->p_reg.control & CONTROL_NPRIV)) return;
  mpu_proc = rp;

  lo = (u32_t) rp->p_map[T].mem_phys << CLICK_SHIFT;
  xn = 0;
  if (lo < (u32_t) _ram_start || rp->p_map[T].mem_len == 0
	|| rp->p_map[T].mem_phys + rp->p_map[T].mem_len != rp->p_map[D].mem_phys) {
	lo = (u32_t) rp->p_map[D].mem_phys << CLICK_SHIFT; /* text in flash */
	xn = MPU_RASR_XN;	/* R4a.2: nothing in its RAM is code */
  }
  hi = (u32_t) (rp->p_map[S].mem_phys + rp->p_map[S].mem_len) << CLICK_SHIFT;

  n = mpu_cover(lo, hi, rbar, rasr, MPU_REGIONS - MPU_FIRST);
  for (i = 0; i < n; i++) rasr[i] |= xn;
  /* R4a.2: a 32 B no-access guard at the break (the end of D, where the
   * gap to the stack starts), in the highest region used so it wins over
   * the cover: a stack that runs down into the heap faults there instead
   * of overwriting it.  Only if a region is left (the cover first, R1.6). */
  dend = (u32_t) (rp->p_map[D].mem_phys + rp->p_map[D].mem_len) << CLICK_SHIFT;
  sbase = (u32_t) rp->p_map[S].mem_phys << CLICK_SHIFT;
  if (n >= 0 && n < MPU_REGIONS - MPU_FIRST && sbase >= dend + 32) {
	rbar[n] = dend;
	/* the kernel copies into a process's memory: privileged RW */
	rasr[n] = MPU_RASR_AP_RW_NO | MPU_RASR_XN | (4u << 1)	/* 2^5 */
							| MPU_RASR_ENABLE;
	n++;
  }
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
  SCB_SHPR(EXC_DEBUGMON) = 0;
  /* R4a.6: DWT comparator 0 watches the kernel stack's guard word, 1 the
   * running task's (mpu_switch moves it); a write to either raises the
   * DebugMonitor exception, reported as a stack overflow.  Armed from the
   * first switch on: main() still fills the stacks. */
  DEMCR |= DEMCR_TRCENA | DEMCR_MON_EN;
  DWT_MASK(0) = DWT_MASK(1) = 2;		/* one word */
  DWT_COMP(0) = DWT_COMP(1) = 0;
  DWT_FUNC(0) = DWT_FUNC(1) = DWT_FUNC_WRITE;
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
  /* One handler for several lines (the buttons' EXTI) takes one slot. */
  for (s = 0; s < NR_IRQ_HANDLERS && irq_handlers[s] != 0
					&& irq_handlers[s] != handler; s++) ;
  if (s == NR_IRQ_HANDLERS) panic("too many irq handlers", irq);
  irq_handlers[s] = handler;
  irq_slot[irq] = s + 1;
}

/*===========================================================================*
 *				The ms timer				     *
 *===========================================================================*/
/* R4b.2: a kernel client (MT_* in const.h) asks for a call of its function
 * in 1..999 ms; it runs in interrupt context and may arm itself again.
 * The HAL timer (LPTIM1, which keeps counting in Stop2) has one compare,
 * holding the earliest due time.  A compare write takes effect a few
 * timer clocks later and must not be followed by another before that:
 * mt.wr is set until its "ready" interrupt, which programs the next one,
 * so nobody waits.  Overdue clients are called at that interrupt too, so
 * a due time that passed while a write was under way is not lost.  User
 * alarms with ms resolution (setitimer, poll) come with R4b.6. */
#define MT_MIN	4		/* timer clocks: more than a write takes */
FORWARD _PROTOTYPE( void pm_wake, (void) );
PRIVATE void (* const mt_func[MT_N])(void) = {
	flipdev_btn,		/* MT_BUTTONS */
	pm_wake,		/* MT_IDLE */
	flipdev_vibro_off,	/* MT_VIBRO */
	flipdev_adc_poll,	/* MT_ADC */
	flipdev_led_tick,	/* MT_LED */
	flipdev_pulse_poll,	/* MT_PULSE */
};
PRIVATE struct {		/* one object: 4 B, not 8 with alignment */
	u16_t at[MT_N];		/* due, in timer clocks */
	u8_t on, wr;		/* armed clients; a compare write under way */
} mt;
FORWARD _PROTOTYPE( int mt_int, (int irq) );
FORWARD _PROTOTYPE( void mt_program, (void) );

PUBLIC void mt_init()
{
  int irq = hal_mt_irq();

  if (irq < 0) return;
  put_irq_handler(irq, mt_int);
  enable_irq(irq);
}

PUBLIC void mt_arm(client, ms)
int client;
unsigned ms;			/* 1..999; 0 disarms */
{
/* Interrupts off: call it from an interrupt handler or under lock(). */
  if (ms == 0) {
	mt.on &= ~(1 << client);
	return;
  }
  mt.at[client] = hal_mt_now() + (ms * HAL_MT_HZ + 999) / 1000;
  mt.on |= 1 << client;
  mt_program();
}

PRIVATE void mt_program()
{
/* The earliest due time into the compare, if it is not there already:
 * every write brings an interrupt, which comes back here. */
  unsigned now, d, best = 0x8000, due = 0;
  int c;

  if (mt.wr || mt.on == 0) return;
  now = hal_mt_now();
  for (c = 0; c < MT_N; c++) {
	if (!(mt.on & (1 << c))) continue;
	d = (u16_t) (mt.at[c] - now);
	if (d >= 0x8000) d = 0;			/* overdue */
	if (d < best) { best = d; due = mt.at[c]; }
  }
  if (best < MT_MIN) due = now + MT_MIN;
  else if (hal_mt_cmp() == due) return;
  hal_mt_set(due);
  mt.wr = TRUE;
}

PRIVATE int mt_int(irq)
int irq;
{
  unsigned now;
  int c;

  if (hal_mt_ack() & HAL_MT_READY) mt.wr = FALSE;
  now = hal_mt_now();
  for (c = 0; c < MT_N; c++) {
	if ((mt.on & (1 << c)) && (u16_t) (now - mt.at[c]) < 0x8000) {
		mt.on &= ~(1 << c);
		(*mt_func[c])();
	}
  }
  mt_program();
  return 1;
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
  hal_tone(2000, 100);
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

  if (exc == EXC_SYSTICK) {		/* the watchdog: clock task */
	if (bell_ticks != 0 && --bell_ticks == 0) hal_tone(0, 0);
  }
  if (exc == EXC_PENDSV) {	/* raised by the idle task or task_yield() */
	struct proc *rp = rdy_head[TASK_Q];

	/* A running task that other tasks wait behind goes to the end of
	 * the task queue (its context is saved by now). */
	if (rp == proc_ptr && rp != NIL_PROC && rp->p_nextready != NIL_PROC) {
		rdy_head[TASK_Q] = rp->p_nextready;
		rdy_tail[TASK_Q]->p_nextready = rp;
		rdy_tail[TASK_Q] = rp;
		rp->p_nextready = NIL_PROC;
		proc_ptr = rdy_head[TASK_Q];
	}
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

/* R4a.1: lock() masks the kernel's interrupts with BASEPRI, not PRIMASK,
 * so a fault inside a locked section (priority 0) is still taken as what
 * it is (MemManage, BusFault, UsageFault) and not escalated to HardFault.
 * Before the first process (k_booted) BASEPRI stays as cstart() left it.
 * unhold() ends locked: restart (mpx.S) clears BASEPRI on the way out. */
PUBLIC void lock()
{
  __asm volatile ("msr basepri, %0" : : "r" (KERNEL_PRIO) : "memory");
#if LATTRACE
  lat_lock();
#endif
}

PUBLIC void unlock()
{
#if LATTRACE
  lat_unlock();
#endif
  if (k_booted) __asm volatile ("msr basepri, %0" : : "r" (0) : "memory");
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
 *				task_yield				     *
 *===========================================================================*/
PUBLIC void task_yield()
{
/* Tasks are never preempted by tasks: a long driver path calls this between
 * pieces of its work (R0.8b, at most 4 KB), so that a task woken meanwhile
 * (an infrared ring to refill, a button) runs first.  The caller must leave
 * its device and shared state consistent: anything may run before it goes
 * on.  PendSV saves its context, then irq_dispatch puts it last.
 */
  if (proc_ptr->p_nextready == NIL_PROC) return;	/* nobody waits */
  SCB_ICSR = ICSR_PENDSVSET;
  __asm volatile ("dsb; isb" : : : "memory");
}


/*===========================================================================*
 *				pm_sleep				     *
 *===========================================================================*/
/* Tickless idle and Stop2 (T14b, R6.8).  Nothing runs, no sleep blocker:
 * the next work of the clock interrupt is some ticks away, so the clock
 * stops, the ms timer (which counts on in Stop2) is set for then, and the
 * CPU goes to Stop2 until it or anything else (a button) wakes it.  The
 * time slept goes into the uptime, and SysTick starts again with the rest
 * of the tick it was in, so the clock keeps the LSE's time.
 *
 * The watchdog (2 s, it keeps running in Stop2: stock option bytes) is
 * left as it is (option b of T14b): no sleep is longer than a second, and
 * every wake-up after a whole tick or more raises the clock interrupt at
 * once with the ticks slept, so the clock task (it kicks the watchdog)
 * runs whenever a 32-tick boundary passed.
 */
#define PM_MAX	(HZ - 2)	/* ticks: below the ms timer's 999 ms */
#if LATTRACE			/* why not: lat counts the reasons */
#define PM_NOT(r)	{ lat_nosleep(r); return(0); }
#else
#define PM_NOT(r)	return(0)
#endif

/* R6.1, once a minute from the clock task (no interrupt lines from the
 * gauge or the charger):
 * - battery below PM_LOW_MV: no new flash erases (an install that starts
 *   would risk the battery cutting out in the middle; MM keeps the text in
 *   RAM, as with a spent budget); back above with 100 mV of hysteresis;
 * - on a charger: pause charging outside 0-45 C by the gauge's temperature
 *   (the board has no thermistor on the charger's TS pin), resume between
 *   2 and 43 C. */
#define PM_LOW_MV	3300

PUBLIC void pm_minute()
{
  int v, t;

  subghz_minute();		/* a forgotten receiver: asleep (R8.1) */
  if ((v = hal_gauge_read(HAL_GAUGE_VOLT)) > 0) {
	if (v < PM_LOW_MV) pm_busy |= PM_LOWBAT;
	else if (v >= PM_LOW_MV + 100) pm_busy &= ~PM_LOWBAT;
  }
  if (!(pm_busy & PM_VBUS) && !hal_usb_awake()) return;
  if ((t = hal_gauge_read(HAL_GAUGE_TEMP)) < 0) return;
  t -= 2731;				/* 0.1 K -> 0.1 C */
  if (t < 0 || t > 450) (void) hal_charger_set(HAL_CHG_PAUSE, 1);
  else if (t >= 20 && t <= 430) (void) hal_charger_set(HAL_CHG_PAUSE, 0);
}

/*===========================================================================*
 *				flash ECC (R4a.5)			     *
 *===========================================================================*/
PUBLIC void NMI_Handler()
{
/* A double ECC error in flash (the only NMI source used): the data read
 * was wrong.  Say where, then stop: anything may have used it.  (The spec's
 * finer rule, kill only the users of a text-cache entry and drop the
 * entry, needs MM: later.) */
  u32_t a;
  unsigned f = hal_flash_ecc(&a);

  if (f & HAL_ECC_DOUBLE) {
	printf("\nflash ECC: uncorrectable error at %08lx (%s)\n",
		(unsigned long) a, k_booted ? proc_ptr->p_name : "boot");
	panic("flash ECC error", NO_NUM);
  }
  panic("NMI", NO_NUM);
}

PUBLIC void ecc_poll()
{
/* The clock task: corrected single-bit errors are worth a line (a page
 * wearing out). */
  u32_t a;

  if (hal_flash_ecc(&a) & HAL_ECC_FIXED)
	printf("flash ECC: corrected a bit at %08lx\n", (unsigned long) a);
}

PUBLIC void pm_vbus()
{
/* The clock task, whenever it runs (at least every 32 ticks while awake,
 * and after each Stop2 sleep of a tick or more): with the USB bus asleep,
 * ask the charger whether VBUS is there.  A cable put in during Stop2 does
 * not reliably wake the CPU through the USB wake-up line (seen on the
 * device: the host's enumeration was lost, the console never came back),
 * so with power on VBUS the CPU stays out of Stop2 and USB can start.  One
 * I2C read (~0.3 ms) a wake-up, on battery only. */
  int r;

  if (hal_usb_awake()) {
	pm_busy &= ~PM_VBUS;
	return;
  }
  r = hal_charger_read(HAL_CHG_STATUS);
  if (r >= 0 && (r & 0x04)) pm_busy |= PM_VBUS;		/* PG_STAT */
  else pm_busy &= ~PM_VBUS;
}

PRIVATE void pm_wake()
{
/* MT_IDLE: the wake-up itself is all. */
}

PRIVATE int pm_sleep()
{
/* Interrupts masked.  0: not now, wfi as before. */
  clock_t d, f;
  unsigned frac, e, us, n, rest, cyc, lp, want;
  int w;

  if ((pm_busy & PM_BLOCKERS) || bell_ticks || !hal_can_stop2()) PM_NOT(1);
  if (hal_usb_awake()) PM_NOT(2);
  d = clock_idle();
  if (d < 2) PM_NOT(3);
  if ((f = flipdev_idle(uptime_irq())) < d) d = f;
  if (d > PM_MAX) d = PM_MAX;
  if (d < 2) PM_NOT(4);

  /* The wake-up first: the compare write's own interrupt comes back to
   * the idle loop (wfi), the next turn sleeps.  Armed already, and not
   * later than now wanted (an alarm may have come closer): fine. */
  want = ((unsigned) d * 1000 / HZ) * HAL_MT_HZ / 1000;
  lp = (u16_t) (mt.at[MT_IDLE] - hal_mt_now());
  if (!(mt.on & (1 << MT_IDLE)) || lp > want + HAL_MT_HZ / HZ) {
	mt_arm(MT_IDLE, (unsigned) d * 1000 / HZ);
	PM_NOT(5);
  }
  if (mt.wr) PM_NOT(6);

  SYST_CSR = 0;
  if (SCB_ICSR & ICSR_PENDSTSET) {	/* a tick to count first */
	SYST_CSR = SYST_CSR_CLKSOURCE | SYST_CSR_TICKINT | SYST_CSR_ENABLE;
	PM_NOT(7);
  }
  frac = SYST_RVR - SYST_CVR;		/* cycles of this tick gone */
  lp = hal_mt_now();
#if LATTRACE
  cyc = hal_cycles();
#endif
  if ((w = hal_stop2()) < 0) {
	SYST_CSR = SYST_CSR_CLKSOURCE | SYST_CSR_TICKINT | SYST_CSR_ENABLE;
	PM_NOT(7);
  }
  e = (u16_t) (hal_mt_now() - lp);
#if LATTRACE
  lat_wake(hal_cycles() - cyc);
#endif
  pm_stat.stop += e;
  if (pm_stat.stop >= 32768UL * 60) {	/* R6.8: whole minutes apart */
	pm_stat.stop -= 32768UL * 60;
	pm_stat.min++;
  }
  if (w & HAL_WAKE_BTN) pm_stat.btn++; else pm_stat.other++;

  /* Microseconds since the last tick (32768 Hz: 15625/512 us a clock),
   * whole ticks in them, and the cycles left of the one we are in. */
  us = frac / (cpu_hz / 1000000) + e * 15625 / 512;
  n = us * HZ / 1000000;
  rest = us * HZ % 1000000;		/* us x HZ into this tick */
  cyc = (1000000 - rest) / HZ * (cpu_hz / 1000000);
  if (cyc < 64) cyc = 64;
  if (n > 0) {
	/* The clock interrupt at once, before the one that woke us (SysTick
	 * goes first at equal priority), counting all the ticks: so the
	 * uptime is right for it, and the clock task runs when a 32-tick
	 * boundary passed (the watchdog). */
	lost_ticks += n - 1;
	SCB_ICSR = ICSR_PENDSTSET;
  }
  SYST_RVR = cyc - 1;
  SYST_CVR = 0;
  SYST_CSR = SYST_CSR_CLKSOURCE | SYST_CSR_TICKINT | SYST_CSR_ENABLE;
  while (SYST_CVR == 0) ;		/* loaded (a cycle or two) */
  SYST_RVR = cpu_hz / HZ - 1;		/* from the next tick on */
  return(1);
}


/*===========================================================================*
 *				idle_task				     *
 *===========================================================================*/
PUBLIC void idle_task()
{
/* Nothing to run.  But a shadowed process may be ready, waiting for the
 * clock task to swap it in: PendSV wakes the clock task now rather than
 * at the next tick (a fork()ed pair taking turns: 16 ms per switch).
 * Else sleep: Stop2 when it may (pm_sleep), wfi as before when not. */
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
	if (pm_sleep()) {
		__asm volatile ("cpsie i; isb" : : : "memory");
		continue;
	}
	__asm volatile ("wfi; cpsie i; isb" : : : "memory");
  }
}


/*===========================================================================*
 *				Faults					     *
 *===========================================================================*/
PRIVATE void fault_regs(cfsr, hfsr, far)
u32_t cfsr, hfsr, far;		/* far: read before CFSR was cleared */
{
  printf("CFSR=%08lx HFSR=%08lx fault address=%08lx\n",
	(unsigned long) cfsr, (unsigned long) hfsr, (unsigned long) far);
}

PUBLIC void arm_exception(exc)
unsigned exc;
{
/* A process caused a fault.  Users get a signal, like exception.c does on
 * the PC; a task or server faulting is fatal.
 */
  struct proc *rp = proc_ptr;
  u32_t cfsr = SCB_CFSR, hfsr = SCB_HFSR;
  /* The fault address first: once MMARVALID/BFARVALID are cleared the
   * register is UNKNOWN (the Flipper then reads back 0xE000ED34, MMFAR's
   * own address: the "addr=e000ed34" of #108 was this, not the fault). */
  u32_t far = (cfsr & 0x80) ? SCB_MMFAR : (cfsr & 0x8000) ? SCB_BFAR : 0;
  int sig;

  SCB_CFSR = cfsr;		/* write 1 to clear */
  SCB_HFSR = hfsr;

  if (cfsr & 0x02000000) sig = SIGFPE;		/* DIVBYZERO */
  else if (cfsr & 0x000F0000) sig = SIGILL;	/* UNDEFINSTR..NOCP */
  else if (cfsr & 0x01000000) sig = SIGBUS;	/* UNALIGNED */
  else sig = SIGSEGV;

  if (exc == EXC_DEBUGMON) {		/* R4a.6: its guard word written */
	printf("\nstack overflow in %s (guard %08lx written), pc=%08lx "
		"lr=%08lx sp=%08lx\n", rp->p_name,
		(unsigned long) DWT_COMP(1), (unsigned long) rp->p_reg.pc,
		(unsigned long) rp->p_reg.lr, (unsigned long) rp->p_reg.sp);
	panic("task stack overflow", proc_number(rp));
  }
  if (!isuserp(rp) || k_reenter != 0) {
	printf("\n%s (exception %u) in %s, pc=%08lx lr=%08lx sp=%08lx\n",
		sig == SIGSEGV ? "memory fault" : "fault", exc, rp->p_name,
		(unsigned long) rp->p_reg.pc, (unsigned long) rp->p_reg.lr,
		(unsigned long) rp->p_reg.sp);
	fault_regs(cfsr, hfsr, far);
	panic("exception in system code", proc_number(rp));
  }
  /* Report it (the PC kernel is silent here, but a one-line note costs
   * nothing and saves a lot of guessing on a machine without an MMU). */
  printf("%s[%d]: signal %d, pc=%08lx lr=%08lx sp=%08lx cfsr=%08lx addr=%08lx\n",
	rp->p_name, proc_number(rp), sig, (unsigned long) rp->p_reg.pc,
	(unsigned long) rp->p_reg.lr, (unsigned long) rp->p_reg.sp,
	(unsigned long) cfsr,
	(unsigned long) far);
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
  if (exc == EXC_DEBUGMON)
	printf("\nKernel stack overflow (its guard word %08lx written)\n",
					(unsigned long) DWT_COMP(0));
  printf("\nKernel fault, exception %u: pc=%08lx lr=%08lx psr=%08lx\n",
	exc, (unsigned long) frame[6], (unsigned long) frame[5],
	(unsigned long) frame[7]);
  printf("r0=%08lx r1=%08lx r2=%08lx r3=%08lx r12=%08lx\n",
	(unsigned long) frame[0], (unsigned long) frame[1],
	(unsigned long) frame[2], (unsigned long) frame[3],
	(unsigned long) frame[4]);
  fault_regs(SCB_CFSR, SCB_HFSR, (SCB_CFSR & 0x80) ? SCB_MMFAR
				: (SCB_CFSR & 0x8000) ? SCB_BFAR : 0);
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
  if (how != RBT_PANIC && how != RBT_MONITOR)
	hal_dev_mode(0);	/* an orderly end: a user's board again */
  if (how == RBT_PANIC) printf("System halted after panic.\n");
  else if (how == RBT_HALT) {
	/* T14c: power off (the charger's ship mode); hold Back, or plug in
	 * USB, to start again.  On USB the power stays until the cable goes. */
	printf("Power off.\n");
	hal_power_off();
  }
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
