/* The Flipper device task: the LCD, buttons, backlight and status LED for
 * user programs.  It takes the place of the PRINTER task (the Flipper has
 * no printer), so FS reaches it as major device 6:
 *
 *	minor 0  /dev/lcd	write: a 1024-byte framebuffer (128x64, ST7567
 *				page layout, see hal.h); read: the current one
 *	minor 1  /dev/buttons	read: button events, 2 bytes each:
 *				  byte 0: BTN_* bit (hal.h)
 *				  byte 1: 1 = pressed, 0 = released
 *				blocks until there is one, unless O_NONBLOCK
 *	minor 2  /dev/backlight	write: one byte, brightness 0..255; three:
 *				and the idle timeout in seconds (LE, 0:
 *				never; default 15; 10 % dimmer each second of
 *				its last 5).  Off when idle, on at
 *				any key or button; kept over resets.
 *				read: the state, 4 bytes (hal.h HAL_BL_*)
 *	minor 3  /dev/led	write: three bytes, red green blue 0..255
 *	minor 4  /dev/vibro	write: one byte, 0 or '0' off, anything else on
 *	minor 5  /dev/speaker	write: notes, 4 bytes each: frequency in Hz and
 *				duration in ms, both 16-bit little endian;
 *				0 Hz is a rest.  Up to 64 notes per write;
 *				the write returns when they have played
 *	minor 6  /dev/gpio	the eight free header pins (hal.h), 0..7
 *				write: pairs of bytes, pin and operation:
 *				  0 low, 1 high (output), 2 input, 3 input
 *				  with pull-up, 4 with pull-down, 5 off
 *				read: one byte per pin, '0' or '1', from pin 0
 *	minor 7  /dev/ir	infrared, "raw" timings: 32-bit little endian
 *				durations in microseconds, mark first, then
 *				space, mark, ... (an odd count).
 *				read: one frame that starts while the read
 *				waits (it ends after 30 ms without carrier),
 *				up to the buffer; blocks
 *				write: up to 2048 durations (each <= 1 s),
 *				38 kHz with a third on, or a first word with
 *				bit 31 set: bits 0-19 the carrier in Hz
 *				(10000..60000), 20-26 the duty in % (0: 33);
 *				the writer waits until the frame is sent
 *
 * Infrared is interrupt driven (spec R6.6, rule 0.8): marks and spaces are
 * timed by a timer interrupt that switches the carrier, the receiver's
 * edges by its EXTI interrupt; this task only moves durations between the
 * caller and two small rings (96 bytes), and nobody busy-waits.
 *
 * Access is controlled with the permissions of the device files.  The
 * buttons are sampled, and notes timed, on every clock tick (flipdev_tick,
 * from clock.c).
 */
#include "../kernel.h"
#include <termios.h>
#include <minix/callnr.h>
#include <minix/com.h>
#include "../proc.h"
#include "hal.h"
#include <minix/lat.h>
#include <minix/fg.h>
#include <signal.h>

#define LCD_MINOR	0
#define BTN_MINOR	1
#define BL_MINOR	2
#define LED_MINOR	3
#define VIBRO_MINOR	4
#define SPK_MINOR	5
#define GPIO_MINOR	6
#define IR_MINOR	7
#define POWER_MINOR	8	/* the fuel gauge, read only (T15a) */
#define IBUTTON_MINOR	9	/* iButton 1-Wire: read the ROM (R6.7) */
#define ADC_MINOR	10	/* header pins' voltages, one-shot (R6.4) */
#define SUBGHZ_MINOR	11	/* the CC1101, receive only (R8.1 part 1) */
#define LAST_MINOR	11
#define NNOTES		64
#define FB_SIZE		(LCD_W * LCD_H / 8)
#define NEVENTS		32		/* button event ring (power of 2) */

#define O_NONBLOCK	04000		/* from <fcntl.h> */

PRIVATE u8_t fbuf[FB_SIZE];
PRIVATE message fm;

/* Button events, filled by flipdev_tick() at interrupt level. */
PRIVATE u8_t ev_key[NEVENTS], ev_down[NEVENTS];
PRIVATE volatile unsigned ev_head, ev_tail;
#define BL_DIMS		5	/* seconds of dimming, 10 % each */
#define BL_GONE		7	/* bl_want: off */
#define BL_SAVE		8	/* off, and the LCD in power save (T14d) */
#define LCD_SAVE_S	120	/* that long after the light went off */
PRIVATE unsigned last_buttons;	/* the buttons down, and these flags: */
#define SWALLOW		0x80000000u	/* a press woke the light */
#define BTN_EXTI	0x40000000u	/* edges interrupt; else the tick samples */
#define BTN_POLL	0x20000000u	/* one is down: the ms timer samples */
#define FG_LONG		0x02000000u	/* Back held long: SIGTERM due (task) */
#define FG_KILL		0x01000000u	/* SIGTERM sent: SIGKILL in FG_KILL_S */
#define FG_SLOT(b)	((int) ((b) >> 16 & 0xFF) - 1)	/* foreground proc */
#define FG_BITS		0x00FF0000u
#define FG_STAMP(b)	((b) >> 8 & 0xFF)	/* Back down (ticks), or the
						 * SIGTERM (seconds), mod 256 */
#define FG_STAMPS	0x0000FF00u
#define BTN_ALL		0x3Fu		/* the buttons themselves */
#define BTN_FLAGS	(~BTN_ALL)
#define FG_LONG_T	HZ		/* Back held this long (ticks) */
#define FG_KILL_S	3		/* then SIGKILL after (s) */
#define BTN_MS		8	/* sampling while down, and the debounce */

/* Notes being played (/dev/speaker), timed by flipdev_tick(); the same
 * memory holds the infrared rings while /dev/ir is in use (one or the
 * other: EBUSY), so they cost no RAM. */
#define IR_RXQ		32		/* receive ring (power of 2) */
#define IR_TXQ		64		/* transmit ring (power of 2: u8 indices) */
PRIVATE union {
  struct { u16_t hz[NNOTES], ticks[NNOTES]; } sp;
  struct { u16_t rxq[IR_RXQ], txq[IR_TXQ]; } ir;
} sbuf;
#define note_hz		sbuf.sp.hz
#define note_ticks	sbuf.sp.ticks
#define ir_rxq		sbuf.ir.rxq
#define ir_txq		sbuf.ir.txq
PRIVATE volatile int note_n, note_i;	/* count, current; note_i == note_n:
					 * done */
PRIVATE volatile unsigned note_left;	/* ticks left of the current one */
PRIVATE int sp_waiting, sp_caller, sp_proc, sp_count;

/* Infrared (/dev/ir).  Durations in the rings are 16 bits: microseconds
 * below 32768, else bit 15 and units of 64 us (up to 2 s). */
#define IR_TX_MAX	2048		/* durations per write */
#define IR_FREQ_FLAG	0x80000000UL	/* first word: carrier, not a time */
#define IR_HZ		38000
#define IR_DUTY		33
#define IR_GAP_MS	30		/* silence that ends a frame */
#define IR_START	0x0000		/* rx ring markers */
#define IR_END		0xFFFF
#define IR_ENC(us)	((us) < 0x8000 ? (u16_t) ((us) ? (us) : 1) \
				: (u16_t) (0x8000 | ((us) >> 6)))
/* IR_DEC uses v twice: never pass it an expression with side effects */
#define IR_DEC(v)	((v) & 0x8000 ? (u32_t) ((v) & 0x7FFF) << 6 : (u32_t) (v))

