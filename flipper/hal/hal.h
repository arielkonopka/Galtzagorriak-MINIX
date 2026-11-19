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
void hal_backlight_fade(unsigned from, unsigned to, unsigned ms);	/* down,
					 * by the LED driver on its own */
/* The backlight's settings, kept over resets (Flipper: RTC backup register
 * 15; no RAM): bit 27 off by the idle timeout, bits 24-26 the dimming step
 * (10 % less each second in the last 5 s before it), bits 16-23 the
 * brightness, bits 0-15 the timeout in seconds (0: never).  Off with all
 * three step bits set: the LCD in power save too (T14d).  hal_bl_state()
 * gives the defaults (255, 15 s) when nothing valid is stored. */
#define HAL_BL_OFF		0x08000000u
#define HAL_BL_STEP(s)		((s) >> 24 & 7)
#define HAL_BL_STEPS		0x07000000u
#define HAL_BL_LEVEL(s)		((s) >> 16 & 0xFF)
#define HAL_BL_TIMEOUT(s)	((s) & 0x3FFF)	/* <= 16383 s (R6.10) */
#define HAL_BL_MODE(s)		((s) >> 14 & 3)	/* R6.10: 0 auto, 1 forced off,
						 * 2 forced on; cleared at boot */
#define HAL_BL_MODES		0x0000C000u
uint32_t hal_bl_state(void);
void hal_bl_state_set(uint32_t s);
void hal_led(unsigned r, unsigned g, unsigned b);	/* status LED 0..255 */
/* R7b.5: a named pattern (blink, fast, double, pulse) run by the LED
 * driver on its own, in the colour rgb (0xRRGGBB); hal_led() stops it.
 * 0, or -1: no such pattern, or black. */
int hal_led_pattern(const char *name, unsigned long rgb);
void hal_led_pwm(unsigned r, unsigned g, unsigned b);	/* only the levels */
void hal_led_get(unsigned char rgb[3]);		/* as last set (I2C read) */
/* The LCD's power save (T14d): 1 = display off and all points on (the
 * ST7567 sleeps and keeps its RAM; flushes still go into it), 0 = back,
 * with no redraw.  -1: the SD card has the bus, try again later. */
int hal_lcd_save(int on);

/* Buttons: bitmask of currently pressed keys. */
#define BTN_UP    0x01
#define BTN_DOWN  0x02
#define BTN_LEFT  0x04
#define BTN_RIGHT 0x08
#define BTN_OK    0x10
#define BTN_BACK  0x20
unsigned hal_buttons(void);
int hal_buttons_fake(unsigned mask);	/* QEMU tests: these held; -1 else */

/* Sub-GHz receive only (R8.1 part 1): the CC1101 listens at hz (stock
 * bands 300-348, 387-464, 779-928 MHz); returns the frequency it really
 * tuned, or -1.  The RSSI in dBm (and the frequency), or HAL_SUBGHZ_OFF
 * when it is not receiving.  Off: back to sleep.  No transmit path. */
#define HAL_SUBGHZ_OFF	(-1000)
long hal_subghz_rx(uint32_t hz);
int hal_subghz_rssi(uint32_t *hz);
void hal_subghz_off(void);

/* Sub-GHz transmit (R8.2, issue #302), behind the kernel's band and
 * duty-cycle check (flipdev.c), at the frequency hal_subghz_rx tuned:
 * - hal_subghz_tx: a frame of n durations in us, carrier on first, then
 *   off, on, ... (the /dev/subghzraw format), in memory at src;
 * - hal_subghz_pkt_tx / _pkt_rx: one packet (at most HAL_SUBGHZ_PKT
 *   bytes) through the chip's FIFO, as stock's `subghz chat`; _pkt_rx
 *   gives a received one (its length) or 0 when none has come.
 * All return -1 when there is no such path: on the Flipper until the
 * CC1101 TX and packet paths are written (local agent; off by default,
 * the Autonomy rule).  QEMU's backend prints each frame or packet as
 * console lines ("sg> tx ...", "sg> pkt ..."); hal_subghz_line gives the
 * FNV-1a hash of what the last one printed, hal_subghz_fake (QEMU tests,
 * -1 on the Flipper) makes the next frame or packet a received one
 * (HAL_SG_FAKE_RAW: /dev/subghzraw's reader gets it; HAL_SG_FAKE_PKT:
 * hal_subghz_pkt_rx gives it), and hal_subghz_fake_take tells whether
 * one was armed (and disarms it). */
