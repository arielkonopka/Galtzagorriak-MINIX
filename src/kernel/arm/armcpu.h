/* Cortex-M core registers and boot image layout for the ARM_CM port. */
#ifndef ARMCPU_H
#define ARMCPU_H

#define REG32(a)	(*(volatile u32_t *) (a))

/* SysTick */
#define SYST_CSR	REG32(0xE000E010)
#define SYST_RVR	REG32(0xE000E014)
#define SYST_CVR	REG32(0xE000E018)
#define SYST_CSR_ENABLE		0x1
#define SYST_CSR_TICKINT	0x2
#define SYST_CSR_CLKSOURCE	0x4

/* NVIC */
#define NVIC_ISER(n)	REG32(0xE000E100 + 4 * (n))
#define NVIC_ICER(n)	REG32(0xE000E180 + 4 * (n))
#define NVIC_ICPR(n)	REG32(0xE000E280 + 4 * (n))
#define NVIC_IPR8(n)	(*(volatile u8_t *) (0xE000E400 + (n)))

/* System control block */
#define SCB_ICSR	REG32(0xE000ED04)
#define ICSR_PENDSVSET	(1u << 28)
#define SCB_VTOR	REG32(0xE000ED08)
#define SCB_AIRCR	REG32(0xE000ED0C)
#define SCB_CCR		REG32(0xE000ED14)
#define SCB_SHPR(n)	(*(volatile u8_t *) (0xE000ED18 + (n) - 4))
#define SCB_SHCSR	REG32(0xE000ED24)
#define SCB_CFSR	REG32(0xE000ED28)
#define SCB_HFSR	REG32(0xE000ED2C)
#define SCB_MMFAR	REG32(0xE000ED34)
#define SCB_BFAR	REG32(0xE000ED38)

/* MPU (PMSAv7) */
#define MPU_CTRL	REG32(0xE000ED94)
#define MPU_RNR		REG32(0xE000ED98)
#define MPU_RBAR	REG32(0xE000ED9C)
#define MPU_RASR	REG32(0xE000EDA0)
#define MPU_CTRL_ENABLE		0x1
#define MPU_CTRL_PRIVDEFENA	0x4
#define MPU_RASR_ENABLE		0x1
#define MPU_RASR_AP_RO_RO	(6u << 24)	/* read-only, priv and user */
#define MPU_RASR_AP_RW_RW	(3u << 24)	/* read/write, priv and user */

#define AIRCR_VECTKEY	0x05FA0000
#define AIRCR_SYSRESETREQ 0x4
#define CCR_DIV_0_TRP	0x10
#define CCR_UNALIGN_TRP	0x08
#define CCR_STKALIGN	0x200
#define SHCSR_ENABLES	0x00070000	/* usage, bus, memmanage fault enable */
#define SHCSR_SVCALLPENDED 0x00008000	/* an SVC waits to be taken */

/* Exception numbers */
#define EXC_MEMMANAGE	4
#define EXC_BUSFAULT	5
#define EXC_USAGEFAULT	6
#define EXC_SVCALL	11
#define EXC_PENDSV	14
#define EXC_IRQ0	16

/* All exceptions that enter MINIX share this priority so they never nest:
 * SVC, PendSV, SysTick and every device IRQ.  Faults stay above them. */
#define KERNEL_PRIO	0x80

/* The clock is SysTick, not an NVIC line; it is given this pseudo-IRQ number
 * in irq_table[] so that put_irq_handler() works for it too. */
#define CLOCK_IRQ	(NR_IRQ_VECTORS - 1)

/* Server images (MM, FS, INIT) live in flash after the kernel.  Each one
 * starts with this header, written by flipper/tools/mkimage.py. */
#define BOOT_IMAGE_MAGIC	0x56534D58	/* "XMSV" */
#define NR_BOOT_SERVERS		3		/* MM, FS, INIT */

struct boot_image {
  u32_t magic;
  u32_t entry;			/* initial pc, Thumb bit set */
  u32_t text_start;		/* flash, click aligned */
  u32_t text_size;
  u32_t data_load;		/* flash copy of initialised data */
  u32_t data_start;		/* RAM, click aligned */
  u32_t data_size;		/* initialised data */
  u32_t bss_size;
  u32_t stack_size;		/* stack follows bss in the data segment */
  char name[8];
};

/* End of the usable part of SRAM2b (FS buffer cache, flipper/ld/server.ld). */
#define SRAM2_END	(0x20038000 + 10 * 1024)

/* Kernel memory layout, from the linker script. */
extern char _sidata[], _sdata[], _edata[], _sbss[], _ebss[];
extern char _stext[], _etext[];
extern char _boot_images[];	/* first server image header in flash */
extern char _ram_start[], _ram_end[];

extern u32_t cpu_hz;

#endif /* ARMCPU_H */
