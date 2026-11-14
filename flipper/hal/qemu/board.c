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
	if (on) {
		UART_CTRL |= CTRL_RXINT;
	} else {
		UART_CTRL &= ~CTRL_RXINT;
		UART_INT = INT_RX;	/* the CMSDK UART keeps a raised
					 * interrupt until it is cleared */
	}
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

/* ARM semihosting call (requires -semihosting-config enable=on). */
static uint32_t semihost(uint32_t op, void *arg)
{
	register uint32_t r0 __asm("r0") = op;
	register uint32_t r1 __asm("r1") = (uint32_t) arg;

	__asm volatile ("bkpt 0xAB" : "+r" (r0) : "r" (r1) : "memory");
	return r0;
}

#define SYS_OPEN	0x01
#define SYS_WRITE	0x05
#define SYS_READ	0x06
#define SYS_SEEK	0x0A
#define SYS_FLEN	0x0C
#define SYS_EXIT_EXTENDED 0x20
#define OPEN_RW_BINARY	3		/* fopen mode "r+b" (7 would be "w+b": truncates!) */

/* SYS_EXIT_EXTENDED: QEMU exits with the given status. */
void hal_halt(int status)
{
	uint32_t block[2] = { 0x20026, (uint32_t) status };	/* ADP_Stopped_ApplicationExit */

	semihost(SYS_EXIT_EXTENDED, block);
	for (;;) __asm volatile ("wfi");
}

/* No watchdog, speaker or DFU in QEMU: "reboot -d" just stops it. */
void hal_wdt_start(void) { }
void hal_wdt_kick(void) { }
void hal_tone(unsigned hz) { (void) hz; }
void hal_vibro(int on) { (void) on; }
/* GPIO: remembered levels only, so programs can be tried in QEMU. */
static unsigned char gpio_level[HAL_GPIO_PINS];
int hal_gpio(unsigned pin, unsigned op)
{
	if (pin >= HAL_GPIO_PINS || op > HAL_GPIO_OFF) return -1;
	if (op <= HAL_GPIO_HIGH) gpio_level[pin] = op;
	return 0;
}
int hal_gpio_read(unsigned pin)
{
	return pin < HAL_GPIO_PINS ? gpio_level[pin] : -1;
}
/* QEMU has no RNG: a xorshift seeded from the tick count.  Not random. */
int hal_random(uint32_t *w)
{
	static uint32_t x;

	if (x == 0) x = hal_ms * 2654435761u | 1;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	*w = x;
	return 0;
}
void hal_dfu_request(void) { hal_halt(0); }

/* The "SD card" is a disk image file on the host, see QEMU_DISK. */
#ifndef QEMU_DISK
#define QEMU_DISK "disk.img"
#endif
static int disk_fd = -1;
static uint32_t disk_sectors;

int hal_disk_init(void)
{
	static const char name[] = QEMU_DISK;
	uint32_t open_args[3] = { (uint32_t) name, OPEN_RW_BINARY,
				  sizeof name - 1 };
	uint32_t fd, len;

	if (disk_fd >= 0) return 0;
	fd = semihost(SYS_OPEN, open_args);
	if (fd == (uint32_t) -1) return -1;
	len = semihost(SYS_FLEN, &fd);
	if (len == (uint32_t) -1) return -1;
	disk_fd = (int) fd;
	disk_sectors = len / HAL_SECTOR_SIZE;
	return 0;
}

uint32_t hal_disk_sectors(void)
{
	return disk_sectors;
}

static int disk_seek(uint32_t sector, unsigned count)
{
	uint32_t args[2];

	if (disk_fd < 0 || sector >= disk_sectors
				|| count > disk_sectors - sector) return -1;
	args[0] = disk_fd;
	args[1] = sector * HAL_SECTOR_SIZE;	/* image must stay < 4 GB */
	return semihost(SYS_SEEK, args) == 0 ? 0 : -1;
}

int hal_disk_read(uint32_t sector, void *buf, unsigned count)
{
	uint32_t args[3] = { disk_fd, (uint32_t) buf, count * HAL_SECTOR_SIZE };

	if (disk_seek(sector, count) < 0) return -1;
	return semihost(SYS_READ, args) == 0 ? 0 : -1;	/* 0 = all read */
}

int hal_disk_write(uint32_t sector, const void *buf, unsigned count)
{
	uint32_t args[3] = { disk_fd, (uint32_t) buf, count * HAL_SECTOR_SIZE };

	if (disk_seek(sector, count) < 0) return -1;
	return semihost(SYS_WRITE, args) == 0 ? 0 : -1;
}

/* No USB in QEMU: the console stays on the UART. */
int hal_usb_init(void) { return -1; }
int hal_usb_irq(void) { return -1; }
void hal_usb_poll(void) { }
int hal_usb_connected(void) { return 0; }
unsigned hal_usb_write(const void *buf, unsigned n) { (void) buf; return n; }
int hal_usb_getc(void) { return -1; }

void hal_backlight(unsigned level) { (void) level; }
void hal_led(unsigned r, unsigned g, unsigned b) { (void) r; (void) g; (void) b; }


/* Flash text cache.  QEMU's "flash" (SSRAM1 at 0) is RAM: copy. */
uint32_t hal_flash_sr;
uint32_t hal_disk_errors;
unsigned hal_disk_mhz;
uint32_t hal_flash_dbg[5];

int hal_cpu2_probe(uint32_t *info)	/* no CPU2 */
{
	(void) info;
	return -1;
}

int hal_cpu2_info(uint32_t *info)
{
	(void) info;
	return -1;
}

void hal_flash_layout(uint32_t *l)	/* no CPU2: a made-up SFSA */
{
	l[HAL_FL_SFSA] = 0x00090000;
	l[HAL_FL_INST] = 0x00030000;		/* after 192 KB of system */
	l[HAL_FL_INST_SIZE] = 0x00030000;
	l[HAL_FL_CACHE] = 0x00060000;
	l[HAL_FL_CACHE_SIZE] = 0x00020000;
}

void hal_flash_region(uint32_t *base, uint32_t *size)
{
	uint32_t l[HAL_FL_N];

	hal_flash_layout(l);
	*base = l[HAL_FL_CACHE];
	*size = l[HAL_FL_CACHE_SIZE];
}

int hal_flash_erase(uint32_t page_addr)
{
	uint32_t *p = (uint32_t *) page_addr;
	unsigned i;

	for (i = 0; i < HAL_FLASH_PAGE / 4; i++) p[i] = 0xFFFFFFFFu;
	return 0;
}

int hal_flash_write(uint32_t dst, const void *src, unsigned len)
{
	const uint8_t *s = src;
	uint8_t *d = (uint8_t *) dst;

	while (len-- > 0) *d++ = *s++;
	return 0;
}