#define HAL_SUBGHZ_PKT	60
#define HAL_SG_FAKE_RAW	1
#define HAL_SG_FAKE_PKT	2
int hal_subghz_tx(uint32_t hz, uint32_t src, unsigned n);
int hal_subghz_pkt_tx(uint32_t hz, const uint8_t *p, unsigned n);
int hal_subghz_pkt_rx(uint8_t *p, unsigned max);
int hal_subghz_fake(int what);
int hal_subghz_fake_take(int what);
int hal_subghz_pkt_fake(const uint8_t *p, unsigned n);
int hal_subghz_line(uint32_t *hash);

/* iButton 1-Wire (R6.7): drive the line low (1) or let it go (0), read
 * it, wait some microseconds (busy: only for the slot timing). */
void hal_ow_line(int low);
int hal_ow_read(void);
void hal_ow_delay(unsigned us);
uint16_t hal_ow_now(void);		/* a us clock, mod 2^16 */
/* R6.7 part 3: Dallas key emulation (1-Wire slave).  hal_ib_emu(1) arms
 * an interrupt on both edges of the contact, in the buttons' vector (its
 * handler asks hal_ib_edge() first, then hal_btn_pending()).  hal_ib_clear
 * forgets the edges of a pulse we drove ourselves. */
void hal_ib_emu(int on);
int hal_ib_edge(void);
void hal_ib_clear(void);
int hal_btn_pending(void);

/* The fuel gauge (BQ27220, T15a): one 16-bit standard command (HAL_GAUGE_*,
 * little endian), read only; -1 on a bus error, -2 if there is none. */
#define HAL_GAUGE_TEMP		0x06	/* 0.1 K */
#define HAL_GAUGE_VOLT		0x08	/* mV */
#define HAL_GAUGE_STATUS	0x0A	/* BatteryStatus; bit 0 DSG */
#define HAL_GAUGE_CURRENT	0x0C	/* mA, signed, < 0 discharging */
#define HAL_GAUGE_AVG		0x14	/* AverageCurrent, mA */
#define HAL_GAUGE_STANDBY	0x1A	/* StandbyCurrent, mA */
#define HAL_GAUGE_SOC		0x2C	/* StateOfCharge, % */
int hal_gauge_read(unsigned cmd);
/* The charger (BQ25896, T14c): one register, read only here; -1 on a bus
 * error, -2 if there is none.  HAL_CHG_STATUS: bits 5-7 VBUS_STAT, 3-4
 * CHRG_STAT (0 not charging, 1 pre, 2 fast, 3 done), 2 power good. */
#define HAL_CHG_STATUS		0x0B
int hal_charger_read(unsigned reg);
/* R6.1: battery profile care (VREG 4.0 V, else 4.208), gentle charging
 * (ICHG 1 A, else the stock 2 A), 5 V OTG on the header.  Profile and
 * gentle are kept over resets.  0, or -1 on a bus error. */
#define HAL_CHG_CARE		1
#define HAL_CHG_GENTLE		2
#define HAL_CHG_OTG		3
#define HAL_CHG_PAUSE		4	/* charging paused (CHG_CONFIG off) */
int hal_charger_set(int what, int on);
/* The default speaker volume (0-100; 100 if never set), kept over resets
 * in a backup register (K.7 Sound). */
int hal_vol_default(void);
void hal_vol_default_set(int v);
/* The buttons' interrupts (EXTI, T14b): hal_btn_irq(0, 1, ...) are the
 * NVIC lines, -1 after the last (none: the kernel samples on its tick).
 * hal_btn_arm(1): an interrupt at the next edge of any button; (0): none. */
int hal_btn_irq(int i);
void hal_btn_arm(int on);

/* The ms timer (R4b.2): a 16-bit counter at HAL_MT_HZ that keeps running
 * in Stop2, and one compare.  hal_mt_set(due) takes effect a little later:
 * HAL_MT_READY in hal_mt_ack(), which also says HAL_MT_MATCH when the
 * counter passed the compare.  hal_mt_irq: -1 when there is none. */
