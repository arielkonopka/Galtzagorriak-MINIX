/* QEMU "mps2-an386" board: Cortex-M4, CMSDK UART0, no LCD or buttons.
 *
 * The LCD is emulated by dumping the framebuffer as ASCII art to the UART
 * when hal_lcd_flush() is called, so display code can be tested headless.
 * There are no buttons; test code may set them (hal_buttons_fake). */
#include <stdint.h>
#include <string.h>
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
#define SYST_SHPR3	(*(volatile uint32_t *) 0xE000ED20)	/* SysTick: 31:24 */

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

static void mt_init(void);

void hal_init(void)
{
	UART_BAUDDIV = CPU_HZ / 115200;
	UART_CTRL = 0x3;		/* TX and RX enable */
	mt_init();

	/* Lowest priority first, like CMSIS SysTick_Config() on the Flipper:
	 * the kernel's cstart() runs this with only BASEPRI masking, and a
	 * tick at priority 0 before it stops SysTick again entered the kernel
	 * with proc_ptr == NULL (zeroed the vectors, lockup).  QEMU's SysTick
	 * runs on host time, so a busy host hit that window (powercut in CI). */
	SYST_SHPR3 = (SYST_SHPR3 & 0x00FFFFFFu) | 0xFF000000u;
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
int hal_lcd_save(int on) { (void) on; return 0; }

int hal_lcd_flush(const uint8_t *fb)
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
	return 0;
}

int hal_gauge_read(unsigned cmd) { (void) cmd; return -2; }
int hal_charger_read(unsigned reg) { (void) reg; return -2; }
int hal_charger_set(int what, int on) { (void) what; (void) on; return -1; }
static int vol_def = 100;			/* QEMU: no backup register */
int hal_vol_default(void) { return vol_def; }
void hal_vol_default_set(int v) { vol_def = v < 0 ? 0 : v > 100 ? 100 : v; }

void hal_ow_line(int low) { (void) low; }
int hal_ow_read(void) { return 1; }	/* nobody pulls it down */
void hal_ow_delay(unsigned us) { (void) us; }
uint16_t hal_ow_now(void) { return 0; }
void hal_ib_emu(int on) { (void) on; }		/* no reader in QEMU */
int hal_ib_edge(void) { return 0; }
void hal_ib_clear(void) { }
int hal_btn_pending(void) { return 1; }

/* Sub-GHz, the QEMU backend (issue #302): a radio that tunes the stock
 * bands, hears nothing by itself (RSSI -110 dBm), and prints what it
 * sends: a frame as console lines
 *	sg> tx 433920000 12
 *	sg> 381 -296 671 -350 ...		(16 values a line, RAW_Data's form)
 * and a packet as "sg> pkt 433920000 68 69".  sg_hash is the FNV-1a hash
 * of the frame's text as one line without the "sg> " prefixes and line
 * ends ("tx 433920000 12 381 -296 ..."), which m2test checks; a test
 * arms sg_fake so that the next frame or packet is a received one
 * instead (the pattern of hid< and HIDIOCFAKE).  RAM: 4 + 4 + 1 B, and
 * the fake packet of at most 8 bytes (QEMU only, not in the Flipper's
 * budget). */
static uint32_t sg_hz;
static uint32_t sg_hash;
static uint8_t sg_fake;
#define SG_FAKE_PKT_MAX	8		/* a test's packet: user RAM is
					 * short in QEMU's M2 run as well */
static uint8_t sg_pkt[SG_FAKE_PKT_MAX], sg_pkt_n;

long hal_subghz_rx(uint32_t hz)
{
	if (!((hz >= 299999755u && hz <= 348000335u)
	    || (hz >= 386999938u && hz <= 464000000u)
	    || (hz >= 778999847u && hz <= 928000000u))) return -1;
	sg_hz = hz;
	return (long) hz;
}

int hal_subghz_rssi(uint32_t *hz)
{
	*hz = sg_hz;
	return sg_hz != 0 ? -110 : HAL_SUBGHZ_OFF;
}

void hal_subghz_off(void) { sg_hz = 0; }

static void sg_kputs(const char *s)	/* not hashed */
{
	while (*s) kputc(*s++);
}

static void sg_put(int c)
{
	sg_hash = (sg_hash ^ (uint8_t) c) * 16777619u;	/* FNV-1a */
	kputc(c);
}

