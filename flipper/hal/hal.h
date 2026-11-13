/* Hardware abstraction layer for the MINIX Flipper Zero port.
 *
 * Two implementations exist: hal/flipper (STM32WB55, the real device) and
 * hal/qemu (qemu-system-arm -M mps2-an386, a Cortex-M4 used for testing).
 * Everything above this interface is shared between them.
 */
#ifndef HAL_H
#define HAL_H

#if __has_include(<stdint.h>)
#include <stdint.h>
#else
/* MINIX code is built against MINIX's own headers, which predate C99. */
typedef __UINT8_TYPE__ uint8_t;
typedef __UINT16_TYPE__ uint16_t;
typedef __UINT32_TYPE__ uint32_t;
typedef __INT32_TYPE__ int32_t;
#endif

/* Board bring-up: clocks, SysTick (1 kHz), GPIO, debug UART, buttons.
 * The LCD is initialised separately by hal_lcd_init(). */
void hal_init(void);
const char *hal_board_name(void);
uint32_t hal_cpu_hz(void);

/* Milliseconds since hal_init(), advanced by the SysTick interrupt.  The
 * kernel's clock task takes SysTick over later and keeps this counting. */
extern volatile uint32_t hal_ms;
void hal_delay_ms(uint32_t ms);
/* Optional hook called from the SysTick interrupt after hal_ms is bumped. */
extern void (*hal_tick_hook)(void);

/* Polled debug serial port (USART1 on GPIO pins 13/14; CMSDK UART0 in QEMU). */
void hal_uart_putc(int c);
int hal_uart_getc(void);		/* -1 if no character is waiting */
int hal_uart_irq(void);			/* NVIC line of the debug UART */
void hal_uart_rx_irq(int on);		/* receive interrupt on/off */

/* 128x64 monochrome display.  The framebuffer is in ST7567 page order:
 * byte (page * 128 + x), bit n = pixel row page * 8 + n. */
#define LCD_W 128
#define LCD_H 64
void hal_lcd_init(void);
void hal_lcd_flush(const uint8_t *fb);	/* LCD_W * LCD_H / 8 bytes */

/* Buttons: bitmask of currently pressed keys. */
#define BTN_UP    0x01
#define BTN_DOWN  0x02
#define BTN_LEFT  0x04
#define BTN_RIGHT 0x08
#define BTN_OK    0x10
#define BTN_BACK  0x20
unsigned hal_buttons(void);

/* Stop the machine.  QEMU exits with the given status (semihosting); the
 * Flipper parks the CPU. */
void hal_halt(int status) __attribute__((noreturn));

/* Low-level console output used by kprintf() and panics. */
void kputc(int c);
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* HAL_H */