#define HAL_MT_HZ	32768u
#define HAL_MT_MATCH	1
#define HAL_MT_READY	2
int hal_mt_irq(void);
unsigned hal_mt_now(void);
void hal_mt_set(unsigned due);
unsigned hal_mt_cmp(void);		/* the compare as last set */
unsigned hal_mt_ack(void);

/* Stop2 (T14b): call with interrupts masked; returns after the wake-up,
 * clocks as before, with what woke it; -1: no Stop2 here (QEMU). */
#define HAL_WAKE_BTN	1
#define HAL_WAKE_TIMER	2
int hal_stop2(void);

/* Flash ECC (R4a.5): flags since the last call (cleared), and where. */
#define HAL_ECC_FIXED	1		/* a single bit, corrected */
#define HAL_ECC_DOUBLE	2		/* two bits: the NMI */
unsigned hal_flash_ecc(uint32_t *addr);
int hal_can_stop2(void);		/* 0: never (QEMU, no HSE at boot) */

/* Vibration motor; GPIO header pins (the eight free ones, numbered 0..7:
 * PA7 PA6 PA4 PB3 PB2 PC3 PC1 PC0 = header pins 2 3 4 5 6 7 15 16). */
void hal_vibro(int on);
#define HAL_GPIO_PINS	8
enum { HAL_GPIO_LOW, HAL_GPIO_HIGH, HAL_GPIO_IN, HAL_GPIO_IN_UP,
       HAL_GPIO_IN_DOWN, HAL_GPIO_OFF };	/* OFF: analog, high impedance */
int hal_gpio(unsigned pin, unsigned op);	/* 0, or -1: bad pin/op */
int hal_gpio_read(unsigned pin);		/* level 0/1, or -1 */
/* R6.4: mV on each header pin left analog ("off") with an ADC input, -1
 * for the others, HAL_ADC_OVER above the reference; returns the reference
 * (VREF+, ~2500 mV) in mV, or -1 (no ADC). */
#define HAL_ADC_OVER	(-2)
int hal_adc_mv(int *mv);
/* R6.4 part 2: n 12-bit samples of header pin `pin` (as hal_gpio; left
 * analog) at `rate` Hz (100..100000) by DMA to the physical address dst
 * (even); -1 if not possible.  Left: samples still to come (0: done). */
int hal_adc_stream(unsigned pin, unsigned rate, uint32_t dst, unsigned n);
unsigned hal_adc_stream_left(void);
void hal_adc_stream_stop(void);

/* Infrared.  Receive: a demodulating receiver, timed by the pulse engine
 * (hal_pulse_rx below, input 0).  Transmit: hal_ir_carrier()
 * switches the carrier (hal_ir_freq: frequency, duty in %); a timer
 * interrupt (hal_ir_tx_irq) at the end of every mark and space, started by
 * hal_ir_tx_start(first, next) and fed by hal_ir_tx_next(the one after),
 * all in microseconds (spec R6.6). */
void hal_ir_carrier(int on);
void hal_ir_target(int rfid);		/* 0 IR LED, 1 the 125 kHz antenna */
void hal_ir_freq(unsigned hz, unsigned duty);
int hal_ir_tx_irq(void);		/* NVIC line, or -1: no transmitter */
void hal_ir_tx_start(uint32_t first_us, uint32_t next_us);
int hal_ir_tx_ack(void);		/* the timer's interrupt: 1 if real */
void hal_ir_tx_next(uint32_t us);
void hal_ir_tx_stop(void);
/* R4b.7, the pulse engine's capture: the time between edges of an input
 * (0: the IR receiver, 1: the CC1101's demodulated data), in us, as 32-bit words by DMA straight into dst
 * (n of them; the caller pins the memory).  The interrupt is the IR
 * transmitter's (hal_ir_tx_irq): hal_pulse_ack() tells HAL_PULSE_START
 * (a frame began: what follows is its), HAL_PULSE_GAP (gap_us without an
 * edge after it: the frame is over).  hal_pulse_left: words not yet
 * written. */