PRIVATE volatile u8_t rx_h, rx_t;	/* free-running ring indices */
PRIVATE volatile u8_t tx_h, tx_t;
PRIVATE volatile u8_t rx_active, rx_level = 1, rx_over;
PRIVATE volatile u32_t rx_last;		/* hal_cycles() of the last edge */
PRIVATE volatile u8_t ir_rd;		/* a reader waits */
PRIVATE u8_t ir_started;		/* its frame began */
PRIVATE volatile u8_t tx_on, tx_done, tx_err;
PRIVATE short ir_caller, ir_proc, tx_caller, tx_proc;
PRIVATE u16_t ir_count, ir_got, tx_count;
PRIVATE volatile u16_t tx_seg;		/* segments started (by the timer) */
PRIVATE u16_t tx_total, tx_fetch;	/* durations; next one to fetch */
PRIVATE vir_bytes ir_vir, tx_vir;	/* reader's buffer; first duration */
#define CYC_US		(hal_cpu_hz() / 1000000)	/* cycles per us */
#define RING_N(h, t)	((u8_t) ((h) - (t)))	/* entries in a ring */

/* A process blocked in a read of /dev/buttons. */
PRIVATE int rd_waiting;
PRIVATE int rd_caller, rd_proc, rd_count;
PRIVATE vir_bytes rd_vir;

FORWARD _PROTOTYPE( void reply, (int code, int replyee, int proc, int status) );
FORWARD _PROTOTYPE( int do_rw, (message *m_ptr)			);
FORWARD _PROTOTYPE( int deliver, (int proc, vir_bytes vir, int count)	);
FORWARD _PROTOTYPE( void do_hardint, (void)				);
FORWARD _PROTOTYPE( void lc_newline, (void)				);
FORWARD _PROTOTYPE( int ir_int, (int irq)				);
FORWARD _PROTOTYPE( int ir_tx_int, (int irq)				);
FORWARD _PROTOTYPE( int ir_write, (message *m_ptr, phys_bytes user, int count) );
FORWARD _PROTOTYPE( void bl_apply, (void) );
FORWARD _PROTOTYPE( int bl_want, (u32_t st, clock_t now) );
FORWARD _PROTOTYPE( unsigned btn_sample, (void) );
FORWARD _PROTOTYPE( int btn_int, (int irq) );
FORWARD _PROTOTYPE( int power_read, (phys_bytes user, int count) );
FORWARD _PROTOTYPE( int power_write, (phys_bytes user, int count) );
FORWARD _PROTOTYPE( int ow_read_rom, (phys_bytes user) );
FORWARD _PROTOTYPE( int adc_read, (phys_bytes user, int count) );
FORWARD _PROTOTYPE( int adc_put, (char *line, int n, int v) );
FORWARD _PROTOTYPE( int subghz_write, (phys_bytes user, int count) );
FORWARD _PROTOTYPE( int subghz_read, (phys_bytes user, int count) );
FORWARD _PROTOTYPE( int sg_put, (char *line, int n, int v) );
FORWARD _PROTOTYPE( int fg_other, (int proc) );
FORWARD _PROTOTYPE( int fg_ioctl, (message *m_ptr) );
FORWARD _PROTOTYPE( void fg_hold, (unsigned now) );
FORWARD _PROTOTYPE( int bl_state, (u32_t st) );
FORWARD _PROTOTYPE( void ir_fill, (void)				);
FORWARD _PROTOTYPE( void ir_drain, (void)				);
FORWARD _PROTOTYPE( void ir_stop_tx, (void)				);

/* The console on the LCD (launcher: "Console"): what goes out on the
 * console (cons.c) is also drawn into fbuf, which holds the screen anyway,
 * so it costs no buffer: 32 x 10 characters of the 4x6 font, scrolled by
 * shifting the pixel columns.  A one-byte write of 'C' to /dev/lcd turns
 * it on, any other write (an app drawing) off.  The task pushes the
 * screen out at most every LC_TICKS clock ticks. */
#define LC_COLS		(LCD_W / 4)
#define LC_ROWS		(LCD_H / 6)
#define LC_TICKS	(HZ / 15)
extern const unsigned char font_4x6[95][6];	/* hal/common/font4x6.c */
PRIVATE u8_t lc_on, lc_x, lc_y, lc_esc, lc_wait;
PRIVATE volatile u8_t lc_dirty;

/*===========================================================================*
 *				flipdev_task				     *
 *===========================================================================*/
PUBLIC void flipdev_task()
{
  int r;

  last_buttons = hal_buttons();
  for (r = 0; hal_btn_irq(r) >= 0; r++) {	/* T14b: one handler, 3 lines */
	put_irq_handler(hal_btn_irq(r), btn_int);
	enable_irq(hal_btn_irq(r));
  }
  if (r > 0) {			/* the ms timer samples until all are up */
	lock();
	last_buttons |= BTN_EXTI | BTN_POLL;
	mt_arm(MT_BUTTONS, BTN_MS);
	unlock();
  }
  {	/* the backlight as it was set before the reset, and on */
	u32_t st = hal_bl_state() & ~(HAL_BL_OFF | HAL_BL_STEPS);

	hal_bl_state_set(st);
	hal_backlight(HAL_BL_LEVEL(st));
  }
  if ((r = hal_ir_irq()) >= 0) {
	put_irq_handler(r, ir_int);
	hal_ir_rx_on(1);
	enable_irq(r);
  }
  if ((r = hal_ir_tx_irq()) >= 0) {
	hal_ir_tx_stop();			/* whatever ran before us */
	put_irq_handler(r, ir_tx_int);
	enable_irq(r);
  }

  while (TRUE) {
	receive(ANY, &fm);
	if (fm.m_source == HARDWARE) {
		do_hardint();
		continue;
	}
	switch (fm.m_type) {
	case DEV_OPEN:
	case DEV_CLOSE:
		r = (unsigned) fm.DEVICE <= LAST_MINOR ? OK : ENXIO;
#if LATTRACE
		if (fm.DEVICE == LAT_MINOR) r = OK;
#endif
		break;
	case DEV_READ:
	case DEV_WRITE:
		r = do_rw(&fm);
		break;
	case DEV_IOCTL:
		r = fg_ioctl(&fm);
		break;
	case CANCEL:
		if (rd_waiting && rd_proc == fm.PROC_NR) rd_waiting = FALSE;
		if (ir_rd && ir_proc == fm.PROC_NR) ir_rd = FALSE;
		if (tx_on && tx_proc == fm.PROC_NR) {	/* stop sending */
			ir_stop_tx();
			tx_on = FALSE;
		}
		if (sp_waiting && sp_proc == fm.PROC_NR) {	/* stop it */
			sp_waiting = FALSE;
			note_i = note_n;
			hal_tone(0);
		}
		r = EINTR;
		break;
	default:
		r = EINVAL;
	}
	reply(TASK_REPLY, fm.m_source, fm.PROC_NR, r);
  }
}


/*===========================================================================*
 *				Sub-GHz (R8.1 part 1)			     *
 *===========================================================================*/
/* /dev/subghz, receive only: write a frequency in Hz (the stock bands) to
 * listen there, "0" to stop; a read gives one line,
 *	rssi=-92 freq=433919830		or	off freq=433919830
 * A read also samples the RSSI as fast as it can for SG_PEAK_MS (between
 * samples the other tasks run; user processes wait, so it is short) and
 * reports the peak: a remote's bursts are a few ms long, one sample would
 * mostly miss them.
 *	rssi=-92 peak=-41 freq=433919830
 * It keeps listening between commands (the shell opens and closes the
 * device for each); "0" puts it to sleep, and so does a minute without a
 * read (subghz_minute: ~15 mA in RX).  No transmit path. */
