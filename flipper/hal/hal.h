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
int hal_lcd_flush(const uint8_t *fb);	/* LCD_W * LCD_H / 8 bytes; -1: the SD
					 * card has SPI2 now, draw again later */
void hal_backlight(unsigned level);	/* 0 (off) .. 255; on after lcd_init */
void hal_led(unsigned r, unsigned g, unsigned b);	/* status LED 0..255 */

/* Buttons: bitmask of currently pressed keys. */
#define BTN_UP    0x01
#define BTN_DOWN  0x02
#define BTN_LEFT  0x04
#define BTN_RIGHT 0x08
#define BTN_OK    0x10
#define BTN_BACK  0x20
unsigned hal_buttons(void);

/* Vibration motor; GPIO header pins (the eight free ones, numbered 0..7:
 * PA7 PA6 PA4 PB3 PB2 PC3 PC1 PC0 = header pins 2 3 4 5 6 7 15 16). */
void hal_vibro(int on);
#define HAL_GPIO_PINS	8
enum { HAL_GPIO_LOW, HAL_GPIO_HIGH, HAL_GPIO_IN, HAL_GPIO_IN_UP,
       HAL_GPIO_IN_DOWN, HAL_GPIO_OFF };	/* OFF: analog, high impedance */
int hal_gpio(unsigned pin, unsigned op);	/* 0, or -1: bad pin/op */
int hal_gpio_read(unsigned pin);		/* level 0/1, or -1 */

/* Infrared.  Receive: a demodulating receiver on an EXTI line; the kernel
 * takes the interrupt, hal_ir_rx_edge() acknowledges it and returns the
 * new level (0 = carrier seen, "mark").  Transmit: hal_ir_carrier()
 * switches the carrier (hal_ir_freq: frequency, duty in %); a timer
 * interrupt (hal_ir_tx_irq) at the end of every mark and space, started by
 * hal_ir_tx_start(first, next) and fed by hal_ir_tx_next(the one after),
 * all in microseconds (spec R6.6). */
int hal_ir_irq(void);			/* NVIC line, or -1: no receiver */
void hal_ir_rx_on(int on);
int hal_ir_rx_edge(void);
void hal_ir_carrier(int on);
void hal_ir_freq(unsigned hz, unsigned duty);
int hal_ir_tx_irq(void);		/* NVIC line, or -1: no transmitter */
void hal_ir_tx_start(uint32_t first_us, uint32_t next_us);
int hal_ir_tx_ack(void);		/* the timer's interrupt: 1 if real */
void hal_ir_tx_next(uint32_t us);
void hal_ir_tx_stop(void);
uint32_t hal_cycles(void);

/* USB CDC-ACM serial port (Flipper; QEMU has none: init returns -1).
 * hal_usb_poll() runs the USB stack and belongs in the USB interrupt. */
int hal_usb_init(void);
int hal_usb_irq(void);
void hal_usb_poll(void);
int hal_usb_connected(void);		/* host has the port open (DTR) */
unsigned hal_usb_write(const void *buf, unsigned n);	/* bytes queued */
unsigned hal_usb_room(void);		/* ring room; nobody listening: all */
int hal_usb_getc(void);			/* -1 if nothing received */
int hal_usb_peekc(void);		/* the same byte, left in place */

/* Block storage: the microSD card (SPI mode) on the Flipper, a disk image
 * file on the host (semihosting) in QEMU.  512-byte sectors, polled.
 * All return 0 on success, -1 on error. */
#define HAL_SECTOR_SIZE 512
int hal_disk_init(void);		/* (re)detect and initialise the card */
uint32_t hal_disk_sectors(void);	/* capacity, valid after init */
int hal_disk_read_part(uint32_t sector, unsigned off, unsigned len, void *buf);
/* Provided by the kernel's disk task: let the other tasks run while the
 * card is busy (it is deselected meanwhile). */
void hal_disk_yield(void);
/* SD data by DMA (T4b): the kernel's disk task registers the interrupt
 * hal_disk_dma_irq() (-1: none), calls hal_disk_dma_ack() in it and wakes
 * itself; hal_disk_wait() (the kernel's) sleeps until then. */