#define HAL_PULSE_START	1
#define HAL_PULSE_GAP	2
int hal_pulse_rx(unsigned input, uint32_t dst, unsigned n, unsigned gap_us);
int hal_pulse_ack(void);
unsigned hal_pulse_left(void);
void hal_pulse_stop(void);
/* R6.5: reading a 125 kHz tag: the field on, the antenna's comparator
 * captured on both edges, n 32-bit timestamps (us) by DMA into dst (the
 * caller pins it; hal_pulse_left tells the words not yet written);
 * hal_rfid_stop turns it all off.  -1: no reader. */
int hal_rfid_read(uint32_t dst, unsigned n, unsigned carrier);
/* Returns the comparator's level just before the capture started (0/1),
 * or -1.  carrier: R9.0.4, 0 125 kHz/50 %, 1 62.5 kHz/25 % (PSK), 2 134.2 kHz;
 * | HAL_RFID_STREAM (R9.0.5): the n words are a ring the DMA fills round
 * and round, with an interrupt (hal_rfid_stream_irq) at each half;
 * hal_rfid_stream_ack() in it: bit 0 the first half full, bit 1 the
 * second. */
#define HAL_RFID_STREAM	0x100
int hal_rfid_stream_irq(void);
unsigned hal_rfid_stream_ack(void);
void hal_rfid_stop(void);
/* R6.5 part 3: hold the 125 kHz field up across a T5577 command and its
 * reply (hal_rfid_field(1)), so a write leaves it on and hal_rfid_capture
 * arms the comparator without raising a field of its own; hal_rfid_field(0)
 * drops it.  hal_pulse_left and hal_rfid_stop count and end the capture. */
void hal_rfid_field(int on);
void hal_rfid_pull(int release);	/* PA2: 1 for a command, 0 to capture */
int hal_rfid_capture(uint32_t dst, unsigned n);
void hal_rfid_capture_stop(void);
/* R6.5 part 4 (R8.4): pretend to be a tag.  The reader's field clocks TIM2
 * (ETR on the carrier node) and the pull pin is PWM: dur[i] is a segment in
 * carrier periods - 1 (ARR) and pulse[i] how many of them the pull is on
 * (CCR3), both physical, n words each, played round and round by DMA with no
 * CPU work.  The caller pins the memory. */
int hal_rfid_emulate(uint32_t dur, uint32_t pulse, unsigned n);
/* R9.0.6: n | HAL_RFID_REFILL: the arrays are a ring the kernel refills;
 * an interrupt (hal_rfid_emu_irq) at each half played, and
 * hal_rfid_emu_ack() in it: bit 0 the first half done, bit 1 the second. */
#define HAL_RFID_REFILL	0x10000
int hal_rfid_emu_irq(void);
unsigned hal_rfid_emu_ack(void);
void hal_rfid_emulate_stop(void);
uint32_t hal_cycles(void);

/* USB CDC-ACM serial port (Flipper; QEMU has none: init returns -1).
 * hal_usb_poll() runs the USB stack and belongs in the USB interrupt. */
int hal_usb_init(void);
int hal_usb_irq(void);
void hal_usb_poll(void);
int hal_usb_connected(void);		/* host has the port open (DTR) */
int hal_usb_awake(void);		/* started, and the bus not suspended
					 * (a cable in): no Stop2 */
unsigned hal_usb_write(const void *buf, unsigned n);	/* bytes queued */
unsigned hal_usb_room(void);		/* ring room; nobody listening: all */
int hal_usb_getc(void);			/* -1 if nothing received */
int hal_usb_peekc(void);		/* the same byte, left in place */

/* USB HID (R10.2, R11.3.1), behind /dev/hidkbd, hidmouse, hidcons
 * (<minix/hid.h>, kernel/arm/hiddev.c).  hal_hid_state(): the bus bits
 * (HAL_HID_*, the same values as HID_*) | the host's LED byte << 16.
 * hal_hid_send(): one report of n bytes for kind (HAL_HID_KBD...): 0 when
 * it went out (or is queued), -1 when it cannot now.  The backend calls
 * the kernel's hid_event(HAL_HID_EV_*) from its interrupt when the state
 * or the LED byte changes, so that poll() and the LED reader wake.
 * QEMU's backend prints each report as one console line,
 * "hid< kbd 02 00 0b 00 00 00 00 00", and is always configured unless
 * hal_hid_fake() (its test hook; -1 on the Flipper) says otherwise;
 * hal_hid_line(): the FNV-1a hash of the last line (-1 on the Flipper). */