PRIVATE u8_t sg_state;		/* SG_ON listening, SG_READ read lately */
#define SG_ON		1
#define SG_READ		2
#define SG_PEAK_MS	30

PUBLIC void subghz_minute()
{
/* The clock task, once a minute (pm_minute): a radio left listening with
 * nobody reading goes back to sleep. */
  if (sg_state == SG_ON) {
	hal_subghz_off();
	sg_state = 0;
  } else sg_state &= SG_ON;
}

PRIVATE int subghz_write(user, count)
phys_bytes user;
int count;
{
  char t[12];
  u32_t hz = 0;
  int i;

  if (count > (int) sizeof(t)) return(EINVAL);
  phys_copy(user, vir2phys(t), (phys_bytes) count);
  for (i = 0; i < count && t[i] >= '0' && t[i] <= '9'; i++)
	hz = hz * 10 + (t[i] - '0');
  if (i == 0 || i > 10) return(EINVAL);
  if (hz == 0) {
	hal_subghz_off();
	sg_state = 0;
	return(count);
  }
  if (hal_subghz_rx(hz) < 0) return(EINVAL);
  sg_state = SG_ON | SG_READ;
  return(count);
}

PRIVATE int sg_put(line, n, v)		/* a signed number */
char *line;
int n, v;
{
  if (v < 0) {
	line[n++] = '-';
	v = -v;
  }
  return(adc_put(line, n, v));
}

PRIVATE int subghz_read(user, count)
phys_bytes user;
int count;
{
  char line[40];
  const char *k;
  u32_t hz;
  int rssi = hal_subghz_rssi(&hz), n = 0, peak, r;
  u32_t t0 = hal_cycles();

  sg_state |= SG_READ;
  if (rssi == HAL_SUBGHZ_OFF) {
	for (k = "off"; *k != 0; k++) line[n++] = *k;
  } else {
	peak = rssi;
	while (hal_cycles() - t0 < SG_PEAK_MS * 1000u * CYC_US) {
		task_yield();		/* the others first: no busy wait */
		if ((r = hal_subghz_rssi(&hz)) > peak && r != HAL_SUBGHZ_OFF)
			peak = r;
	}
	for (k = "rssi="; *k != 0; k++) line[n++] = *k;
	n = sg_put(line, n, rssi);
	for (k = " peak="; *k != 0; k++) line[n++] = *k;
	n = sg_put(line, n, peak);
  }
  for (k = " freq="; *k != 0; k++) line[n++] = *k;
  n = adc_put(line, n, (int) hz);
  line[n++] = '\n';
  if (count > n) count = n;
  phys_copy(vir2phys(line), user, (phys_bytes) count);
  return(count);
}


/*===========================================================================*
 *				adc_read				     *
 *===========================================================================*/
/* /dev/adc (R6.4, part 1): one line with the header pins' voltages in mV,
 *	pa7=1650 pa6=- pa4=- pc3=- pc1=- pc0=> ref=2500
 * "-": the pin is not left analog ("gpio N off", the default), so it is
 * not measured; ">": above the reference (VREF+, 2.5 V from the internal
 * buffer: the range is 0-2.5 V).  PB3 and PB2 have no ADC input.  ~0.5 ms, on demand. */
PRIVATE const char adc_names[HAL_GPIO_PINS][4] =
	{ "pa7", "pa6", "pa4", "", "", "pc3", "pc1", "pc0" };

PRIVATE int adc_put(line, n, v)
char *line;
int n, v;
{
  char d[6];
  int j = 0;

  if (v == HAL_ADC_OVER) { line[n++] = '>'; return(n); }
  if (v < 0) { line[n++] = '-'; return(n); }
  do d[j++] = '0' + v % 10; while ((v /= 10) != 0);
  while (j > 0) line[n++] = d[--j];
  return(n);
}

PRIVATE int adc_read(user, count)
phys_bytes user;
int count;
{
  char line[80];
  int mv[HAL_GPIO_PINS], vdda, i, n = 0;
  const char *k;

  if ((vdda = hal_adc_mv(mv)) < 0) return(ENXIO);
  for (i = 0; i < HAL_GPIO_PINS; i++) {
	if (adc_names[i][0] == 0) continue;
	for (k = adc_names[i]; *k != 0; k++) line[n++] = *k;
	line[n++] = '=';
	n = adc_put(line, n, mv[i]);
	line[n++] = ' ';
  }
  for (k = "ref="; *k != 0; k++) line[n++] = *k;
  n = adc_put(line, n, vdda);
  line[n++] = '\n';
  if (count > n) count = n;
  phys_copy(vir2phys(line), user, (phys_bytes) count);
  return(count);
}


/*===========================================================================*
 *				iButton (R6.7)				     *
 *===========================================================================*/
/* 1-Wire master, standard speed.  Interrupts are masked (BASEPRI, lock)
 * only around the timed part of a slot, at most 70 us; between slots the
 * line idles high and anything may run (1-Wire has no upper bound there).
 * The 480 us reset pulse runs unmasked: longer is fine. */
PRIVATE int ow_reset()
{
  int present;

  hal_ow_line(1);
  hal_ow_delay(480);
  lock();
  hal_ow_line(0);
  hal_ow_delay(70);
  present = !hal_ow_read();
  unlock();
  hal_ow_delay(410);
  return(present);
}

PRIVATE int ow_bit(b)
int b;
{
  int r = 0;

  lock();
  hal_ow_line(1);
  hal_ow_delay(b ? 6 : 60);
  hal_ow_line(0);
  if (b) {				/* a 1, or a read slot */
	hal_ow_delay(9);
	r = hal_ow_read();
	hal_ow_delay(55);
  } else {
	hal_ow_delay(10);
  }
  unlock();
  return(r);
}

PRIVATE int ow_read_rom(user)
phys_bytes user;
{
/* Reset, READ ROM (0x33), 8 bytes: family, serial, CRC8 (Dallas). */
  u8_t rom[8], crc = 0;
  int i, j;

  if (!ow_reset()) return(ENODEV);		/* no key on the contacts */
  for (i = 0; i < 8; i++) (void) ow_bit(0x33 >> i & 1);
  for (i = 0; i < 8; i++) {
	rom[i] = 0;
	for (j = 0; j < 8; j++) rom[i] |= (u8_t) (ow_bit(1) << j);
  }
  for (i = 0; i < 7; i++) {
	crc ^= rom[i];
	for (j = 0; j < 8; j++) crc = (crc & 1) ? (crc >> 1) ^ 0x8C : crc >> 1;
  }
  if (crc != rom[7] || rom[0] == 0) return(EIO);	/* a bad touch */
  phys_copy(vir2phys(rom), user, (phys_bytes) 8);
  return(8);
}


/*===========================================================================*
 *				power_read				     *
 *===========================================================================*/
/* /dev/power (T15a): the BQ27220's standard values as one text line,
 *	soc=87 mv=3912 ma=-6 avg=-5 sby=-5 temp=243 flags=0x0181 usb=0 chg=0 vbus=0
 * read now, on the task stack (no static RAM): % of charge, mV, mA (signed,
 * < 0 discharging), the gauge's average and standby current, 0.1 C, the
 * raw BatteryStatus, and whether a USB host is there (the bus not
 * suspended; the gauge's DSG flag says nothing below its discharge
 * threshold, tens of mA); then the charger's state (T14c): chg 0 not
 * charging, 1 pre-charge, 2 fast, 3 done; vbus its VBUS_STAT (0 none, 1
 * USB host, ...).  The gauge is never written.  More fields come at the
 * end. */
