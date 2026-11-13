/* QEMU "mps2-an386" board: Cortex-M4, CMSDK UART0, no LCD or buttons.
 *
 * The LCD is emulated by dumping the framebuffer as ASCII art to the UART
 * when hal_lcd_flush() is called, so display code can be tested headless.
 * There are no buttons; test code may set qemu_fake_buttons. */
#include <stdint.h>
#include "hal.h"

#define CPU_HZ		25000000u	/* MPS2 FPGA system clock */

#define UART0_BASE	0x40004000u
#define UART_DATA	(*(volatile uint32_t *) (UART0_BASE + 0x00))
#define UART_STATE	(*(volatile uint32_t *) (UART0_BASE + 0x04))
#define UART_CTRL	(*(volatile uint32_t *) (UART0_BASE + 0x08))
#define UART_INT	(*(volatile uint32_t *) (UART0_BASE + 0x0C))
#define UART_BAUDDIV	(*(volatile uint32_t *) (UART0_BASE + 0x10))
#define CTRL_RXINT	0x8
#define INT_RX		0x2
#define UART0_RX_IRQ	0		/* mps2-an385/386 interrupt map */
#define STATE_TXFULL	0x1
#define STATE_RXFULL	0x2

#define SYST_CSR	(*(volatile uint32_t *) 0xE000E010)
#define SYST_RVR	(*(volatile uint32_t *) 0xE000E014)
#define SYST_CVR	(*(volatile uint32_t *) 0xE000E018)

volatile uint32_t hal_ms;
void (*hal_tick_hook)(void);
unsigned qemu_fake_buttons;

const char *hal_board_name(void) { return "QEMU mps2-an386 (Cortex-M4)"; }
uint32_t hal_cpu_hz(void) { return CPU_HZ; }

__attribute__((weak)) void SysTick_Handler(void)
{
	hal_ms++;
	if (hal_tick_hook) hal_tick_hook();
}

void hal_init(void)
{
	UART_BAUDDIV = CPU_HZ / 115200;
	UART_CTRL = 0x3;		/* TX and RX enable */

	SYST_RVR = CPU_HZ / 1000 - 1;
	SYST_CVR = 0;
	SYST_CSR = 0x7;			/* CPU clock, interrupt, enable */
}

void hal_delay_ms(uint32_t ms)
{
	uint32_t start = hal_ms;

	while (hal_ms - start < ms)
		__asm volatile ("wfi");
}

void hal_uart_putc(int c)
{
	while (UART_STATE & STATE_TXFULL) ;
	UART_DATA = (uint8_t) c;
}

int hal_uart_getc(void)
{
	UART_INT = INT_RX;		/* acknowledge the receive interrupt */
	if (!(UART_STATE & STATE_RXFULL)) return -1;
	return UART_DATA & 0xFF;
}

int hal_uart_irq(void)
{
	return UART0_RX_IRQ;
}

void hal_uart_rx_irq(int on)
{
	if (on) UART_CTRL |= CTRL_RXINT;
	else UART_CTRL &= ~CTRL_RXINT;
}

void hal_lcd_init(void)
{
}

static int fb_get(const uint8_t *fb, int x, int y)
{
	return (fb[(y >> 3) * LCD_W + x] >> (y & 7)) & 1;
}

/* Render the 128x64 framebuffer at full resolution, 128x32 characters,
 * using UTF-8 half blocks (upper pixel, lower pixel per character cell). */
void hal_lcd_flush(const uint8_t *fb)
{
	static const char *const cell[4] = { " ", "▀", "▄", "█" };
	const char *s;
	int x, y;

	kprintf("\n+");
	for (x = 0; x < LCD_W; x++) kputc('-');
	kprintf("+\n");
	for (y = 0; y < LCD_H; y += 2) {
		kputc('|');
		for (x = 0; x < LCD_W; x++)
			for (s = cell[fb_get(fb, x, y) | fb_get(fb, x, y + 1) << 1];
			     *s != 0; s++)
				kputc(*s);
		kprintf("|\n");
	}
	kputc('+');
	for (x = 0; x < LCD_W; x++) kputc('-');
	kprintf("+\n");
}

unsigned hal_buttons(void)
{
	return qemu_fake_buttons;
}

/* ARM semihosting SYS_EXIT_EXTENDED: QEMU exits with the given status
 * (requires -semihosting-config enable=on). */
void hal_halt(int status)
{
	uint32_t block[2] = { 0x20026, (uint32_t) status };	/* ADP_Stopped_ApplicationExit */
	register uint32_t r0 __asm("r0") = 0x20;		/* SYS_EXIT_EXTENDED */
	register uint32_t r1 __asm("r1") = (uint32_t) block;

	__asm volatile ("bkpt 0xAB" : : "r" (r0), "r" (r1) : "memory");
	for (;;) __asm volatile ("wfi");
}