#define HAL_HID_ATTACHED	0x01
#define HAL_HID_CONFIGURED	0x02
#define HAL_HID_SUSPENDED	0x04
#define HAL_HID_KBD		0
#define HAL_HID_MOUSE		1
#define HAL_HID_CONS		2
#define HAL_HID_EV_STATE	1
#define HAL_HID_EV_LED		2
uint32_t hal_hid_state(void);
int hal_hid_send(unsigned kind, const uint8_t *rep, unsigned n);
int hal_hid_fake(uint32_t st);
int hal_hid_line(uint32_t *hash);
void hid_event(unsigned what);		/* the kernel's; interrupt level */

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
/* Spec N.6, card pulled out: the card-detect switch (polled; 1 = a card
 * is in), and an id of the card in the slot (a hash of its CID:
 * manufacturer, product, serial), read after hal_disk_init(): 0, or -1.
 * hal_disk_fake() is QEMU's test hook: 0 card in, 1 out, 2 another card
 * in, 3 returns the sectors written since boot (-1 on the Flipper). */
int hal_disk_present(void);
int hal_disk_id(uint32_t *id);
int hal_disk_fake(int state);
/* R12.40: DISCARD sectors first..last (the card marks them unused, no
 * erase): 0, -1 error, -2 the card has no DISCARD (SD Status, read at
 * init).  The card is busy meanwhile; the HAL waits by hal_disk_nap() (the
 * kernel's: the disk task sleeps one clock tick). */
int hal_disk_discard(uint32_t first, uint32_t last);
void hal_disk_nap(void);

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
/* T14a: the Sub-GHz and NFC chips asleep, at boot (hal_init).  The check
 * for the boot report: us the CC1101 took to wake (asleep: hundreds),
 * the ST25R3916's op-control (0: off) and id, SMPS step-down running. */
void hal_radios_park(void);
void hal_radios_check(unsigned *cc_us, unsigned *nfc_op, unsigned *nfc_id,
							int *smps);
/* The RTC (R6.2, R7b.1).  hal_rtc_ok: it runs and can be set (QEMU: 0, the
 * kernel keeps its own clock).  The time in seconds since 1970 (UTC, spec
 * I.1); 0: not running or never set.  hal_rtc_set: 2000-2099; -1: no. */
int hal_rtc_ok(void);
uint32_t hal_rtc_time(void);
int hal_rtc_set(uint32_t t);
/* Alarm A at due (UTC s, at most 27 days ahead; 0 clears it); at power-off
 * it is moved lead seconds earlier (the boot takes that long; 0 keeps the
 * stored lead, first 10 s).  -1: in the past, too far, or no RTC. */
int hal_rtc_alarm(uint32_t due, unsigned lead);
#define HAL_RTC_ARMED	1		/* it will fire */
#define HAL_RTC_FIRED	2		/* it fired since it was set */
#define HAL_RTC_WOKE	4		/* this boot came from it */
uint32_t hal_rtc_alarm_get(unsigned *lead, unsigned *flags);  /* due, 0 none */
int hal_rtc_alarm_irq(void);		/* -1: none */
int hal_rtc_alarm_ack(void);		/* in the handler: 1 if it fired */
/* Smooth calibration (RTC_CALR) in steps of 0.954 ppm, positive faster,
 * -511..512; set != 0 writes v.  Returns the value in force. */
int hal_rtc_cal(int set, int v);
int hal_reset_bits(void);		/* RCC_CSR >> 24, 0x100: dev mode */
/* Speaker: a square wave at hz (0 = off) at volume vol 0..100; the duty
 * is 50 % * (vol/100)^3, as stock furi_hal_speaker_start (100: full). */
void hal_tone(unsigned hz, unsigned vol);
void hal_halt(int status) __attribute__((noreturn));
/* Power off (T14c): the charger's ship mode, as stock furi_hal_power_off;
 * on VBUS it cannot cut the power and waits for the cable to go.  QEMU:
 * hal_halt(0). */
void hal_power_off(void) __attribute__((noreturn));

/* Low-level console output used by kprintf() and panics. */
void kputc(int c);
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* HAL_H */