PRIVATE const u8_t pw_cmd[7] = {
  HAL_GAUGE_SOC, HAL_GAUGE_VOLT, HAL_GAUGE_CURRENT, HAL_GAUGE_AVG,
  HAL_GAUGE_STANDBY, HAL_GAUGE_TEMP, HAL_GAUGE_STATUS
};
PRIVATE const char pw_key[] = "soc=\0 mv=\0 ma=\0 avg=\0 sby=\0 temp=\0 flags=0x";

PRIVATE int power_read(user, count)
phys_bytes user;
int count;
{
  char line[128], d[6];
  const char *k = pw_key;
  int i, j, n = 0, v, r6, r4, r3;

  for (i = 0; i < 7; i++) {
	if ((v = hal_gauge_read(pw_cmd[i])) < 0)
		return(v == -2 ? ENXIO : EIO);
	while (*k != 0) line[n++] = *k++;
	k++;
	if (i == 6) {				/* flags, 4 hex digits */
		for (j = 12; j >= 0; j -= 4)
			line[n++] = "0123456789abcdef"[v >> j & 15];
		continue;
	}
	if (i >= 2 && i <= 4) v = (short) v;	/* currents: signed */
	if (i == 5) v -= 2731;			/* 0.1 K -> 0.1 C */
	if (v < 0) { line[n++] = '-'; v = -v; }
	j = 0;
	do d[j++] = '0' + v % 10; while ((v /= 10) != 0);
	while (j > 0) line[n++] = d[--j];
  }
  for (k = " usb=0"; *k != 0; k++) line[n++] = *k;
  if (hal_usb_awake()) line[n - 1] = '1';
  if ((v = hal_charger_read(HAL_CHG_STATUS)) < 0) return(EIO);
  for (k = " chg=0 vbus=0"; *k != 0; k++) line[n++] = *k;
  line[n - 8] += v >> 3 & 3;		/* CHRG_STAT */
  line[n - 1] += v >> 5 & 7;		/* VBUS_STAT */
  /* R6.1: what the charger is set to (end-of-charge mV, charge mA, 5 V) */
  if ((r6 = hal_charger_read(0x06)) < 0 || (r4 = hal_charger_read(0x04)) < 0
			|| (r3 = hal_charger_read(0x03)) < 0) return(EIO);
  for (i = 0; i < 2; i++) {
	for (k = i == 0 ? " vreg=" : " ichg="; *k != 0; k++) line[n++] = *k;
	v = i == 0 ? 3840 + (r6 >> 2) * 16 : (r4 & 0x7F) * 64;
	j = 0;
	do d[j++] = '0' + v % 10; while ((v /= 10) != 0);
	while (j > 0) line[n++] = d[--j];
  }
  for (k = " otg=0\n"; *k != 0; k++) line[n++] = *k;
  if (r3 & 0x20) line[n - 2] = '1';
  if (count > n) count = n;
  phys_copy(vir2phys(line), user, (phys_bytes) count);
  return(count);
}


/*===========================================================================*
 *				power_write				     *
 *===========================================================================*/
/* R6.1: a command line written to /dev/power (writable by root only):
 *	profile full|care	end of charge 4.208 V / 4.0 V (kept)
 *	gentle on|off		charge current 1 A / the stock 2 A (kept)
 *	otg on|off		5 V on the GPIO header
 * Text rather than ioctl: FS checks the write permission, the kernel does
 * not know the caller's uid. */
PRIVATE const char *const pw_cmds[] = {
  "profile full", "profile care", "gentle off", "gentle on", "otg off", "otg on"
};

PRIVATE int power_write(user, count)
phys_bytes user;
int count;
{
  char cmd[16];
  int n = count, i;

  if (n > (int) sizeof(cmd) - 1) return(EINVAL);
  phys_copy(user, vir2phys(cmd), (phys_bytes) n);
  while (n > 0 && (cmd[n - 1] == '\n' || cmd[n - 1] == ' ')) n--;
  cmd[n] = 0;
  for (i = 0; i < 6; i++) {
	if (strcmp(cmd, pw_cmds[i]) != 0) continue;
	if (hal_charger_set(HAL_CHG_CARE + i / 2, i & 1) < 0) return(EIO);
	return(count);
  }
  return(EINVAL);
}


/*===========================================================================*
 *				do_rw					     *
 *===========================================================================*/