static void sg_puts(const char *s)
{
	while (*s) sg_put(*s++);
}

static void sg_num(long v)
{
	char b[12];
	int i = 0;
	unsigned long u;

	if (v < 0) sg_put('-');
	u = v < 0 ? (unsigned long) -v : (unsigned long) v;
	do b[i++] = (char) ('0' + u % 10); while ((u /= 10) != 0);
	while (i > 0) sg_put(b[--i]);
}

static void sg_head(const char *what, uint32_t hz)
{
	sg_kputs("sg> ");
	sg_hash = 2166136261u;
	sg_puts(what);
	sg_put(' ');
	sg_num((long) hz);
}

int hal_subghz_tx(uint32_t hz, uint32_t src, unsigned n)
{
	const uint32_t *d = (const uint32_t *) src;
	unsigned i;

	sg_head("tx", hz);
	sg_put(' ');
	sg_num((long) n);
	for (i = 0; i < n; i++) {
		if (i % 16 == 0) sg_kputs("\nsg>");
		sg_put(' ');
		sg_num(i & 1 ? -(long) d[i] : (long) d[i]);
	}
	kputc('\n');
	return 0;
}

int hal_subghz_pkt_tx(uint32_t hz, const uint8_t *p, unsigned n)
{
	static const char hex[] = "0123456789abcdef";

	sg_head("pkt", hz);
	while (n-- > 0) {
		sg_put(' ');
		sg_put(hex[*p >> 4]);
		sg_put(hex[*p++ & 15]);
	}
	kputc('\n');
	return 0;
}

int hal_subghz_pkt_rx(uint8_t *p, unsigned max)
{
	unsigned n = sg_pkt_n;

	if (n > max) n = max;
	for (sg_pkt_n = 0; sg_pkt_n < n; sg_pkt_n++) p[sg_pkt_n] = sg_pkt[sg_pkt_n];
	sg_pkt_n = 0;
	return (int) n;
}

int hal_subghz_fake(int what)
{
	sg_fake = (uint8_t) what;
	return 0;
}

int hal_subghz_fake_take(int what)
{
	if (sg_fake != what) return 0;
	sg_fake = 0;
	return 1;
}

int hal_subghz_pkt_fake(const uint8_t *p, unsigned n)
{
	if (n > SG_FAKE_PKT_MAX) return -1;
	for (sg_pkt_n = 0; sg_pkt_n < n; sg_pkt_n++) sg_pkt[sg_pkt_n] = p[sg_pkt_n];
	return 0;
}

int hal_subghz_line(uint32_t *hash)
{
	*hash = sg_hash;
	return 0;
}

/* With no button interrupt the kernel samples the buttons once a clock
 * tick (flipdev_tick, 1/HZ s).  A fake press (/dev/buttons write) and its
 * release can both come within one tick, the time of one short command
 * in the tests: then no sample saw the press and the key was lost (#260,
 * more often with a warm text cache).  So a press stays down until a
 * sample has seen it; the release shows at the next sample. */
static unsigned fake_unseen;		/* pressed, not sampled yet */

unsigned hal_buttons(void)		/* the clock interrupt: a sample */
{
	unsigned b = qemu_fake_buttons | fake_unseen;

	fake_unseen = 0;
	return b;
}

