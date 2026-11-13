/* Cortex-M4 vector table and reset handler, shared by both boards. */
#include <stdint.h>
#include "hal.h"

extern uint32_t _sidata[], _sdata[], _edata[], _sbss[], _ebss[], _estack[];
extern int main(void);

void Reset_Handler(void);
void Default_Handler(void);
void HardFault_Handler(void);

#define WEAK_ALIAS __attribute__((weak, alias("Default_Handler")))
void NMI_Handler(void) WEAK_ALIAS;
void MemManage_Handler(void) WEAK_ALIAS;
void BusFault_Handler(void) WEAK_ALIAS;
void UsageFault_Handler(void) WEAK_ALIAS;
void SVC_Handler(void) WEAK_ALIAS;
void DebugMon_Handler(void) WEAK_ALIAS;
void PendSV_Handler(void) WEAK_ALIAS;
void SysTick_Handler(void);	/* weak in hal/<board>/board.c; the kernel overrides it */
void IRQ_Handler(void) WEAK_ALIAS;	/* all device interrupts, for now */

#define NR_IRQ 64			/* STM32WB55 has 63, MPS2 fewer */

__attribute__((section(".isr_vector"), used))
void (*const vector_table[16 + NR_IRQ])(void) = {
	(void (*)(void)) _estack,
	Reset_Handler,
	NMI_Handler,
	HardFault_Handler,
	MemManage_Handler,
	BusFault_Handler,
	UsageFault_Handler,
	0, 0, 0, 0,
	SVC_Handler,
	DebugMon_Handler,
	0,
	PendSV_Handler,
	SysTick_Handler,
	[16 ... 16 + NR_IRQ - 1] = IRQ_Handler,
};

/* Board hook run before .data/.bss exist (must not touch globals). */
__attribute__((weak)) void hal_early_boot(void) {}

void Reset_Handler(void)
{
	uint32_t *src, *dst;

	hal_early_boot();
	for (src = _sidata, dst = _sdata; dst < _edata; )
		*dst++ = *src++;
	for (dst = _sbss; dst < _ebss; )
		*dst++ = 0;

	main();
	hal_halt(0);
}

void Default_Handler(void)
{
	uint32_t ipsr;

	__asm volatile ("mrs %0, ipsr" : "=r" (ipsr));
	kprintf("\n*** unexpected exception %lu\n", (unsigned long) ipsr);
	hal_halt(1);
}

/* Print the stacked exception frame, then stop. */
__attribute__((used)) static void hardfault_report(uint32_t *frame)
{
	volatile uint32_t *scb = (volatile uint32_t *) 0xE000ED00;

	kprintf("\n*** HardFault  CFSR=%08lx HFSR=%08lx MMFAR=%08lx BFAR=%08lx\n",
		(unsigned long) scb[0x28 / 4], (unsigned long) scb[0x2C / 4],
		(unsigned long) scb[0x34 / 4], (unsigned long) scb[0x38 / 4]);
	kprintf("    r0=%08lx r1=%08lx r2=%08lx r3=%08lx r12=%08lx\n",
		(unsigned long) frame[0], (unsigned long) frame[1],
		(unsigned long) frame[2], (unsigned long) frame[3],
		(unsigned long) frame[4]);
	kprintf("    lr=%08lx pc=%08lx psr=%08lx\n",
		(unsigned long) frame[5], (unsigned long) frame[6],
		(unsigned long) frame[7]);
	hal_halt(2);
}

__attribute__((naked, weak)) void HardFault_Handler(void)
{
	__asm volatile (
		"tst lr, #4\n"
		"ite eq\n"
		"mrseq r0, msp\n"
		"mrsne r0, psp\n"
		"b hardfault_report\n");
}