PRIVATE int do_rw(m_ptr)
message *m_ptr;
{
  phys_bytes user;
  int count = m_ptr->COUNT;
  u8_t buf[3];

  if (count <= 0) return(EINVAL);
  user = m_ptr->m_type == DEV_WRITE	/* a source may be constant */
	? numap_src(m_ptr->PROC_NR, (vir_bytes) m_ptr->ADDRESS, count)
	: numap(m_ptr->PROC_NR, (vir_bytes) m_ptr->ADDRESS, count);
  if (user == 0) return(EFAULT);

  switch (m_ptr->DEVICE) {
#if LATTRACE
  case LAT_MINOR:			/* the latency trace (arm/lat.c) */
	if (m_ptr->m_type == DEV_READ) return(lat_read(user, count));
	lat_clear();
	return(count);
#endif

  case LCD_MINOR:
	if (count > FB_SIZE) count = FB_SIZE;
	/* R4b.8: a frame from behind the foreground app is dropped */
	if (m_ptr->m_type == DEV_WRITE && fg_other(m_ptr->PROC_NR))
		return(count);
	if (m_ptr->m_type == DEV_READ) {
		if (lc_on) return(0);	/* console text is not for apps */
		phys_copy(vir2phys(fbuf), user, (phys_bytes) count);
	} else if (count == 1) {
		phys_copy(user, vir2phys(buf), (phys_bytes) 1);
		if (buf[0] != 'C') return(EINVAL);
		memset(fbuf, 0, sizeof(fbuf));	/* the console from now on */
		lc_x = lc_y = lc_esc = 0;
		lc_on = TRUE;
		if (hal_lcd_flush(fbuf) < 0) lc_dirty = TRUE;	/* SD: later */
	} else {
		if (lc_on) {	/* a short frame must not leave console text
				 * behind in fbuf for a later read */
			memset(fbuf, 0, sizeof(fbuf));
			lc_on = FALSE;
		}
		phys_copy(user, vir2phys(fbuf), (phys_bytes) count);
		LAT_EVENT(LE_LCD, m_ptr->PROC_NR);
		if (hal_lcd_flush(fbuf) < 0) lc_dirty = TRUE;	/* SD: later */
	}
	return(count);

  case BTN_MINOR:
	if (m_ptr->m_type != DEV_READ) return(EINVAL);
	if (count < 2) return(EINVAL);
	if (fg_other(m_ptr->PROC_NR)) {	/* R4b.8: not yours: wait (until
					 * a signal; the keys are not kept) */
		if (m_ptr->TTY_FLAGS & O_NONBLOCK) return(EAGAIN);
		return(SUSPEND);
	}
	if (ev_head != ev_tail) {
		LAT_EVENT(LE_BREPLY, m_ptr->PROC_NR);
		return(deliver(m_ptr->PROC_NR, (vir_bytes) m_ptr->ADDRESS, count));
	}
	if (m_ptr->TTY_FLAGS & O_NONBLOCK) return(EAGAIN);
	if (rd_waiting) return(EIO);		/* one reader at a time */
	rd_waiting = TRUE;
	rd_caller = m_ptr->m_source;
	rd_proc = m_ptr->PROC_NR;
	rd_vir = (vir_bytes) m_ptr->ADDRESS;
	rd_count = count;
	return(SUSPEND);

  case VIBRO_MINOR: {
	/* "0" off, "1" on (or a byte 0/1), "N" (2..999): on for N ms, ended
	 * by the ms timer (R6.3), so the writer need not stay awake. */
	char t[4];
	int i, ms = 0, n = count < 4 ? count : 4;

	if (m_ptr->m_type != DEV_WRITE) return(EINVAL);
	if (n < 1) return(0);
	phys_copy(user, vir2phys(t), (phys_bytes) n);
	if (t[0] >= '0' && t[0] <= '9') {
		for (i = 0; i < n && t[i] >= '0' && t[i] <= '9'; i++)
			ms = ms * 10 + t[i] - '0';
		if (i == 4) return(EINVAL);		/* > 999 */
	} else {
		ms = t[0] != 0;
	}
	lock();
	mt_arm(MT_VIBRO, ms > 1 ? (unsigned) ms : 0);
	unlock();
	hal_vibro(ms != 0);
	return(count);
  }

  case SPK_MINOR: {
	u8_t nb[4 * NNOTES];
	int i, n;

	if (m_ptr->m_type != DEV_WRITE || count % 4 != 0) return(EINVAL);
	if (sp_waiting) return(EIO);		/* one tune at a time */
	if (ir_rd || tx_on) return(EBUSY);	/* sbuf holds the IR rings */
	if (count > (int) sizeof(nb)) count = sizeof(nb);
	phys_copy(user, vir2phys(nb), (phys_bytes) count);
	n = count / 4;
	for (i = 0; i < n; i++) {
		unsigned ms = nb[4*i+2] | nb[4*i+3] << 8;
		note_hz[i] = nb[4*i] | nb[4*i+1] << 8;
		note_ticks[i] = (ms * HZ + 999) / 1000;
		if (note_ticks[i] == 0) note_ticks[i] = 1;
	}
	lock();
	note_n = n;
	note_i = 0;
	note_left = note_ticks[0];
	hal_tone(note_hz[0]);
	unlock();
	sp_waiting = TRUE;
	sp_caller = m_ptr->m_source;
	sp_proc = m_ptr->PROC_NR;
	sp_count = count;
	return(SUSPEND);
  }

  case GPIO_MINOR: {
	u8_t gb[2 * HAL_GPIO_PINS];
	int i;

	if (m_ptr->m_type == DEV_READ) {
		if (count > HAL_GPIO_PINS) count = HAL_GPIO_PINS;
		for (i = 0; i < count; i++) gb[i] = '0' + hal_gpio_read(i);
		phys_copy(vir2phys(gb), user, (phys_bytes) count);
		return(count);
	}
	if (count % 2 != 0) return(EINVAL);
	if (count > (int) sizeof(gb)) count = sizeof(gb);
	phys_copy(user, vir2phys(gb), (phys_bytes) count);
	for (i = 0; i < count; i += 2)
		if (hal_gpio(gb[i], gb[i+1]) < 0) return(i > 0 ? i : EINVAL);
	return(count);
  }

  case SUBGHZ_MINOR:
	if (m_ptr->m_type == DEV_WRITE) return(subghz_write(user, count));
	if (m_ptr->POSITION != 0) return(0);	/* one line, then EOF */
	return(subghz_read(user, count));

  case ADC_MINOR:
	if (m_ptr->m_type != DEV_READ) return(EINVAL);
	if (m_ptr->POSITION != 0) return(0);	/* one line, then EOF */
	return(adc_read(user, count));

  case IBUTTON_MINOR:
	if (m_ptr->m_type != DEV_READ) return(EINVAL);
	if (m_ptr->POSITION != 0) return(0);	/* one ROM, then EOF */
	if (count < 8) return(EINVAL);
	return(ow_read_rom(user));

  case POWER_MINOR:
	if (m_ptr->m_type == DEV_WRITE) return(power_write(user, count));
	if (m_ptr->POSITION != 0) return(0);	/* one line, then EOF */
	return(power_read(user, count));

  case IR_MINOR:
	if (m_ptr->m_type == DEV_WRITE) return(ir_write(m_ptr, user, count));
	if (count < 4) return(EINVAL);
	if (hal_ir_irq() < 0) return(ENXIO);
	if (m_ptr->TTY_FLAGS & O_NONBLOCK) return(EAGAIN);  /* no frame kept */
	if (ir_rd) return(EIO);			/* one reader at a time */
	if (sp_waiting) return(EBUSY);		/* sbuf: the speaker's */
	rx_t = rx_h;				/* nothing old */
	ir_caller = m_ptr->m_source;
	ir_proc = m_ptr->PROC_NR;
	ir_vir = (vir_bytes) m_ptr->ADDRESS;
	ir_count = count & ~3;
	ir_got = 0;
	ir_started = FALSE;
	ir_rd = TRUE;
	return(SUSPEND);

  case BL_MINOR:
	if (m_ptr->m_type == DEV_READ) {	/* the state, 4 bytes (hal.h) */
		u32_t st;

		bl_apply();		/* a key that just came: on first */
		st = hal_bl_state();
		if (m_ptr->POSITION != 0) return(0);	/* EOF after it */
		if (count < 4) return(EINVAL);
		phys_copy(vir2phys(&st), user, (phys_bytes) 4);
		return(4);
	}
	/* fall through */
  case LED_MINOR:
	if (m_ptr->m_type != DEV_WRITE) return(EINVAL);
	if (count > 3) count = 3;
	phys_copy(user, vir2phys(buf), (phys_bytes) count);
	if (m_ptr->DEVICE == BL_MINOR) {
		/* brightness; 3 bytes: and the idle timeout in seconds
		 * (little endian, 0: never).  Kept over resets. */
		u32_t st = hal_bl_state() & 0xFFFF;

		if (count == 3) st = buf[1] | (u32_t) buf[2] << 8;
		else if (count != 1) return(EINVAL);
		hal_bl_state_set((u32_t) buf[0] << 16 | st);	/* on */
		hal_backlight(buf[0]);
		k_input = get_uptime();		/* the timeout starts now */
	}
	else if (count == 3) hal_led(buf[0], buf[1], buf[2]);
	else return(EINVAL);
	return(count);
  }
  return(ENXIO);
}


/*===========================================================================*
 *				deliver					     *
 *===========================================================================*/
PRIVATE int deliver(proc, vir, count)
int proc;
vir_bytes vir;
int count;
{
/* Copy as many whole events as fit; returns the byte count. */
  u8_t buf[2 * NEVENTS];
  phys_bytes user;
  int n = 0;

  while (ev_tail != ev_head && n + 2 <= count && n + 2 <= sizeof(buf)) {
	buf[n++] = ev_key[ev_tail % NEVENTS];
	buf[n++] = ev_down[ev_tail % NEVENTS];
	ev_tail++;
  }
  if ((user = numap(proc, vir, (vir_bytes) n)) == 0) return(EFAULT);
  phys_copy(vir2phys(buf), user, (phys_bytes) n);
  return(n);
}


/*===========================================================================*
 *				bl_apply				     *
 *===========================================================================*/