int hal_disk_dma_irq(void);
void hal_disk_dma_ack(void);
void hal_disk_wait(void);
int hal_disk_read(uint32_t sector, void *buf, unsigned count);
int hal_disk_write(uint32_t sector, const void *buf, unsigned count);

/* Internal flash for the program text cache (mm/fcache.c): a region of
 * whole pages that only the cache uses.  The kernel calls these with
 * interrupts masked and the MPU off.  len and dst must be multiples of 8
 * (double-word programming).  Return 0 on success, -1 on error. */
#define HAL_FLASH_PAGE	4096
int hal_random(uint32_t *w);		/* hardware RNG: 0, or -1 on error */
extern uint32_t hal_disk_errors;	/* transfers retried (CRC, timeout) */
extern unsigned hal_disk_mhz;		/* bus clock for disk data */
void hal_flash_region(uint32_t *base, uint32_t *size);	/* the text cache */
/* The whole layout (Phase 2b): l[HAL_FL_N] */
enum { HAL_FL_SFSA, HAL_FL_INST, HAL_FL_INST_SIZE, HAL_FL_CACHE,
       HAL_FL_CACHE_SIZE, HAL_FL_ROM, HAL_FL_ROM_SIZE, HAL_FL_N };
/* The default settings image (/dev/rom, spec L.2): at a fixed place, right
 * after the installed region's two directory pages, so that make
 * base-flash can put it there; installs never use those pages. */
#define HAL_ROM_OFF	(2 * 4096u)
#define HAL_ROM_SIZE	(32 * 1024u)
uint32_t hal_rom_base(void);	/* the same, without the layout table */
void hal_flash_layout(uint32_t *l);
/* /dev/mem and /dev/kmem: how many of the len bytes from addr can be read
 * (write: written) without a bus fault; 0: none (outside every range). */
uint32_t hal_mem_span(uint32_t addr, uint32_t len, int write);
/* Flash erase accounting (R2b.11), kept in RTC backup registers (no RAM):
 * hal_fc_erase_ok() is asked before every erase, cache = 1 for the
 * run-time cache (0: installed programs); it counts the erase and says 0
 * when today's cache budget is spent.  hal_fc_counts(): c[0] cache pages
 * erased in all, c[1] installed, c[2] cache today, c[3] the budget.
 * hal_fc_budget(): set the daily budget (0-255 pages).  upday: days of
 * uptime, the day when the RTC is not running. */
int hal_fc_erase_ok(int cache, unsigned upday);
void hal_fc_counts(uint32_t *c, unsigned upday);
void hal_fc_budget(unsigned pages, unsigned upday);
#define HAL_FC_BUDGET	32	/* the default: one cycle a day of a 128 KB cache */
int hal_flash_erase(uint32_t page_addr);
int hal_flash_write(uint32_t dst, const void *src, unsigned len);
extern uint32_t hal_flash_sr;	/* status of the last failure */

/* Stop the machine.  QEMU exits with the given status (semihosting); the
 * Flipper parks the CPU. */
/* Watchdog (2 s, kicked by the clock task), and DFU without buttons: the
 * word below survives the reset; hal_early_boot() looks at it.  After
 * "reboot -d" the board is in dev mode (a watchdog reset goes to DFU)
 * until a normal reboot or halt. */
#define HAL_BOOT_WORD	((volatile uint32_t *) 0x20000000)	/* not in RAM */
#define HAL_BOOT_DFU	0x21554644u				/* "DFU!" */
void hal_wdt_start(void);
void hal_wdt_kick(void);
void hal_dfu_request(void) __attribute__((noreturn));
void hal_dev_mode(int on);
int hal_dev(void);
const char *hal_reset_cause(void);	/* of the last reset */
/* The RTC's time, seconds since 1970 (UTC, spec I.1); 0: it is not running
 * or was never set.  Read once at boot (the clock task). */
uint32_t hal_rtc_time(void);
int hal_reset_bits(void);		/* RCC_CSR >> 24, 0x100: dev mode */
void hal_tone(unsigned hz);		/* speaker; 0 = off */
void hal_halt(int status) __attribute__((noreturn));

/* Low-level console output used by kprintf() and panics. */
void kputc(int c);
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* HAL_H */