int hal_buttons_fake(unsigned mask)	/* the device task */
{
	uint32_t pm;

	__asm volatile ("mrs %0, primask\n\tcpsid i" : "=r" (pm) :: "memory");
	fake_unseen |= mask & ~qemu_fake_buttons;
	qemu_fake_buttons = mask;
	__asm volatile ("msr primask, %0" :: "r" (pm) : "memory");
	return 0;
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
#define SYS_TIME	0x11
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
void hal_tone(unsigned hz, unsigned vol) { (void) hz; (void) vol; }
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
void hal_power_off(void) { hal_halt(0); }
void hal_dev_mode(int on) { (void) on; }
int hal_dev(void) { return 0; }
int hal_reset_bits(void) { return 0x08; }	/* BORRSTF: power on */
const char *hal_reset_cause(void) { return "power on"; }

/* The "SD card" is a disk image file on the host, see QEMU_DISK. */
#ifndef QEMU_DISK
#define QEMU_DISK "disk.img"
#endif
static int disk_fd = -1;
static int disk_fake;		/* N.6 test hook: 0 in, 1 out, 2 another card */
static uint32_t disk_wsect;	/* sectors written (hook 3: ACK7.2, AC12.14) */
static uint32_t disk_sectors;

int hal_disk_init(void)
{
	static const char name[] = QEMU_DISK;
	uint32_t open_args[3] = { (uint32_t) name, OPEN_RW_BINARY,
				  sizeof name - 1 };
	uint32_t fd, len;

	if (disk_fake == 1) return -1;		/* N.6 test: no card */
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

/* Spec N.6 in QEMU: the image is "pulled out" or "another card" by the
 * test hook (kernel: DIOCFAKECARD on /dev/hd0); while it is out every
 * transfer fails, as on the Flipper. */
int hal_disk_present(void)
{
	return disk_fake != 1;
}

int hal_disk_id(uint32_t *id)
{
	*id = disk_fake == 2 ? 0x0DD5D00Du : 0x5D0C0A2Du;
	return disk_fake == 1 ? -1 : 0;
}

int hal_disk_fake(int state)
{
	if (state == 3) return (int) (disk_wsect & 0x7FFFFFFFu);	/* K.7 */
	if (state < 0 || state > 2) return -1;
	disk_fake = state;
	return 0;
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

int hal_disk_read_part(uint32_t sector, unsigned off, unsigned len, void *buf)
{
	uint8_t chunk[64], *out = buf;
	unsigned pos;

	/* the semihosted file: just seek inside the sector */
	for (pos = 0; pos < len; pos += sizeof(chunk)) {
		unsigned n = len - pos < sizeof(chunk) ? len - pos : sizeof(chunk);
		uint32_t args[3] = { disk_fd, (uint32_t) chunk, n };

		if (disk_seek(sector, 1) < 0) return -1;
		{
			uint32_t sk[2] = { disk_fd,
				sector * HAL_SECTOR_SIZE + off + pos };
			if (semihost(SYS_SEEK, sk) != 0) return -1;
		}
		if (semihost(SYS_READ, args) != 0) return -1;
		memcpy(out + pos, chunk, n);
	}
	return 0;
}

int hal_disk_read(uint32_t sector, void *buf, unsigned count)
{
	uint32_t args[3] = { disk_fd, (uint32_t) buf, count * HAL_SECTOR_SIZE };

	if (disk_fake == 1 || disk_seek(sector, count) < 0) return -1;
	return semihost(SYS_READ, args) == 0 ? 0 : -1;	/* 0 = all read */
}

int hal_disk_write(uint32_t sector, const void *buf, unsigned count)
{
	uint32_t args[3] = { disk_fd, (uint32_t) buf, count * HAL_SECTOR_SIZE };

	if (disk_fake == 1 || disk_seek(sector, count) < 0) return -1;
	disk_wsect += count;
	return semihost(SYS_WRITE, args) == 0 ? 0 : -1;
}

/* R12.40 in QEMU: the image has no DISCARD, so the range is filled with
 * zeros, which tests can see (a real card's discarded data is undefined).
 * One nap first, as a busy card would cost (the kernel's tick sleep). */
int hal_disk_discard(uint32_t first, uint32_t last)
{
	static const uint8_t zero[4096];
	uint32_t n;

	if (disk_fake == 1) return -1;
	if (last < first) return 0;		/* only asked */
	hal_disk_nap();
	while (first <= last) {
		n = last - first + 1;
		if (n > sizeof zero / HAL_SECTOR_SIZE)
			n = sizeof zero / HAL_SECTOR_SIZE;
		if (hal_disk_write(first, zero, n) != 0) return -1;
		first += n;
	}
	return 0;
}

/* No USB in QEMU: the console stays on the UART. */
int hal_usb_init(void) { return -1; }
int hal_usb_irq(void) { return -1; }
void hal_usb_poll(void) { }
int hal_usb_connected(void) { return 0; }
int hal_usb_awake(void) { return 0; }
unsigned hal_usb_write(const void *buf, unsigned n) { (void) buf; return n; }
unsigned hal_usb_room(void) { return 512; }
int hal_usb_getc(void) { return -1; }
int hal_usb_peekc(void) { return -1; }

/* USB HID, the QEMU backend (spec R11.3.1): no USB here, so each report
 * becomes one console line, "hid< kbd 02 00 0b 00 00 00 00 00", which
 * nattest and M2 check.  Configured from the start; hal_hid_fake() plays
 * the host (unplug, suspend, LEDs).  RAM: the state and the last line's
 * hash, 8 B (the spec allows 32). */
static uint32_t hid_st = HAL_HID_ATTACHED | HAL_HID_CONFIGURED;
static uint32_t hid_hash;

static void hid_put(int c)
{
	hid_hash = (hid_hash ^ (uint8_t) c) * 16777619u;	/* FNV-1a */
	kputc(c);
}

uint32_t hal_hid_state(void) { return hid_st; }

int hal_hid_send(unsigned kind, const uint8_t *rep, unsigned n)
{
	static const char names[3][6] = { "kbd", "mouse", "cons" };
	static const char hex[] = "0123456789abcdef";
	const char *s;

	if (kind > HAL_HID_CONS || (hid_st & (HAL_HID_CONFIGURED |
			HAL_HID_SUSPENDED)) != HAL_HID_CONFIGURED) return -1;
	hid_hash = 2166136261u;
	for (s = "hid< "; *s; s++) hid_put(*s);
	for (s = names[kind]; *s; s++) hid_put(*s);
	while (n-- > 0) {
		hid_put(' ');
		hid_put(hex[*rep >> 4]);
		hid_put(hex[*rep++ & 15]);
	}
	kputc('\n');
	return 0;
}

int hal_hid_fake(uint32_t st)
{
	hid_st = st & (0xFF0000u | HAL_HID_ATTACHED | HAL_HID_CONFIGURED
						| HAL_HID_SUSPENDED);
	if (!(hid_st & HAL_HID_ATTACHED)) hid_st &= 0xFF0000u;	/* unplugged */
	return 0;
}

int hal_hid_line(uint32_t *hash)
{
	*hash = hid_hash;
	return 0;
}

void hal_backlight(unsigned level) { (void) level; }
int hal_disk_dma_irq(void) { return -1; }	/* QEMU: semihosting, no DMA */
void hal_disk_dma_ack(void) { }
void hal_backlight_fade(unsigned from, unsigned to, unsigned ms) { (void) from; (void) to; (void) ms; }
static uint32_t bl_state = 255u << 16 | 15u;	/* QEMU: no backup register */
uint32_t hal_bl_state(void) { return bl_state; }
void hal_bl_state_set(uint32_t s) { bl_state = s & 0x0FFFFFFFu; }
void hal_led(unsigned r, unsigned g, unsigned b) { (void) r; (void) g; (void) b; }
void hal_led_pwm(unsigned r, unsigned g, unsigned b) { (void) r; (void) g; (void) b; }
int hal_led_pattern(const char *name, unsigned long rgb) { (void) name; return (rgb & 0xFFFFFF) ? 0 : -1; }
void hal_led_get(unsigned char rgb[3]) { rgb[0] = rgb[1] = rgb[2] = 0; }


/* Flash text cache.  QEMU's "flash" (SSRAM1 at 0) is RAM: copy. */
uint32_t hal_flash_sr;
uint32_t hal_disk_errors;
unsigned hal_disk_mhz;

void hal_flash_layout(uint32_t *l)	/* no CPU2: a made-up SFSA */
{
	l[HAL_FL_SFSA] = 0x000D0000;
	l[HAL_FL_INST] = 0x00048000;		/* after 192 KB of system and
						 * 96 KB of shared libc and
						 * libfuri (<minix/shlibc.h>) */
	/* 416 KB, near the Flipper's (its SFSA is higher still): a
	 * --flash package as big as subghz (130 KB of text) fits */
	l[HAL_FL_INST_SIZE] = 0x00068000;
	l[HAL_FL_CACHE] = 0x000B0000;
	l[HAL_FL_CACHE_SIZE] = 0x00020000;
	l[HAL_FL_ROM] = 0x00048000 + HAL_ROM_OFF;
	l[HAL_FL_ROM_SIZE] = HAL_ROM_SIZE;
}

uint32_t hal_rom_base(void) { return 0x00048000 + HAL_ROM_OFF; }

void hal_flash_region(uint32_t *base, uint32_t *size)
{
	uint32_t l[HAL_FL_N];

	hal_flash_layout(l);
	*base = l[HAL_FL_CACHE];
	*size = l[HAL_FL_CACHE_SIZE];
}

/* Like the STM32WB's flash, a double word may be programmed only when it
 * is erased (or with zeros: the "dead" marks); otherwise PROGERR.
 *
 * Power-cut test (AC2b.5, tools/powercut.py): the words at CUT_CFG are
 * loaded by QEMU (-device loader,data=...) and survive a system reset,
 * like the region itself.  CUT_CFG[0] = N: the Nth operation on the
 * installed-programs region since boot (1 = the first; an erase or one
 * double word) is cut: CUT_CFG[1] 0 = not done at all, 1 = half done (half
 * the page erased, or only the first word of the double word written);
 * then the machine resets.  CUT_CFG[2] = CUT_DONE afterwards, so the cut
 * happens once.  N = 0xFFFFFFFF: print every erase and directory write
 * with its number instead (to choose N). */
#define CUT_CFG		((volatile uint32_t *) 0x000CFFF0)	/* unused */
#define CUT_DONE	0x43555421u	/* "!TUC" */
static uint32_t cut_ops;

static void cut_say(const char *s, uint32_t a, uint32_t b)	/* small */
{
	int i;

	while (*s) hal_uart_putc(*s++);
	for (i = 28; i >= 0; i -= 4) hal_uart_putc("0123456789abcdef"[a >> i & 15]);
	hal_uart_putc(' ');
	for (i = 28; i >= 0; i -= 4) hal_uart_putc("0123456789abcdef"[b >> i & 15]);
	hal_uart_putc(']');
	hal_uart_putc('\n');
}

static int cut_here(uint32_t addr, int erase)	/* 1: cut now */
{
	uint32_t l[HAL_FL_N];

	hal_flash_layout(l);
	if (addr < l[HAL_FL_INST] || addr >= l[HAL_FL_INST] + l[HAL_FL_INST_SIZE]
			|| CUT_CFG[0] == 0 || CUT_CFG[2] == CUT_DONE)
		return 0;
	cut_ops++;
	if (CUT_CFG[0] == 0xFFFFFFFFu) {
		if (erase || addr < l[HAL_FL_INST] + 2 * HAL_FLASH_PAGE)
			cut_say(erase ? "[flashop erase " : "[flashop write ",
							cut_ops, addr);
		return 0;
	}
	return cut_ops == CUT_CFG[0];
}

static void cut_power(void)
{
	/* the MPU stays on during flash operations (R4a.3) and only the
	 * target is writable: off for this test hook's own word */
	*(volatile uint32_t *) 0xE000ED94 = 0;		/* MPU_CTRL */
	__asm volatile ("dsb; isb");
	CUT_CFG[2] = CUT_DONE;
	cut_say("\n[power cut at flash op ", cut_ops, 0);
	*(volatile uint32_t *) 0xE000ED0C = 0x05FA0004u;	/* SYSRESETREQ */
	for (;;) ;
}

int hal_flash_erase(uint32_t page_addr)
{
	uint32_t *p = (uint32_t *) page_addr;
	unsigned i, n = HAL_FLASH_PAGE / 4;

	if (cut_here(page_addr, 1)) {
		if (CUT_CFG[1]) for (i = 0; i < n / 2; i++) p[i] = 0xFFFFFFFFu;
		cut_power();
	}
	for (i = 0; i < n; i++) p[i] = 0xFFFFFFFFu;
	return 0;
}

int hal_flash_write(uint32_t dst, const void *src, unsigned len)
{
	const uint8_t *s = src;
	volatile uint32_t *d;
	uint32_t w0, w1;

	if (dst % 8 != 0 || len % 8 != 0) return -1;
	for (; len > 0; len -= 8, dst += 8, s += 8) {
		memcpy(&w0, s, 4);
		memcpy(&w1, s + 4, 4);
		if (w0 == 0xFFFFFFFFu && w1 == 0xFFFFFFFFu) continue;
		d = (volatile uint32_t *) dst;
		if ((d[0] != 0xFFFFFFFFu || d[1] != 0xFFFFFFFFu)
				&& (w0 != 0 || w1 != 0)) {
			cut_say("[flash: double word not erased ", dst, 0);
			hal_flash_sr = 0x8;			/* PROGERR */
			return -1;
		}
		if (cut_here(dst, 0)) {
			if (CUT_CFG[1]) d[0] = w0;
			cut_power();
		}
		d[0] = w0;
		d[1] = w1;
	}
	return 0;
}

/* Flash erase accounting (R2b.11): QEMU has no backup registers; these
 * words live as long as the machine (QEMU RAM, not the Flipper's). */
static uint32_t fc_total[2], fc_today, fc_budget = HAL_FC_BUDGET;

int hal_fc_erase_ok(int cache, unsigned upday)
{
	if (!cache) { fc_total[1]++; return 1; }
	if (fc_today >= fc_budget) return 0;
	fc_today++;
	fc_total[0]++;
	return 1;
}

void hal_fc_counts(uint32_t *c, unsigned upday)
{
	c[0] = fc_total[0]; c[1] = fc_total[1]; c[2] = fc_today; c[3] = fc_budget;
}

void hal_fc_budget(unsigned pages, unsigned upday) { fc_budget = pages & 0xFF; }

/* /dev/mem ranges: the 192 KB of RAM MINIX uses and SSRAM1 (its "flash")
 * up to the made-up SFSA. */
#define RAM_RANGES	{ 0x20000000u, 0x20030000u }
#define ROM_SPANS \
	sfsa = 0x000D0000; /* hal_flash_layout's made-up SFSA */ \
	if ((n = span_in(addr, len, 0, sfsa)) != 0) return n;

static uint32_t span_in(uint32_t addr, uint32_t len, uint32_t lo, uint32_t hi)
{
	if (addr < lo || addr >= hi) return 0;
	return len < hi - addr ? len : hi - addr;
}

uint32_t hal_mem_span(uint32_t addr, uint32_t len, int write)
{
	static const uint32_t ram[][2] = { RAM_RANGES };
	uint32_t sfsa, n;	/* no layout table: the MEMORY task's stack */
	unsigned i;

	for (i = 0; i < sizeof ram / sizeof ram[0]; i++)
		if ((n = span_in(addr, len, ram[i][0], ram[i][1])) != 0)
			return n;
	if (write) return 0;		/* flash and ROM only read */
	ROM_SPANS
	return 0;
}

/* QEMU: the host's clock, seconds since 1970 (semihosting). */
uint32_t hal_rtc_time(void) { return semihost(SYS_TIME, 0); }
int hal_rtc_ok(void) { return 0; }	/* the host's clock: not settable */
int hal_rtc_set(uint32_t t) { (void) t; return -1; }
int hal_rtc_alarm(uint32_t due, unsigned lead) { (void) due; (void) lead; return -1; }
uint32_t hal_rtc_alarm_get(unsigned *lead, unsigned *flags) { *lead = 0; *flags = 0; return 0; }
int hal_rtc_alarm_irq(void) { return -1; }
int hal_rtc_alarm_ack(void) { return 0; }
int hal_rtc_cal(int set, int v) { (void) set; (void) v; return 0; }

/* No radios in QEMU (T14a). */
void hal_radios_park(void) { }
void hal_radios_check(unsigned *cc_us, unsigned *nfc_op, unsigned *nfc_id,
							int *smps)
{
	*cc_us = *nfc_op = *nfc_id = 0;
	*smps = 0;
}

/* No infrared in QEMU. */
int hal_btn_irq(int i) { (void) i; return -1; }
void hal_btn_arm(int on) { (void) on; }
int hal_stop2(void) { return -1; }
unsigned hal_flash_ecc(uint32_t *addr) { *addr = 0; return 0; }
int hal_can_stop2(void) { return 0; }

/* The ms timer (R4b.2) on the board's CMSDK APB timers, so that it works
 * in QEMU as on the Flipper (LPTIM1): TIMER0 runs free with a period of
 * 2 s = 65536 clocks of 32768 Hz, giving the 16-bit count; TIMER1 counts
 * down to the compare (IRQ 9, one-shot).  LPTIM1's "compare written"
 * interrupt is pended by hand at once. */
#define TIMER0		0x40000000u
#define TIMER1		0x40001000u
#define T_REG(t, o)	(*(volatile uint32_t *) ((t) + (o)))
#define T_CTRL(t)	T_REG(t, 0x0)	/* 1 enable, 8 interrupt enable */
#define T_VALUE(t)	T_REG(t, 0x4)
#define T_RELOAD(t)	T_REG(t, 0x8)
#define T_INT(t)	T_REG(t, 0xC)	/* read: status; write 1: clear */
#define MT_PERIOD	(2u * CPU_HZ)	/* 2 s: 65536 clocks */
#define MT_IRQ		9
#define NVIC_ISPR0	(*(volatile uint32_t *) 0xE000E200u)
static unsigned mt_cmp, mt_ready;

static void mt_init(void)
{
	T_RELOAD(TIMER0) = MT_PERIOD - 1;
	T_VALUE(TIMER0) = MT_PERIOD - 1;
	T_CTRL(TIMER0) = 1;
	T_CTRL(TIMER1) = 0;
}

int hal_mt_irq(void) { return MT_IRQ; }

unsigned hal_mt_now(void)
{
	uint32_t c = MT_PERIOD - 1 - T_VALUE(TIMER0);	/* < 50 000 000 */

	return (c / 3125) * 4096 / 1000;	/* x 65536 / 50 000 000 */
}

void hal_mt_set(unsigned due)
{
	unsigned d = (due - hal_mt_now()) & 0xFFFF;

	mt_cmp = due & 0xFFFF;
	T_CTRL(TIMER1) = 0;
	T_RELOAD(TIMER1) = d * 763 + 1;	/* 762.9 cycles a clock: never early */
	T_VALUE(TIMER1) = d * 763 + 1;
	T_INT(TIMER1) = 1;
	T_CTRL(TIMER1) = 1 | 8;
	mt_ready = 1;
	NVIC_ISPR0 = 1u << MT_IRQ;
}

unsigned hal_mt_cmp(void) { return mt_cmp; }

unsigned hal_mt_ack(void)
{
	unsigned f = mt_ready ? HAL_MT_READY : 0;

	mt_ready = 0;
	if (T_INT(TIMER1) & 1) {
		T_INT(TIMER1) = 1;
		T_CTRL(TIMER1) = 0;		/* one-shot */
		f |= HAL_MT_MATCH;
	}
	return f;
}
int hal_adc_mv(int *mv) { (void) mv; return -1; }
int hal_adc_stream(unsigned pin, unsigned rate, uint32_t dst, unsigned n) { (void) pin; (void) rate; (void) dst; (void) n; return -1; }
unsigned hal_adc_stream_left(void) { return 0; }
void hal_adc_stream_stop(void) { }
void hal_ir_carrier(int on) { (void) on; }
void hal_ir_target(int rfid) { (void) rfid; }
void hal_ir_freq(unsigned hz, unsigned duty) { (void) hz; (void) duty; }
int hal_ir_tx_irq(void) { return -1; }
void hal_ir_tx_start(uint32_t a, uint32_t b) { (void) a; (void) b; }
int hal_ir_tx_ack(void) { return 0; }
void hal_ir_tx_next(uint32_t us) { (void) us; }
void hal_ir_tx_stop(void) { }
/* /dev/subghzraw's reader waits for a fake frame (hal_subghz_fake) */
int hal_pulse_rx(unsigned i, uint32_t d, unsigned n, unsigned g) { (void) d; (void) n; (void) g; return i == 1 && sg_hz != 0 ? 0 : -1; }
int hal_pulse_ack(void) { return 0; }
unsigned hal_pulse_left(void) { return 0; }
void hal_pulse_stop(void) { }
int hal_rfid_read(uint32_t d, unsigned n, unsigned c) { (void) d; (void) n; (void) c; return -1; }
int hal_rfid_stream_irq(void) { return -1; }
int hal_rfid_emu_irq(void) { return -1; }
unsigned hal_rfid_emu_ack(void) { return 0; }
unsigned hal_rfid_stream_ack(void) { return 0; }
void hal_rfid_stop(void) { }
void hal_rfid_field(int on) { (void) on; }
void hal_rfid_pull(int release) { (void) release; }
int hal_rfid_capture(uint32_t d, unsigned n) { (void) d; (void) n; return -1; }
void hal_rfid_capture_stop(void) { }
int hal_rfid_emulate(uint32_t d, uint32_t p, unsigned n)
	{ (void) d; (void) p; (void) n; return -1; }
void hal_rfid_emulate_stop(void) { }
uint32_t hal_cycles(void) { return hal_ms * 25000u; }	/* coarse */