PRIVATE void bl_apply()
{
/* In the last 5 s of the idle timeout the backlight dims by 10 % a second,
 * then it fades out in 0.5 s (owner, 2026-11-15: 15 s, so from 10 s on;
 * 0 = never), and it comes back at once at any key or button (k_input).
 * The ramps run in the LED driver.  The setting and the state live in an
 * RTC backup register (no RAM). */
  u32_t st = hal_bl_state();
  int want = bl_want(st, get_uptime()), cur = bl_state(st);
  unsigned lv = HAL_BL_LEVEL(st);

  if (want == cur) return;
  if (cur == BL_SAVE) {		/* the LCD back first, as it was: no redraw */
	if (hal_lcd_save(0) < 0) return;	/* the SD card's: next tick */
	hal_bl_state_set(st &= ~HAL_BL_STEPS);
	if ((cur = BL_GONE) == want) return;
  }
  if (want == BL_SAVE) {
	if (cur == BL_GONE) {
		if (hal_lcd_save(1) == 0) hal_bl_state_set(st | HAL_BL_STEPS);
		return;
	}
	want = BL_GONE;			/* the light out first */
  }
  st &= ~(HAL_BL_STEPS | HAL_BL_OFF);
  if (want == BL_GONE) {
	hal_backlight_fade(lv - lv * cur / 10, 0, 500);	/* out, 0.5 s */
	hal_bl_state_set(st | HAL_BL_OFF);
  } else if (want > 0 && cur != BL_GONE) {
	hal_backlight_fade(lv - lv * cur / 10, lv - lv * want / 10, 200);
	hal_bl_state_set(st | (u32_t) want << 24);
  } else if (want == 0) {
	hal_backlight(lv);			/* on at once */
	hal_bl_state_set(st);
  }
}

PRIVATE int bl_want(st, now)	/* 0 on, 1..5 dimmed, BL_GONE off, BL_SAVE */
u32_t st;
clock_t now;
{
/* LCD_SAVE_S after the light went off, on battery only, the LCD sleeps as
 * well (T14d; a few hundred uA). */
  clock_t t = (clock_t) HAL_BL_TIMEOUT(st) * HZ, idle = now - k_input;

  if (t == 0) return(0);
  if (idle >= t + (clock_t) LCD_SAVE_S * HZ && !hal_usb_awake())
	return(BL_SAVE);
  if (idle >= t) return(BL_GONE);
  if (t > BL_DIMS * HZ && idle >= t - BL_DIMS * HZ)
	return((int) ((idle - (t - BL_DIMS * HZ)) / HZ) + 1);
  return(0);
}

PRIVATE int bl_state(st)	/* what bl_apply last did */
u32_t st;
{
  if (!(st & HAL_BL_OFF)) return((int) HAL_BL_STEP(st));
  return(HAL_BL_STEP(st) == 7 ? BL_SAVE : BL_GONE);
}


/*===========================================================================*
 *				do_hardint				     *
 *===========================================================================*/
PRIVATE void do_hardint()
{
/* New button events: finish a suspended read.  Console text on the LCD:
 * show it. */
  int r;

  bl_apply();
  if (last_buttons & (FG_LONG | FG_KILL)) {	/* R4b.8, here: not in the
						 * interrupt handler */
	unsigned b, sec = (unsigned) (get_uptime() / HZ);
	int fg;

	lock();
	b = last_buttons;
	fg = FG_SLOT(b);
	if (b & FG_LONG) {
		b = (b & ~(FG_LONG | FG_STAMPS)) | FG_KILL | (sec & 0xFF) << 8;
		if (fg >= 0) cause_sig(fg, SIGTERM);
	} else if (((sec - FG_STAMP(b)) & 0xFF) >= FG_KILL_S) {
		b &= ~FG_KILL;
		if (fg >= 0 && !(proc_addr(fg)->p_flags & P_SLOT_FREE))
			cause_sig(fg, SIGKILL);
	}
	last_buttons = b;
	unlock();
  }
  if (lc_dirty) {		/* console text, or a frame the SD held up */
	lc_dirty = FALSE;
	if (hal_lcd_flush(fbuf) < 0) lc_dirty = TRUE;
  }
  if (sp_waiting && note_i >= note_n) {		/* the tune is over */
	sp_waiting = FALSE;
	reply(REVIVE, sp_caller, sp_proc, sp_count);
  }
  if (rx_h != rx_t || rx_over) ir_drain();	/* received durations */
  if (tx_on) {
	if (tx_done) {				/* the frame is out */
		tx_on = FALSE;
		reply(REVIVE, tx_caller, tx_proc, tx_err ? EIO : tx_count);
	} else
		ir_fill();
  }
  if (!rd_waiting || ev_head == ev_tail) return;
  rd_waiting = FALSE;
  LAT_EVENT(LE_BREPLY, rd_proc);
  r = deliver(rd_proc, rd_vir, rd_count);
  reply(REVIVE, rd_caller, rd_proc, r);
}


/*===========================================================================*
 *				flipdev_tick				     *
 *===========================================================================*/
PUBLIC void flipdev_tick()
{
/* Called by the clock interrupt handler: the LCD console, the speaker,
 * the end of an IR frame, the backlight; the buttons when they have no
 * interrupt (QEMU).
 */
  u32_t st = hal_bl_state();

  if (lc_dirty && ++lc_wait >= LC_TICKS) {
	lc_wait = 0;
	interrupt(PRINTER);
  }
  if (note_i < note_n && --note_left == 0) {	/* next note, or quiet */
	if (++note_i < note_n) {
		note_left = note_ticks[note_i];
		hal_tone(note_hz[note_i]);
	} else {
		hal_tone(0);
		interrupt(PRINTER);
	}
  }
  if (rx_active && rx_level == 1
		&& hal_cycles() - rx_last > IR_GAP_MS * 1000 * CYC_US) {
	rx_active = FALSE;			/* silence: the frame is over */
	if (ir_rd) {
		if (RING_N(rx_h, rx_t) < IR_RXQ)
			ir_rxq[rx_h++ % IR_RXQ] = IR_END;
		else rx_over = TRUE;
		interrupt(PRINTER);
	}
  }
  /* the backlight's idle timeout: the task does the I2C (bl_apply) */
  if (bl_want(st, uptime_irq()) != bl_state(st)) interrupt(PRINTER);
  if (!(last_buttons & BTN_EXTI)) fg_hold(btn_sample());
  if ((last_buttons & FG_KILL) && ((unsigned) (uptime_irq() / HZ)
		- FG_STAMP(last_buttons) & 0xFF) >= FG_KILL_S)
	interrupt(PRINTER);
}


/*===========================================================================*
 *				flipdev_idle				     *
 *===========================================================================*/
PUBLIC clock_t flipdev_idle(now)
clock_t now;
{
/* Tickless idle (T14b): the ticks until flipdev_tick has work, 0 if it
 * has some on every tick (a tune, an IR reader or frame, the LCD console
 * waiting for the bus).  The backlight's next step: see bl_want. */
  u32_t st = hal_bl_state();
  clock_t t = (clock_t) HAL_BL_TIMEOUT(st) * HZ, idle = now - k_input, d;

  if (lc_dirty || note_i < note_n || rx_active || ir_rd || tx_on) return(0);
  if (last_buttons & FG_KILL) return(HZ);	/* SIGKILL due soon */
  if (t == 0 || bl_state(st) == BL_SAVE) return(LONG_MAX);
  if (st & HAL_BL_OFF) {		/* the LCD's power save is next */
	t += (clock_t) LCD_SAVE_S * HZ;
	if (hal_usb_awake()) return(LONG_MAX);
	return(idle >= t ? 0 : t - idle);
  }
  if (idle >= t) return(0);
  if (t <= BL_DIMS * HZ) return(t - idle);
  d = t - BL_DIMS * HZ;			/* the dimming starts */
  return(idle < d ? d - idle : HZ - (idle - d) % HZ);
}


/*===========================================================================*
 *				foreground (R4b.8)			     *
 *===========================================================================*/
/* The foreground app's slot is kept in last_buttons (no RAM).  fg_other:
 * a foreground app is set, alive, and proc is not it. */
PRIVATE int fg_other(proc)
int proc;
{
  int fg = FG_SLOT(last_buttons);

  return(fg >= 0 && fg != proc && !(proc_addr(fg)->p_flags & P_SLOT_FREE));
}

PRIVATE int fg_ioctl(m_ptr)
message *m_ptr;
{
  phys_bytes user;
  int pid, fg, n;
  struct proc *rp;

  if (m_ptr->DEVICE != LCD_MINOR && m_ptr->DEVICE != BTN_MINOR) return(ENOTTY);
  if (m_ptr->TTY_REQUEST != FGIOCSET) return(ENOTTY);
  if (fg_other(m_ptr->PROC_NR)) return(EPERM);
  user = numap(m_ptr->PROC_NR, (vir_bytes) m_ptr->ADDRESS, sizeof(int));
  if (user == 0) return(EFAULT);
  phys_copy(user, vir2phys(&pid), (phys_bytes) sizeof(int));
  fg = -1;
  if (pid != 0) {
	for (n = 0; n < NR_PROCS; n++) {
		rp = proc_addr(n);
		if (!(rp->p_flags & P_SLOT_FREE) && rp->p_pid == pid) break;
	}
	if (n == NR_PROCS) return(ESRCH);
	/* never a server or init: holding Back would kill the system */
	if (n <= INIT_PROC_NR) return(EPERM);
	fg = n;
  }
  lock();
  last_buttons = (last_buttons & ~(FG_BITS | FG_LONG | FG_KILL))
					| (unsigned) (fg + 1) << 16;
  unlock();
  return(OK);
}

PRIVATE void fg_hold(now)
unsigned now;
{
/* In the button sampling (interrupt): Back held FG_LONG_T with a
 * foreground app: the task sends SIGTERM (cause_sig is task work). */
  unsigned b = last_buttons;

  if (!(now & BTN_BACK) || FG_SLOT(b) < 0 || (b & (FG_LONG | FG_KILL)))
	return;
  if ((((unsigned) uptime_irq() - FG_STAMP(b)) & 0xFF) < FG_LONG_T) return;
  last_buttons = b | FG_LONG;
  interrupt(PRINTER);
}


/*===========================================================================*
 *				flipdev_vibro_off			     *
 *===========================================================================*/
PUBLIC void flipdev_vibro_off()
{
/* The ms timer: a timed buzz is over (R6.3). */
  hal_vibro(0);
}


/*===========================================================================*
 *				btn_int					     *
 *===========================================================================*/
PRIVATE int btn_int(irq)
int irq;
{
/* An edge on a button (T14b): take it now, then turn the edges off; the
 * ms timer samples while any is down, which also debounces.
 */
  hal_btn_arm(0);
  last_buttons |= BTN_POLL;
  (void) btn_sample();
  mt_arm(MT_BUTTONS, BTN_MS);
  return 1;
}


/*===========================================================================*
 *				flipdev_btn				     *
 *===========================================================================*/
PUBLIC void flipdev_btn()
{
/* The ms timer, BTN_MS after an edge or the last sample: all up in two
 * samples in a row, and the edges interrupt again.
 */
  unsigned was = last_buttons & ~BTN_FLAGS, now;

  if ((now = btn_sample()) != 0 || was != 0) {
	fg_hold(now);
	mt_arm(MT_BUTTONS, BTN_MS);
	return;
  }
  last_buttons &= ~BTN_POLL;
  hal_btn_arm(1);
  if (hal_buttons() != 0) (void) btn_int(0);	/* pressed just now */
}


/*===========================================================================*
 *				btn_sample				     *
 *===========================================================================*/
PRIVATE unsigned btn_sample()
{
/* In an interrupt handler: sample the buttons, queue the changes, wake the
 * task if somebody is waiting.  Returns the buttons down.
 */
  unsigned now = hal_buttons(), b;
  unsigned flags = last_buttons & BTN_FLAGS;
  unsigned changed = (now ^ last_buttons) & ~BTN_FLAGS;

  if (changed == 0) return(now);
  k_input = uptime_irq();		/* no background flash work now */
  if ((changed & now & BTN_BACK) && !(flags & FG_KILL))	/* R4b.8 */
	flags = (flags & ~FG_STAMPS) | ((unsigned) k_input & 0xFF) << 8;
  /* With the backlight off, a press only turns it on (owner): neither it
   * nor anything else reaches the apps until every button is up again. */
  if (hal_bl_state() & (HAL_BL_OFF | HAL_BL_STEPS))
	interrupt(PRINTER);		/* the light back on, at once */
  if ((hal_bl_state() & HAL_BL_OFF) && (now & changed)) flags |= SWALLOW;
  if (flags & SWALLOW) {
	if (now == 0) flags &= ~SWALLOW;
	last_buttons = now | flags;
	return(now);
  }
  for (b = 1; b <= BTN_BACK; b <<= 1) {
	if (!(changed & b)) continue;
	if (ev_head - ev_tail < NEVENTS) {
		ev_key[ev_head % NEVENTS] = b;
		ev_down[ev_head % NEVENTS] = (now & b) != 0;
		if (now & b) LAT_EVENT(LE_BTN, b);
		ev_head++;
	}
  }
  last_buttons = now | flags;
  if (rd_waiting) interrupt(PRINTER);
  return(now);
}


/*===========================================================================*
 *				reply					     *
 *===========================================================================*/
PRIVATE void reply(code, replyee, proc, status)
int code, replyee, proc, status;
{
  message m;

  m.m_type = code;
  m.REP_PROC_NR = proc;
  m.REP_STATUS = status;
  send(replyee, &m);
}


/*===========================================================================*
 *				lcd_cons_putc				     *
 *===========================================================================*/
PUBLIC void lcd_cons_putc(c)
int c;
{
/* A console character, for the LCD if the console is shown there.  Escape
 * sequences (ESC [ ... letter) are skipped. */
  const unsigned char *g;
  int px, py, row, col;

  if (!lc_on) return;
  if (lc_esc) {
	if (lc_esc == 1) lc_esc = (c == '[') ? 2 : 0;
	else if (c >= 0x40 && c <= 0x7E) lc_esc = 0;
	return;
  }
  switch (c) {
  case 033:	lc_esc = 1; return;
  case '\r':	lc_x = 0; return;
  case '\n':	lc_newline(); break;
  case '\b':	if (lc_x > 0) lc_x--; return;
  case '\t':	c = ' '; /* one space: the screen is narrow */ /* FALLTHROUGH */
  default:
	if (c < ' ' || c > '~') return;
	if (lc_x >= LC_COLS) {
		lc_x = 0;
		lc_newline();
	}
	g = font_4x6[c - ' '];
	for (row = 0; row < 6; row++) {
		py = lc_y * 6 + row;
		for (col = 0; col < 4; col++) {
			px = lc_x * 4 + col;
			if (g[row] & (0x80 >> col))
				fbuf[(py >> 3) * LCD_W + px] |= 1 << (py & 7);
			else
				fbuf[(py >> 3) * LCD_W + px] &= ~(1 << (py & 7));
		}
	}
	lc_x++;
  }
  lc_dirty = TRUE;
}


PRIVATE void lc_newline()
{
/* Next line; at the bottom, move every pixel column up by one text row. */
  unsigned long long colbits;
  int x, p;

  lc_x = 0;
  if (lc_y < LC_ROWS - 1) {
	lc_y++;
	return;
  }
  for (x = 0; x < LCD_W; x++) {
	colbits = 0;
	for (p = LCD_H / 8 - 1; p >= 0; p--)
		colbits = colbits << 8 | fbuf[p * LCD_W + x];
	colbits >>= 6;
	for (p = 0; p < LCD_H / 8; p++) {
		fbuf[p * LCD_W + x] = colbits;
		colbits >>= 8;
	}
  }
}


/*===========================================================================*
 *				ir_int					     *
 *===========================================================================*/
PRIVATE int ir_int(irq)
int irq;
{
/* An edge from the infrared receiver: queue the time since the last one
 * (a frame starts with a mark after a silence). */
  u32_t now = hal_cycles(), us;
  int level = hal_ir_rx_edge();
  u16_t v;

  if (level == rx_level) return 1;	/* glitch, nothing changed */
  rx_level = level;
  if (!rx_active) {
	if (level != 0) return 1;
	rx_active = TRUE;
	v = IR_START;
  } else {
	us = (now - rx_last) / CYC_US;
	if (us > 0x7FFE0) us = 0x7FFE0;
	v = IR_ENC(us);
  }
  rx_last = now;
  if (!ir_rd) return 1;			/* nobody reads: sbuf is not ours */
  if (RING_N(rx_h, rx_t) < IR_RXQ) ir_rxq[rx_h++ % IR_RXQ] = v;
  else rx_over = TRUE;			/* the task was too slow */
  if (RING_N(rx_h, rx_t) >= IR_RXQ / 2) interrupt(PRINTER);
  return 1;
}


/*===========================================================================*
 *				ir_drain				     *
 *===========================================================================*/
PRIVATE void ir_drain()
{
/* Move received durations to a waiting reader; without one they go. */
  phys_bytes user;
  u32_t d;
  u16_t v;

  while (rx_t != rx_h) {
	v = ir_rxq[rx_t % IR_RXQ];
	rx_t++;
	if (!ir_rd) continue;
	if (v == IR_START) {		/* a frame begins: it is the reader's */
		ir_started = TRUE;
		ir_got = 0;
		continue;
	}
	if (!ir_started || ir_got >= ir_count) continue;
	if (v == IR_END) {
		ir_rd = FALSE;
		reply(REVIVE, ir_caller, ir_proc, ir_got);
		continue;
	}
	d = IR_DEC(v);
	if ((user = numap(ir_proc, ir_vir + ir_got, (vir_bytes) 4)) == 0) {
		ir_rd = FALSE;
		reply(REVIVE, ir_caller, ir_proc, EFAULT);
		continue;
	}
	phys_copy(vir2phys(&d), user, (phys_bytes) 4);
	ir_got += 4;
  }
  if (rx_over) {			/* durations were lost */
	rx_over = FALSE;
	if (ir_rd && ir_started) {
		ir_rd = FALSE;
		reply(REVIVE, ir_caller, ir_proc, EIO);
	}
  }
}


/*===========================================================================*
 *				ir_write				     *
 *===========================================================================*/
PRIVATE int ir_write(m_ptr, user, count)
message *m_ptr;
phys_bytes user;
int count;
{
/* Check the frame, fill the transmit ring and start the pulse engine; the
 * writer waits (SUSPEND) and is revived when the last space is over. */
  u32_t d[16], hz = IR_HZ, duty = IR_DUTY, first_us, next_us;
  int i, j, k, n, first = 0;

  if (hal_ir_tx_irq() < 0) return(ENXIO);
  if (count % 4 != 0 || count == 0) return(EINVAL);
  if (tx_on) return(EIO);			/* one frame at a time */
  if (sp_waiting) return(EBUSY);		/* sbuf: the speaker's */
  n = count / 4;
  if (n > IR_TX_MAX + 1) return(EINVAL);
  for (i = 0; i < n; i += 16) {			/* check everything first */
	k = n - i < 16 ? n - i : 16;
	phys_copy(user + i * 4, vir2phys(d), (phys_bytes) k * 4);
	for (j = 0; j < k; j++) {
		if (i + j == 0 && (d[0] & IR_FREQ_FLAG)) {
			hz = d[0] & 0xFFFFF;
			duty = (d[0] >> 20) & 0x7F;
			if (duty == 0) duty = IR_DUTY;
			if (hz < 10000 || hz > 60000 || duty > 90)
				return(EINVAL);
			first = 1;
		} else if (d[j] == 0 || d[j] > 1000000)
			return(EINVAL);		/* 0, or over a second */
	}
  }
  if ((n - first) % 2 == 0 || n - first > IR_TX_MAX) return(EINVAL);

  tx_caller = m_ptr->m_source;
  tx_proc = m_ptr->PROC_NR;
  tx_vir = (vir_bytes) m_ptr->ADDRESS + first * 4;
  tx_count = count;
  tx_total = n - first;
  tx_fetch = 0;
  tx_h = tx_t = 0;
  tx_done = tx_err = FALSE;
  tx_on = TRUE;
  ir_fill();
  if (tx_h == tx_t) {
	tx_on = FALSE;
	return(EFAULT);
  }
  hal_ir_freq(hz, duty);
  first_us = IR_DEC(ir_txq[tx_t % IR_TXQ]);
  tx_t++;
  next_us = first_us;
  if (tx_total > 1) {
	next_us = IR_DEC(ir_txq[tx_t % IR_TXQ]);
	tx_t++;
  }
  tx_seg = 1;
  hal_ir_carrier(1);				/* the first mark */
  hal_ir_tx_start(first_us, next_us);
  return(SUSPEND);
}


/*===========================================================================*
 *				ir_fill					     *
 *===========================================================================*/
PRIVATE void ir_fill()
{
/* Top up the transmit ring from the writer (its image may have moved:
 * numap every time). */
  phys_bytes user;
  u32_t d;

  while (tx_fetch < tx_total && RING_N(tx_h, tx_t) < IR_TXQ) {
	if ((user = numap(tx_proc, tx_vir + tx_fetch * 4, (vir_bytes) 4)) == 0)
		break;
	phys_copy(user, vir2phys(&d), (phys_bytes) 4);
	ir_txq[tx_h % IR_TXQ] = IR_ENC(d);
	tx_h++;
	tx_fetch++;
  }
}


/*===========================================================================*
 *				ir_tx_int				     *
 *===========================================================================*/
PRIVATE int ir_tx_int(irq)
int irq;
{
/* A mark or space is over: the next one (already loaded) starts now. */
  int update = hal_ir_tx_ack();

  if (!tx_on || tx_done) {			/* nothing to send: off */
	hal_ir_tx_stop();
	return 1;
  }
  if (!update) return 1;			/* not the timer's update */
  if (tx_seg >= tx_total) {			/* the last one ended */
	ir_stop_tx();
	tx_done = TRUE;
	interrupt(PRINTER);
	return 1;
  }
  hal_ir_carrier((tx_seg & 1) == 0);		/* even: mark */
  tx_seg++;
  if (tx_seg < tx_total) {			/* load the one after */
	if (tx_t == tx_h) {			/* the task was too slow */
		ir_stop_tx();
		tx_err = tx_done = TRUE;
		interrupt(PRINTER);
		return 1;
	}
	hal_ir_tx_next(IR_DEC(ir_txq[tx_t % IR_TXQ]));
	tx_t++;
	/* a quarter gone: refill, so that the task may come up to 48
	 * durations late (an SD piece, other tasks: R0.8b) */
	if (RING_N(tx_h, tx_t) == IR_TXQ * 3 / 4 && tx_fetch < tx_total)
		interrupt(PRINTER);
  }
  return 1;
}


/*===========================================================================*
 *				ir_busy					     *
 *===========================================================================*/
PUBLIC int ir_busy()
{
/* A frame is going out: no background flash work now (system.c). */
  return tx_on;
}


/*===========================================================================*
 *				ir_stop_tx				     *
 *===========================================================================*/
PRIVATE void ir_stop_tx()
{
  hal_ir_tx_stop();
  hal_ir_carrier(0);
}
