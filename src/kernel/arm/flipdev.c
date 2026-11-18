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
 *				the write returns when they have played.
 *				Volume: a note of 0xFFFF Hz sets the volume
 *				of the notes after it to its duration field,
 *				0..100 (above 100: 100), and plays nothing.
 *				It lasts until the next such note or until
 *				the device is closed, which sets 100 again
 *				(the full sound of programs that never set
 *				it).  The duty is 50 % * (vol/100)^3 (hal.h).
 *	minor 6  /dev/gpio	the eight free header pins (hal.h), 0..7
 *				write: pairs of bytes, pin and operation:
 *				  0 low, 1 high (output), 2 input, 3 input
 *				  with pull-up, 4 with pull-down, 5 off
 *				read: one byte per pin, '0' or '1', from pin 0
 *	minors 17-19		/dev/hidkbd, /dev/hidmouse, /dev/hidcons:
 *				USB HID reports (hiddev.c, <minix/hid.h>)
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
 * timed by a timer interrupt that switches the carrier; received frames
 * are timed by the pulse engine (R4b.7: timer capture, DMA straight into
 * the reader's buffer, one interrupt at the start and one at the end).
 * Nobody busy-waits.
 *
 * poll() (R4b.6, protocol #131): DEV_SELECT says which ops are ready.
 * /dev/buttons is readable with an event queued for the caller (not while
 * another app owns the buttons, R4b.8), /dev/speaker writable when no tune
 * plays; every other minor is always ready (IR, RFID and Sub-GHz capture
 * only while a read waits: nothing is "readable" before the read).  With
 * SEL_NOTIFY the two remember what FS waits for and send one DEV_SELECTED:
 * the buttons on a new event (not on a queue the owner has not read yet:
 * that would wake FS again and again), the speaker when the tune ends.
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
#include <minix/rtc.h>
#include <minix/rfid.h>
#include <minix/ibutton.h>
#include <minix/hid.h>
#include <minix/subghz.h>
#include <signal.h>
#include "ibemu.c"		/* the Dallas key's 1-Wire slave (R6.7) */

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
#define ADCS_MINOR	12	/* a header pin sampled into the reader (R6.4) */
#define RTC_MINOR	13	/* the RTC alarm, calibration (R6.2, R7b.1) */
#define SGRAW_MINOR	14	/* its demodulated data, timed (R4b.7);
				 * a write sends a frame (R8.2) */
#define RFID_MINOR	16	/* a 125 kHz tag's signal, timed (R6.5) */
			/* (15 is /dev/lat, include/minix/lat.h) */
#define LAST_MINOR	16	/* (17-19: USB HID, hiddev.c) */
#define SGPKT_MINOR	20	/* Sub-GHz packets, <minix/subghz.h> */
#define NNOTES		64
#define FB_SIZE		(LCD_W * LCD_H / 8)
#define NEVENTS		32		/* button event ring (power of 2) */

#define O_NONBLOCK	04000		/* from <fcntl.h> */

PRIVATE u8_t fbuf[FB_SIZE];
PRIVATE message fm;

/* Button events, filled by flipdev_tick() at interrupt level. */
PRIVATE u8_t ev_key[NEVENTS], ev_down[NEVENTS];
PRIVATE volatile u8_t ev_head, ev_tail;	/* wrap: NEVENTS divides 256 */
PRIVATE u8_t sel_wait;		/* poll(): SEL_RD buttons, SEL_WR speaker */
PRIVATE u8_t sel_head;		/* ev_head when the buttons registered */
PRIVATE u8_t fg_pre;		/* R7b.2: an alert preempts (PRE_*), 0: no */
#define PRE_ON		0x80
#define PRE_FG		0x40	/* the slot below was the foreground app */
#define PRE_SLOT	0x3F	/* the preempted foreground's slot + 1 */
PRIVATE u8_t pre_rd;		/* R7b.2: a reader parked meanwhile, slot + 1 */
PRIVATE volatile u8_t gp_watch;	/* R6.4: header pins watched for edges */
PRIVATE volatile u8_t gp_last;	/*   their levels at the last tick */
PRIVATE volatile u8_t gp_chg;	/*   changed since the last read */
PRIVATE u8_t gp_proc;		/*   who watches, slot + 1 (its close ends it) */
PRIVATE struct ie_state ie;	/* R6.7 part 3: a Dallas key emulated */
#define SEL_GPIO	4	/* sel_wait: a poller waits for an edge */
PRIVATE u8_t nt_st;		/* N.6: the card notice (NT_*) */
#define NT_KIND		3	/* the notice over every frame: */
#define NT_OUT		1	/*   the card is out */
#define NT_OTHER	2	/*   another card, or one that does not answer */
#define NT_NEW		0x40	/* the task draws it, flushes and buzzes */
#define NT_SEEN		0x80	/* the switch said "card in" at the last tick */
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
#define GE_SAVED	0x00000040u	/* R6.10: the LED saved in ge_led (task) */
#define GE_BLINK	0x00000080u	/* the red blink is lit now (task) */
#define GE_CHORD	0x04000000u	/* a gesture chord held since k_input */
#define GE_FIRE		0x08000000u	/* its hold time is reached: task work */
#define GE_OFF		0x10000000u	/* the foreground app turned them off */
#define BTN_ALL		0x3Fu		/* the buttons themselves */
#define BTN_FLAGS	(~BTN_ALL)
#define FG_LONG_T	HZ		/* Back held this long (ticks) */
#define FG_KILL_S	3		/* then SIGKILL after (s) */
#define BTN_MS		8	/* sampling while down, and the debounce */

/* R6.10 button gestures (T16): exactly these buttons held GE_HOLD toggles
 * the backlight's forced mode (1 off, 2 on; BKP15R bits 14-15).  Bits 0-5
 * of an entry are the buttons, 6-7 the action.  Never with Back (long
 * Back is R4b.8's).  The table is const for now; BTNIOCGTAB (root, from
 * /etc/flipper/gestures.conf) comes with a way to check the caller. */
PRIVATE const u8_t ge_tab[] = {
  BTN_LEFT | BTN_UP | 1 << 6,		/* forced off */
  BTN_RIGHT | BTN_DOWN | 2 << 6,	/* forced on */
};
#define GE_N		(sizeof(ge_tab) / sizeof(ge_tab[0]))
#define GE_HOLD		(3 * HZ / 2)	/* 1.5 s, in ticks */
#define GE_RED		64		/* the blink: red at ~25 % */
PRIVATE u8_t ge_led[3];		/* the LED before the blink (3 B) */
PRIVATE u8_t sp_quiet;		/* /dev/speaker: 100 - the volume (0: full);
				 * here it fills ge_led's padding */

/* Notes being played (/dev/speaker), timed by flipdev_tick(); the same
 * memory holds the infrared transmit ring while a frame goes out (one or
 * the other: EBUSY), so it costs no RAM. */
#define IR_TXQ		64		/* transmit ring (power of 2: u8 indices) */
PRIVATE union {
  struct { u16_t hz[NNOTES], ticks[NNOTES]; } sp;
  struct { u16_t txq[IR_TXQ]; } ir;
} sbuf;
#define note_hz		sbuf.sp.hz
#define note_ticks	sbuf.sp.ticks
#define ir_txq		sbuf.ir.txq
PRIVATE volatile int note_n, note_i;	/* count, current; note_i == note_n:
					 * done */
PRIVATE volatile unsigned note_left;	/* ticks left of the current one */
PRIVATE int sp_waiting, sp_caller, sp_proc, sp_count;
#define SPK_VOL		0xFFFF		/* the volume note's "frequency" */
#define SPK_VOLDEF	0xFFFE		/* the same, and the system default
					 * (K.7 Sound; kept in a backup
					 * register by the HAL) */

/* Infrared (/dev/ir).  Durations in the rings are 16 bits: microseconds
 * below 32768, else bit 15 and units of 64 us (up to 2 s). */
#define IR_TX_MAX	2048		/* durations per write */
#define IR_FREQ_FLAG	0x80000000UL	/* first word: carrier, not a time */
#define IR_HZ		38000
#define IR_DUTY		33
#define IR_GAP_MS	30		/* silence that ends a frame */
#define IR_ENC(us)	((us) < 0x8000 ? (u16_t) ((us) ? (us) : 1) \
				: (u16_t) (0x8000 | ((us) >> 6)))
/* IR_DEC uses v twice: never pass it an expression with side effects */
#define IR_DEC(v)	((v) & 0x8000 ? (u32_t) ((v) & 0x7FFF) << 6 : (u32_t) (v))

PRIVATE volatile u16_t rb;		/* the rainbow (R7b.5, see rb_step) */
PRIVATE volatile u8_t tx_h, tx_t;
PRIVATE volatile u8_t rtc_fired;	/* SIGRTC to INIT is due (R7b.1) */
PRIVATE volatile u8_t ir_rd;		/* a reader waits */
PRIVATE volatile u8_t ir_started, ir_done;	/* its frame began, ended */
PRIVATE u8_t ir_paused;			/* a frame went out meanwhile */
PRIVATE u8_t pu_input;			/* 0 /dev/ir, 1 /dev/subghzraw, 2 /dev/rfid */
PRIVATE u8_t rf_polls;			/* RFID: ms timer polls so far */
PRIVATE u8_t tx_rfid;			/* the frame goes to the RFID antenna */
PRIVATE u8_t rf_held;			/* R6.5 p3: the field held up across a
					 * T5577 command and its reply */
PRIVATE u8_t rf_emu;			/* R6.5 p4: the emulating process + 1, 0 none */
PRIVATE u8_t rf_carrier;		/* R9.0.4: the read field, RFID_CARRIER_* */
PRIVATE u8_t rf_settle;			/* ms-timer ticks before a held read
					 * arms its capture (field settling) */
PRIVATE volatile u8_t tx_on, tx_done, tx_err;
PRIVATE short ir_caller, ir_proc, tx_caller, tx_proc;
PRIVATE u16_t ir_count, tx_count;
PRIVATE volatile u16_t tx_seg;		/* segments started (by the timer) */
PRIVATE u16_t tx_total, tx_fetch;	/* durations; next one to fetch */
PRIVATE phys_bytes ir_buf;		/* reader's buffer (pinned) */
PRIVATE vir_bytes tx_vir;		/* first duration */
#define CYC_US		(hal_cpu_hz() / 1000000)	/* cycles per us */
#define RING_N(h, t)	((u8_t) ((h) - (t)))	/* entries in a ring */

/* A process blocked in a read of /dev/buttons. */
PRIVATE int rd_waiting;
PRIVATE int rd_caller, rd_proc, rd_count;
PRIVATE vir_bytes rd_vir;

FORWARD _PROTOTYPE( void reply, (int code, int replyee, int proc, int status) );
FORWARD _PROTOTYPE( int do_rw, (message *m_ptr)			);
FORWARD _PROTOTYPE( int rtc_ioctl, (message *m_ptr)			);
FORWARD _PROTOTYPE( int rtc_int, (int irq)				);
/* R7b.5: colour sequences run by the task from the ms timer: rainbow (the
 * six colours of the pride flag), dual, dualsoft and morph (two colours
 * given).  Each colour is held for its mode's time, then glides into the
 * next in steps of RB_MS.  rb: on, the next step is due, the mode, the
 * colour and the step within its glide. */
#define RB_ON		0x8000
#define RB_DUE		0x4000
#define RB_MODE(r)	((r) >> 12 & 3)
#define RB_COLOUR(r)	((r) >> 8 & 7)
#define RB_STEPOF(r)	((r) & 0x7F)
#define RB_MS		40
PRIVATE const struct {
	char name[9];
	u8_t ncol, glide;		/* colours; steps of RB_MS to the next */
	u16_t hold;			/* ms at the colour (0: glide on) */
} rb_mode[4] = {
	{ "rainbow",  6, 5, 960 },	/* 1 s each, 0.2 s glide (owner) */
	{ "dual",     2, 1, 500 },	/* switches every 0.5 s */
	{ "dualsoft", 2, 5, 960 },	/* as rainbow */
	{ "morph",    2, 125, 0 },	/* 5 s there, 5 s back */
};
PRIVATE const u8_t rb_col[6][3] = {	/* PWM, white = 200:128:96 */
	{ 200, 0, 0 }, { 200, 32, 0 }, { 200, 128, 0 },
	{ 0, 128, 0 }, { 0, 0, 96 }, { 120, 0, 96 },
};
/* (rb, the rainbow's state, is declared next to the IR ring indices:
 * there it fills their padding, 0 B) */
PRIVATE u8_t rb_user[2][3];		/* dual, dualsoft, morph: PWM */
FORWARD _PROTOTYPE( void rb_step, (void)				);
FORWARD _PROTOTYPE( int led_text, (phys_bytes user, int count)		);
FORWARD _PROTOTYPE( int deliver, (int proc, vir_bytes vir, int count)	);
FORWARD _PROTOTYPE( void do_hardint, (void)				);
FORWARD _PROTOTYPE( int fg_preempt, (int proc)				);
FORWARD _PROTOTYPE( void fg_release, (void)				);
FORWARD _PROTOTYPE( int do_select, (message *m_ptr)			);
FORWARD _PROTOTYPE( int ow_run, (message *m_ptr)			);
FORWARD _PROTOTYPE( int ib_emu, (message *m_ptr)			);
FORWARD _PROTOTYPE( void ib_stop, (void)				);
FORWARD _PROTOTYPE( void ib_edge, (void)				);
FORWARD _PROTOTYPE( void gp_stop, (void)				);
FORWARD _PROTOTYPE( void nt_over, (void)				);
FORWARD _PROTOTYPE( void nt_box, (char *l1, char *l2)			);
FORWARD _PROTOTYPE( void nt_text, (char *s, int y)			);
FORWARD _PROTOTYPE( void sel_check, (void)				);
FORWARD _PROTOTYPE( void sp_play, (void)				);
FORWARD _PROTOTYPE( void lc_newline, (void)				);
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
FORWARD _PROTOTYPE( int sg_tx_check, (u32_t ms, u32_t *hzp, u32_t *limit) );
FORWARD _PROTOTYPE( int sg_raw_write, (phys_bytes user, int count) );
FORWARD _PROTOTYPE( int sg_pkt_rw, (message *m_ptr, phys_bytes user, int count) );
FORWARD _PROTOTYPE( int sg_ioctl, (message *m_ptr) );
FORWARD _PROTOTYPE( int adcs_config, (phys_bytes user, int count) );
FORWARD _PROTOTYPE( int adcs_read, (message *m_ptr, phys_bytes user, int count) );
FORWARD _PROTOTYPE( void adcs_end, (int ok) );
/* the ADC stream (R6.4 part 2, see adcs_read) */
PRIVATE u32_t as_cfg;		/* rate << 4 | pin; 0: not set */
/* (sg_state, the Sub-GHz receiver: SG_ON listening, SG_READ read lately;
 * here, next to the stream's bytes, it costs no padding) */
PRIVATE u8_t sg_state;
PRIVATE u8_t as_proc;		/* the reader + 1, 0: none */
PRIVATE u8_t as_caller, as_done;
PRIVATE u16_t as_count;
FORWARD _PROTOTYPE( int fg_other, (int proc) );
FORWARD _PROTOTYPE( int fg_ioctl, (message *m_ptr) );
FORWARD _PROTOTYPE( void fg_hold, (unsigned now) );
FORWARD _PROTOTYPE( int ge_match, (unsigned now) );
FORWARD _PROTOTYPE( int ge_blink_on, (clock_t e) );
FORWARD _PROTOTYPE( void ge_hold, (void) );
FORWARD _PROTOTYPE( void ge_task, (void) );
FORWARD _PROTOTYPE( int bl_state, (u32_t st) );
FORWARD _PROTOTYPE( void ir_fill, (void)				);
FORWARD _PROTOTYPE( int ir_rx_arm, (void)				);
FORWARD _PROTOTYPE( int rfid_read, (message *m_ptr, phys_bytes user,
							int count)	);
FORWARD _PROTOTYPE( int pulse_read, (message *m_ptr, phys_bytes user,
						int count, int input)	);
FORWARD _PROTOTYPE( void ir_rx_end, (int r)				);
FORWARD _PROTOTYPE( unsigned rf_durations, (unsigned n)			);
FORWARD _PROTOTYPE( void rfid_tx_off, (void)				);
FORWARD _PROTOTYPE( void rfid_field_off, (void)				);
FORWARD _PROTOTYPE( int rfid_ioctl, (message *m_ptr)			);
FORWARD _PROTOTYPE( int rfid_emu_start, (message *m_ptr)			);
FORWARD _PROTOTYPE( void rfid_emu_off, (void)				);
FORWARD _PROTOTYPE( void ir_stop_tx, (void)				);

#include "hiddev.c"		/* USB HID, /dev/hidkbd... (R11.3.1) */

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
  sp_quiet = 100 - hal_vol_default();	/* K.7: the system's volume */
  {	/* the backlight as it was set before the reset, and on */
	u32_t st = hal_bl_state() & ~(HAL_BL_OFF | HAL_BL_STEPS | HAL_BL_MODES);

	hal_bl_state_set(st);
	hal_backlight(HAL_BL_LEVEL(st));
  }
  if ((r = hal_rtc_alarm_irq()) >= 0) {	/* R7b.1 */
	put_irq_handler(r, rtc_int);
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
		if (IS_HID(fm.DEVICE)) {
			r = hid_open(&fm);
			break;
		}
		if (fm.m_type == DEV_CLOSE && fm.DEVICE == SPK_MINOR
							&& !sp_waiting)
			sp_quiet = 100 - hal_vol_default();	/* the default
							 * again (K.7) */
		if (fm.m_type == DEV_CLOSE && fm.DEVICE == GPIO_MINOR
				&& gp_proc == fm.PROC_NR + 1) gp_stop();
		if (fm.m_type == DEV_CLOSE && fm.DEVICE == IBUTTON_MINOR
				&& ie.proc == fm.PROC_NR + 1) ib_stop();
		if (fm.m_type == DEV_CLOSE && fm.DEVICE == RFID_MINOR)
			rf_carrier = RFID_CARRIER_125;	/* R9.0.4 */
		if (fm.m_type == DEV_CLOSE && fm.DEVICE == RFID_MINOR) {
			if (rf_held) rfid_field_off();	/* the holder let go */
			if (rf_emu != 0) rfid_emu_off();
		}
		r = (unsigned) fm.DEVICE <= LAST_MINOR
				|| fm.DEVICE == SGPKT_MINOR ? OK : ENXIO;
#if LATTRACE
		if (fm.DEVICE == LAT_MINOR) r = OK;
#endif
		break;
	case DEV_READ:
	case DEV_WRITE:
		r = do_rw(&fm);
		break;
	case DEV_IOCTL:
		r = IS_HID(fm.DEVICE) ? hid_ioctl(&fm)
		  : fm.DEVICE == RTC_MINOR ? rtc_ioctl(&fm)
		  : fm.DEVICE == RFID_MINOR ? rfid_ioctl(&fm)
		  : fm.DEVICE == IBUTTON_MINOR ? ow_run(&fm)
		  : fm.DEVICE == SUBGHZ_MINOR || fm.DEVICE == SGRAW_MINOR
		    || fm.DEVICE == SGPKT_MINOR ? sg_ioctl(&fm)
		  : fg_ioctl(&fm);
		break;
	case DEV_SELECT:
		r = do_select(&fm);
		break;
	case CANCEL:
		if (rd_waiting && rd_proc == fm.PROC_NR) rd_waiting = FALSE;
		if (hid_rdp == fm.PROC_NR + 1) hid_rdp = 0;
		if (pre_rd == fm.PROC_NR + 1) pre_rd = 0;	/* R7b.2 */
		if (ir_rd && ir_proc == fm.PROC_NR) ir_rx_end(1);
		if (tx_on && tx_proc == fm.PROC_NR) {	/* stop sending */
			ir_stop_tx();
			tx_on = FALSE;
			if (tx_rfid && rf_held) {	/* the whole session */
				rfid_field_off();
				tx_rfid = FALSE;
			} else if (tx_rfid) rfid_tx_off();
		}
		if (rf_emu == fm.PROC_NR + 1) rfid_emu_off();	/* R6.5 p4 */
		if (as_proc == fm.PROC_NR + 1) adcs_end(FALSE);	/* R4b.5 */
		if (sp_waiting && sp_proc == fm.PROC_NR) {	/* stop it */
			sp_waiting = FALSE;
			note_i = note_n;
			hal_tone(0, 0);
		}
		r = EINTR;
		break;
	default:
		r = EINVAL;
	}
	reply(TASK_REPLY, fm.m_source, fm.PROC_NR, r);
	if (sel_wait) sel_check();	/* e.g. a tune cancelled */
	hid_check();			/* e.g. a persona let go */
  }
}


/*===========================================================================*
 *				gp_stop					     *
 *===========================================================================*/
PRIVATE void gp_stop()
{
/* R6.4: no pin is watched any more (the watcher stopped, closed the
 * device or exited): the tick stops sampling, Stop2 is allowed again. */
  lock();
  gp_watch = gp_chg = 0;
  gp_proc = 0;
  unlock();
  pm_busy &= ~PM_GPIO;
}


/*===========================================================================*
 *				do_select				     *
 *===========================================================================*/
PRIVATE int do_select(m_ptr)
message *m_ptr;
{
/* DEV_SELECT (R4b.6): the ops of COUNT ready now; with SEL_NOTIFY remember
 * the buttons' and the speaker's that are not; ops 0: forget them.  One
 * registration per minor for all pollers (FS keeps who they are).
 */
  int ops = m_ptr->COUNT, want = ops & (SEL_RD | SEL_WR), r = want;

  if (IS_HID(m_ptr->DEVICE)) return(hid_select(m_ptr));
  if (ops == 0) {
	if (m_ptr->DEVICE == BTN_MINOR) sel_wait &= ~SEL_RD;
	if (m_ptr->DEVICE == SPK_MINOR) sel_wait &= ~SEL_WR;
	if (m_ptr->DEVICE == GPIO_MINOR) sel_wait &= ~SEL_GPIO;
	return(0);
  }
  switch (m_ptr->DEVICE) {
  case BTN_MINOR:		/* a write is the QEMU tests' fake: ready */
	if (ev_head == ev_tail || fg_other(m_ptr->PROC_NR)) r &= ~SEL_RD;
	if ((ops & SEL_NOTIFY) && (want & ~r & SEL_RD)) {
		if (!(sel_wait & SEL_RD)) sel_head = (u8_t) ev_head;
		sel_wait |= SEL_RD;
	}
	break;
  case GPIO_MINOR:		/* R6.4: with pins watched, an edge */
	if (gp_watch != 0 && gp_chg == 0) r &= ~SEL_RD;
	if ((ops & SEL_NOTIFY) && (want & ~r & SEL_RD)) sel_wait |= SEL_GPIO;
	break;
  case SPK_MINOR:		/* a read fails at once: ready */
	if (sp_waiting) r &= ~SEL_WR;
	if ((ops & SEL_NOTIFY) && (want & ~r & SEL_WR)) sel_wait |= SEL_WR;
	break;
  default:
#if LATTRACE
	if (m_ptr->DEVICE == LAT_MINOR) break;
#endif
	if ((unsigned) m_ptr->DEVICE > LAST_MINOR) return(ENXIO);
  }
  return(r);
}


/*===========================================================================*
 *				sel_check				     *
 *===========================================================================*/
PRIVATE void sel_check()
{
/* In the task: what FS waits for has happened, tell it once (DEV_SELECTED,
 * DEVICE where a reply has REP_PROC_NR).  The pollers that still wait
 * register again. */
  if ((sel_wait & SEL_RD) && (u8_t) ev_head != sel_head) {
	sel_wait &= ~SEL_RD;
	reply(DEV_SELECTED, FS_PROC_NR, BTN_MINOR, SEL_RD);
  }
  if ((sel_wait & SEL_GPIO) && gp_chg != 0) {
	sel_wait &= ~SEL_GPIO;
	reply(DEV_SELECTED, FS_PROC_NR, GPIO_MINOR, SEL_RD);
  }
  if ((sel_wait & SEL_WR) && !sp_waiting) {
	sel_wait &= ~SEL_WR;
	reply(DEV_SELECTED, FS_PROC_NR, SPK_MINOR, SEL_WR);
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
#define SG_ON		1
#define SG_READ		2
#define SG_PEAK_MS	30
#define SG_GAP_MS	100		/* silence that ends a raw frame */

PUBLIC void subghz_minute()
{
/* The clock task, once a minute (pm_minute): a radio left listening with
 * nobody reading goes back to sleep. */
  if (ir_rd && pu_input == 1) return;	/* a raw reader waits */
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
  int rssi = HAL_SUBGHZ_OFF, n = 0, peak, r;
  u32_t t0;

  /* not listening: leave the chip alone (CS low wakes it from sleep and
   * nothing would put it back) */
  hz = 0;
  if (sg_state & SG_ON) rssi = hal_subghz_rssi(&hz);
  t0 = hal_cycles();
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
 *				Sub-GHz transmit (R8.2)			     *
 *===========================================================================*/
/* Every transmit (a frame on /dev/subghzraw, a packet on /dev/subghzpkt)
 * passes here first: the frequency is read back from the chip (not taken
 * from what was asked), it must lie in a band of the region set at boot,
 * and the band's duty cycle over the hour must allow the frame's air time.
 * The table is const (flash).  Bands as stock's region data for the three
 * regions; the EU duty cycles are ETSI EN 300 220's (as libmodem's P.4
 * table).  RAM: the region (1 B) and the hour's air time (8 B). */
#define SG_HOUR_S	3600
PRIVATE const struct sg_band {
  char rg[2];
  u16_t duty_pm;		/* per mille of the hour; 1000: no limit */
  u32_t lo, hi;			/* Hz */
} sg_bands[] = {
  { "EU", 100,  433050000, 434790000 },		/* 10 % */
  { "EU",  10,  868000000, 868600000 },		/* 1 % */
  { "EU",   1,  868700000, 869200000 },		/* 0.1 % */
  { "EU", 100,  869400000, 869650000 },		/* 10 % */
  { "EU", 1000, 869700000, 870000000 },
  { "US", 500,  304100000, 321950000 },		/* stock: 50 % */
  { "US", 500,  433050000, 434790000 },
  { "US", 500,  915000000, 928000000 },
  { "JP", 500,  312000000, 315250000 },
  { "JP", 500,  920500000, 923500000 },
};
#define SG_NBANDS	(sizeof(sg_bands) / sizeof(sg_bands[0]))
PRIVATE u16_t sg_region;		/* SG_REGION('E','U'); 0: none set */
PRIVATE u32_t sg_air_ms, sg_hour;	/* air time since sg_hour (s) */

PRIVATE int sg_tx_check(ms, hzp, limit)
u32_t ms;				/* the frame's air time */
u32_t *hzp;				/* out: the frequency */
u32_t *limit;				/* out: the band's ms per hour */
{
  unsigned i;
  u32_t hz, now = (u32_t) (get_uptime() / HZ);

  if (!(sg_state & SG_ON) || hal_subghz_rssi(&hz) == HAL_SUBGHZ_OFF)
	return(EIO);			/* not tuned (R8.1: read back) */
  if (sg_region == 0) return(EPERM);
  for (i = 0; i < SG_NBANDS; i++) {
	if (SG_REGION(sg_bands[i].rg[0], sg_bands[i].rg[1]) == sg_region
			&& hz >= sg_bands[i].lo && hz <= sg_bands[i].hi) break;
  }
  if (i == SG_NBANDS) return(EPERM);
  if (now - sg_hour >= SG_HOUR_S) {		/* a new hour */
	sg_hour = now;
	sg_air_ms = 0;
  }
  *hzp = hz;
  *limit = (u32_t) sg_bands[i].duty_pm * SG_HOUR_S;
  if (sg_air_ms + ms > *limit) return(EAGAIN);
  return(OK);
}

PRIVATE int sg_raw_write(user, count)
phys_bytes user;
int count;
{
/* One frame: durations in us, carrier first.  Checked a few words at a
 * time on the stack, then sent from the writer's memory. */
  u32_t d[16], us = 0, hz, limit;
  int i, j, k, n, r;

  if (count % 4 != 0 || count > 0x7FFC) return(EINVAL);
  n = count / 4;
  if (hal_subghz_fake_take(HAL_SG_FAKE_RAW)) {
	/* QEMU test hook: a received frame for the waiting reader */
	if (!ir_rd || pu_input != 1) return(EAGAIN);
	if (count > (int) ir_count) count = ir_count;
	phys_copy(user, ir_buf, (phys_bytes) count);
	hal_pulse_stop();
	lock();
	pm_busy &= ~PM_PULSE;
	unlock();
	proc_addr(ir_proc)->p_physio = FALSE;
	ir_rd = FALSE;
	reply(REVIVE, ir_caller, ir_proc, count);
	return(count);
  }
  for (i = 0; i < n; i += k) {
	k = n - i < 16 ? n - i : 16;
	phys_copy(user + (phys_bytes) i * 4, vir2phys(d), (phys_bytes) k * 4);
	for (j = 0; j < k; j++) {
		if (d[j] == 0 || d[j] > SG_DURATION_MAX) return(EINVAL);
		us += d[j];
	}
  }
  if (ir_rd || tx_on) return(EBUSY);	/* one radio: a reader listens */
  if ((r = sg_tx_check((us + 999) / 1000, &hz, &limit)) != OK) return(r);
  if (hal_subghz_tx(hz, (u32_t) user, (unsigned) n) < 0) return(ENOTSUP);
  sg_air_ms += (us + 999) / 1000;
  return(count);
}

PRIVATE int sg_pkt_rw(m_ptr, user, count)
message *m_ptr;
phys_bytes user;
int count;
{
/* /dev/subghzpkt: one packet.  The air time as stock chat's preset sends
 * it (GFSK 9.99 kBd; preamble, sync, length and CRC: 12 bytes more). */
  u8_t p[HAL_SUBGHZ_PKT];
  u32_t hz, limit, ms;
  int r;

  if (m_ptr->m_type == DEV_READ) {
	if (!(sg_state & SG_ON)) return(EIO);
	if (count > HAL_SUBGHZ_PKT) count = HAL_SUBGHZ_PKT;
	sg_state |= SG_READ;
	if ((r = hal_subghz_pkt_rx(p, (unsigned) count)) < 0) return(ENOTSUP);
	if (r == 0) return(EAGAIN);	/* none: poll again later */
	phys_copy(vir2phys(p), user, (phys_bytes) r);
	return(r);
  }
  if (count > HAL_SUBGHZ_PKT) return(EINVAL);
  phys_copy(user, vir2phys(p), (phys_bytes) count);
  if (hal_subghz_fake_take(HAL_SG_FAKE_PKT))	/* QEMU: a received one */
	return(hal_subghz_pkt_fake(p, (unsigned) count) < 0 ? EINVAL : count);
  ms = ((u32_t) count + 12) * 8 * 1000 / 9990 + 1;
  if ((r = sg_tx_check(ms, &hz, &limit)) != OK) return(r);
  if (hal_subghz_pkt_tx(hz, p, (unsigned) count) < 0) return(ENOTSUP);
  sg_air_ms += ms;
  return(count);
}

PRIVATE int sg_ioctl(m_ptr)
message *m_ptr;
{
/* <minix/subghz.h>: the region, the hour's air time, the QEMU hooks. */
  phys_bytes user;
  struct sg_air a;
  u32_t hz, limit, st;
  int v, size, r;
  unsigned i;

  switch (m_ptr->TTY_REQUEST) {
  case SGIOCREGION: case SGIOCGREGION: case SGIOCFAKE: size = sizeof(int); break;
  case SGIOCLINE: size = sizeof(long); break;
  case SGIOCAIR: size = sizeof(a); break;
  default: return(ENOTTY);
  }
  user = numap(m_ptr->PROC_NR, (vir_bytes) m_ptr->ADDRESS, (vir_bytes) size);
  if (user == 0) return(EFAULT);
  switch (m_ptr->TTY_REQUEST) {
  case SGIOCREGION:
	phys_copy(user, vir2phys(&v), (phys_bytes) size);
	for (i = 0; i < SG_NBANDS; i++)
		if (SG_REGION(sg_bands[i].rg[0], sg_bands[i].rg[1]) == v) break;
	if (i == SG_NBANDS) return(EINVAL);
	if (sg_region != 0 && sg_region != (u16_t) v) return(EPERM);
	sg_region = (u16_t) v;		/* once: until the next boot */
	return(OK);
  case SGIOCGREGION:
	v = sg_region;
	break;
  case SGIOCAIR:
	r = sg_tx_check(0, &hz, &limit);
	if (r != OK && r != EAGAIN) return(r);
	a.used_ms = sg_air_ms;
	a.limit_ms = limit;
	phys_copy(vir2phys(&a), user, (phys_bytes) size);
	return(OK);
  case SGIOCFAKE:
	phys_copy(user, vir2phys(&v), (phys_bytes) size);
	if (v != HAL_SG_FAKE_RAW && v != HAL_SG_FAKE_PKT) return(EINVAL);
	if ((m_ptr->DEVICE == SGRAW_MINOR) != (v == HAL_SG_FAKE_RAW))
		return(EINVAL);		/* the file it is for */
	if (hal_subghz_fake(v) < 0) return(ENOTTY);
	return(OK);
  case SGIOCLINE: {
	long h;

	if (hal_subghz_line(&st) < 0) return(ENOTTY);
	h = (long) st;
	phys_copy(vir2phys(&h), user, (phys_bytes) size);
	return(OK);
  }
  }
  phys_copy(vir2phys(&v), user, (phys_bytes) size);
  return(OK);
}


/*===========================================================================*
 *				ADC stream (R6.4 part 2)		     *
 *===========================================================================*/
/* /dev/adcs: write "PIN RATE" (header pin number as gpio(1) counts them,
 * 0..7, left analog; 100..100000 Hz), then a read of n bytes gets n/2
 * 12-bit samples, by DMA straight into the reader's buffer (R4b.5: the
 * process is pinned meanwhile, p_physio, and a signal stops the DMA
 * before its memory can go).  The ms timer looks whether it is done; no
 * interrupt.  TIM2 is shared with the IR pulse engine: one or the other. */

PRIVATE int adcs_config(user, count)
phys_bytes user;
int count;
{
  char t[16];
  u32_t pin = 0, rate = 0;
  int i = 0;

  if (count >= (int) sizeof(t)) return(EINVAL);
  phys_copy(user, vir2phys(t), (phys_bytes) count);
  t[count] = 0;
  while (t[i] >= '0' && t[i] <= '9') pin = pin * 10 + (t[i++] - '0');
  if (t[i++] != ' ') return(EINVAL);
  while (t[i] >= '0' && t[i] <= '9') rate = rate * 10 + (t[i++] - '0');
  if (pin >= HAL_GPIO_PINS || rate < 100 || rate > 100000) return(EINVAL);
  as_cfg = rate << 4 | pin;
  return(count);
}

PRIVATE int adcs_read(m_ptr, user, count)
message *m_ptr;
phys_bytes user;
int count;
{
  unsigned ms;

  if (as_cfg == 0 || count < 2 || count > 8192 || (count & 1)) return(EINVAL);
  if (as_proc != 0 || tx_on || ir_rd) return(EBUSY);	/* TIM2 */
  if (hal_adc_stream(as_cfg & 15, as_cfg >> 4, user, (unsigned) count / 2)
							< 0) return(EIO);
  proc_addr(m_ptr->PROC_NR)->p_physio = TRUE;	/* no shadow swap now */
  as_proc = m_ptr->PROC_NR + 1;
  as_caller = m_ptr->m_source;
  as_count = count;
  as_done = FALSE;
  ms = (unsigned) ((u32_t) count / 2 * 1000 / (as_cfg >> 4)) + 2;
  lock();
  pm_busy |= PM_ADC;			/* TIM2, ADC and DMA need the clocks */
  mt_arm(MT_ADC, ms > 999 ? 999 : ms);
  unlock();
  return(SUSPEND);
}

PUBLIC void flipdev_adc_poll()
{
/* The ms timer (interrupt): the stream's DMA is done, or look again. */
  if (as_proc == 0) return;
  if (hal_adc_stream_left() == 0) {
	as_done = TRUE;
	interrupt(PRINTER);
  } else mt_arm(MT_ADC, 10);
}

PRIVATE void adcs_end(ok)
int ok;			/* done (reply), else cancelled */
{
  int proc = as_proc - 1;

  hal_adc_stream_stop();		/* the DMA first, then the memory */
  lock();
  mt_arm(MT_ADC, 0);
  pm_busy &= ~PM_ADC;
  unlock();
  proc_addr(proc)->p_physio = FALSE;
  as_proc = 0;
  if (ok) reply(REVIVE, as_caller, proc, as_count);
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
 *				ib_emu					     *
 *===========================================================================*/
PRIVATE int ib_emu(m_ptr)
message *m_ptr;
{
/* IBIOCEMU (R6.7 part 3): be the Dallas key q.rom for a reader on the
 * contact (on 1), or stop (on 0); the caller's close stops it too.  Asked
 * again while on: a new ROM takes over, and q.served says how many times
 * a reader got the whole ROM.  Stop2 waits meanwhile: waking from it
 * takes longer than a 1-Wire slot. */
  struct ib_emu q;
  phys_bytes user;
  int i;

  user = numap(m_ptr->PROC_NR, (vir_bytes) m_ptr->ADDRESS,
						(vir_bytes) sizeof(q));
  if (user == 0) return(EFAULT);
  phys_copy(user, vir2phys(&q), (phys_bytes) sizeof(q));
  if (ie.st != IE_OFF && ie.proc != m_ptr->PROC_NR + 1) return(EBUSY);
  if (!q.on) {
	if (ie.st != IE_OFF) ib_stop();
  } else {
	lock();				/* the edges read rom[] */
	for (i = 0; i < 8; i++) ie.rom[i] = q.rom[i];
	if (ie.st == IE_OFF) {
		ie.st = IE_IDLE;
		ie.low = ie.served = 0;
		ie.proc = m_ptr->PROC_NR + 1;
		pm_busy |= PM_IBUTTON;
		hal_ib_emu(1);
	}
	unlock();
  }
  q.served = ie.served;
  phys_copy(vir2phys(&q), user, (phys_bytes) sizeof(q));
  return(OK);
}

PRIVATE void ib_stop()
{
  lock();
  hal_ib_emu(0);
  ie.st = IE_OFF;
  ie.proc = 0;
  pm_busy &= ~PM_IBUTTON;
  unlock();
  hal_ow_line(0);			/* the contact released */
}


/*===========================================================================*
 *				ib_edge					     *
 *===========================================================================*/
PRIVATE void ib_edge()
{
/* An edge on the contact while a key is emulated (interrupt, from
 * btn_int): ie_step() decides, this drives the line.  The only waits are
 * the 1-Wire times themselves (devguide 3.5: bounded microsecond timing):
 * a 0 bit held until 30 us after the reader's fall, the presence pulse
 * 20 + 120 us.  The edges of our own presence pulse are forgotten; the
 * end of a held 0 is the slot's rising edge and is taken as such. */
  int low = !hal_ow_read(), t;
  unsigned us = 0;

  if (low) ie.fall = hal_ow_now();
  else us = (u16_t) (hal_ow_now() - ie.fall);
  switch (ie_step(&ie, !low, us)) {
  case IE_HOLD0:
	hal_ow_line(1);
	t = IE_HOLD_US - (int) (u16_t) (hal_ow_now() - ie.fall);
	if (t > 0) hal_ow_delay((unsigned) t);
	hal_ow_line(0);
	break;
  case IE_PRESENCE:
	hal_ow_delay(IE_PRES_WAIT_US);
	hal_ow_line(1);
	hal_ow_delay(IE_PRES_US);
	hal_ow_line(0);
	hal_ow_delay(2);		/* the pull-up's rise */
	hal_ib_clear();
	break;
  }
}


/*===========================================================================*
 *				ow_run					     *
 *===========================================================================*/
PRIVATE int ow_run(m_ptr)
message *m_ptr;
{
/* IBIOCRUN (R6.7 part 2): 1-Wire master steps for a program (an RW1990
 * blank written by ikey).  The struct is on the task's stack (no static
 * RAM); each bit slot masks interrupts as ow_bit does; pauses are the
 * caller's.  At most IB_MAXOP steps a call (a few ms). */
  struct ib_run q;
  phys_bytes user;
  int i, j, v;

  if (m_ptr->TTY_REQUEST == IBIOCEMU) return(ib_emu(m_ptr));
  if (m_ptr->TTY_REQUEST != IBIOCRUN) return(ENOTTY);
  if (ie.st != IE_OFF) return(EBUSY);	/* the contact is a key now */
  user = numap(m_ptr->PROC_NR, (vir_bytes) m_ptr->ADDRESS,
						(vir_bytes) sizeof(q));
  if (user == 0) return(EFAULT);
  phys_copy(user, vir2phys(&q), (phys_bytes) sizeof(q));
  if (q.nop > IB_MAXOP) return(EINVAL);
  q.nout = 0;
  for (i = 0; i < q.nop; i++) {
	switch (q.op[i]) {
	case IB_W0:
	case IB_W1:
		(void) ow_bit(q.op[i]);
		break;
	case IB_RESET:
		if (q.nout >= IB_MAXOUT) return(EINVAL);
		q.out[q.nout++] = ow_reset();
		break;
	case IB_WBYTE:
		if (++i >= q.nop) return(EINVAL);
		for (j = 0; j < 8; j++) (void) ow_bit(q.op[i] >> j & 1);
		break;
	case IB_RBYTE:
		if (q.nout >= IB_MAXOUT) return(EINVAL);
		for (v = j = 0; j < 8; j++) v |= ow_bit(1) << j;
		q.out[q.nout++] = (unsigned char) v;
		break;
	default:
		return(EINVAL);
	}
  }
  phys_copy(vir2phys(&q), user, (phys_bytes) sizeof(q));
  return(OK);
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
  char line[176], d[10];
  const char *k = pw_key;
  unsigned long u;
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
  for (k = " otg=0"; *k != 0; k++) line[n++] = *k;
  if (r3 & 0x20) line[n - 1] = '1';
  /* R6.8: seconds up and in Stop2 since boot, and the wake-ups from it
   * (a button; anything else): energy runs need no trace build */
  for (i = 0; i < 4; i++) {
	for (k = i == 0 ? " up=" : i == 1 ? " stop2=" : i == 2 ? " wbtn="
						: " wother="; *k != 0; k++)
		line[n++] = *k;
	u = i == 0 ? (unsigned long) get_uptime() / HZ
	  : i == 1 ? (unsigned long) pm_stat.min * 60 + pm_stat.stop / 32768
	  : i == 2 ? pm_stat.btn : pm_stat.other;
	j = 0;
	do d[j++] = '0' + u % 10; while ((u /= 10) != 0);
	while (j > 0) line[n++] = d[--j];
  }
  line[n++] = '\n';
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
  u8_t buf[4];

  if (count <= 0) return(EINVAL);
  user = m_ptr->m_type == DEV_WRITE	/* a source may be constant */
	? numap_src(m_ptr->PROC_NR, (vir_bytes) m_ptr->ADDRESS, count)
	: numap(m_ptr->PROC_NR, (vir_bytes) m_ptr->ADDRESS, count);
  if (user == 0) return(EFAULT);
  if (IS_HID(m_ptr->DEVICE)) return(hid_rw(m_ptr, user, count));

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
		if (nt_st & NT_KIND) nt_over();		/* N.6 */
		if (hal_lcd_flush(fbuf) < 0) lc_dirty = TRUE;	/* SD: later */
	}
	return(count);

  case BTN_MINOR:
	if (m_ptr->m_type == DEV_WRITE) {	/* QEMU tests: fake buttons */
		phys_copy(user, vir2phys(buf), (phys_bytes) 1);
		return(count == 1 && hal_buttons_fake(buf[0] & BTN_ALL) == 0
							? 1 : EINVAL);
	}
	if (m_ptr->m_type != DEV_READ) return(EINVAL);
	if (count < 2) return(EINVAL);
	if (fg_other(m_ptr->PROC_NR)) {	/* R4b.8: not yours: wait (until
					 * a signal; the keys are not kept) */
		if (m_ptr->TTY_FLAGS & O_NONBLOCK) return(EAGAIN);
		/* R7b.2: while an alert holds them, one reader is parked:
		 * its read ends empty at the release, so it redraws and
		 * reads again */
		if (fg_pre != 0 && pre_rd == 0) pre_rd = m_ptr->PROC_NR + 1;
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
	if (tx_on) return(EBUSY);		/* sbuf holds the IR ring */
	if (count > (int) sizeof(nb)) count = sizeof(nb);
	phys_copy(user, vir2phys(nb), (phys_bytes) count);
	n = count / 4;
	for (i = 0; i < n; i++) {
		unsigned ms = nb[4*i+2] | nb[4*i+3] << 8;
		note_hz[i] = nb[4*i] | nb[4*i+1] << 8;
		if (note_hz[i] == SPK_VOLDEF) {	/* also the default */
			hal_vol_default_set(ms > 100 ? 100 : (int) ms);
			note_hz[i] = SPK_VOL;
		}
		if (note_hz[i] == SPK_VOL) {	/* the volume, kept as is */
			note_ticks[i] = ms > 100 ? 100 : ms;
			continue;
		}
		note_ticks[i] = (ms * HZ + 999) / 1000;
		if (note_ticks[i] == 0) note_ticks[i] = 1;
	}
	lock();
	note_n = n;
	note_i = 0;
	sp_play();
	unlock();
	if (note_i >= note_n) return(count);	/* only volume notes */
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
		/* the levels; then (R6.4) '1' for each pin that changed
		 * since the last read, which clears them */
		if (count > 2 * HAL_GPIO_PINS) count = 2 * HAL_GPIO_PINS;
		for (i = 0; i < HAL_GPIO_PINS; i++) {
			gb[i] = '0' + hal_gpio_read(i);
			gb[HAL_GPIO_PINS + i] = gp_chg & (1 << i) ? '1' : '0';
		}
		if (count > HAL_GPIO_PINS) gp_chg = 0;
		phys_copy(vir2phys(gb), user, (phys_bytes) count);
		return(count);
	}
	if (count % 2 != 0) return(EINVAL);
	if (count > (int) sizeof(gb)) count = sizeof(gb);
	phys_copy(user, vir2phys(gb), (phys_bytes) count);
	for (i = 0; i < count; i += 2) {
		unsigned pin = gb[i], o = gb[i+1];

		if (pin >= HAL_GPIO_PINS) return(i > 0 ? i : EINVAL);
		if (o == 6 || o == 7) {		/* R6.4: watch edges, stop */
			if (gp_proc != 0 && gp_proc != m_ptr->PROC_NR + 1
			    && gp_watch != 0) return(i > 0 ? i : EBUSY);
			lock();
			if (o == 6) {
				gp_watch |= 1 << pin;
				gp_last = (gp_last & ~(1 << pin))
					| (hal_gpio_read(pin) == 1) << pin;
				gp_proc = m_ptr->PROC_NR + 1;
			} else gp_watch &= ~(1 << pin);
			gp_chg &= ~(1 << pin);
			unlock();
			if (gp_watch) pm_busy |= PM_GPIO;
			else gp_stop();
			continue;
		}
		if (hal_gpio(pin, o) < 0) return(i > 0 ? i : EINVAL);
	}
	return(count);
  }

  case SUBGHZ_MINOR:
	if (m_ptr->m_type == DEV_WRITE) return(subghz_write(user, count));
	if (m_ptr->POSITION != 0) return(0);	/* one line, then EOF */
	return(subghz_read(user, count));

  case ADCS_MINOR:
	if (m_ptr->m_type == DEV_WRITE) return(adcs_config(user, count));
	return(adcs_read(m_ptr, user, count));

  case ADC_MINOR:
	if (m_ptr->m_type != DEV_READ) return(EINVAL);
	if (m_ptr->POSITION != 0) return(0);	/* one line, then EOF */
	if (as_proc != 0) return(EBUSY);	/* the ADC is a stream's: a
						 * reading would stop it */
	return(adc_read(user, count));

  case IBUTTON_MINOR:
	if (m_ptr->m_type != DEV_READ) return(EINVAL);
	if (m_ptr->POSITION != 0) return(0);	/* one ROM, then EOF */
	if (count < 8) return(EINVAL);
	if (ie.st != IE_OFF) return(EBUSY);	/* the contact is a key now */
	return(ow_read_rom(user));

  case POWER_MINOR:
	if (m_ptr->m_type == DEV_WRITE) return(power_write(user, count));
	if (m_ptr->POSITION != 0) return(0);	/* one line, then EOF */
	return(power_read(user, count));

  case RFID_MINOR:
	if (m_ptr->m_type == DEV_WRITE) {	/* field on/off times (T5577) */
		if (ir_rd || tx_on) return(EBUSY);
		int rr;

		if (rf_held) hal_rfid_pull(1);	/* a command: PA2 high (the
					 * capture left it low) */
		tx_rfid = TRUE;
		rr = ir_write(m_ptr, user, count);
		if (rr != SUSPEND) tx_rfid = FALSE;
		return(rr);
	}
	return(rfid_read(m_ptr, user, count));

  case SGPKT_MINOR:
	return(sg_pkt_rw(m_ptr, user, count));

  case SGRAW_MINOR:
	/* R4b.7 part 2: what the receiver demodulates, as /dev/ir's
	 * durations, high (carrier) first; tune with /dev/subghz first.
	 * A write sends such a frame (R8.2). */
	if (m_ptr->m_type == DEV_WRITE) return(sg_raw_write(user, count));
	if (!(sg_state & SG_ON)) return(EIO);	/* not listening */
	sg_state |= SG_READ;			/* in use: it stays on */
	return(pulse_read(m_ptr, user, count, 1));

  case IR_MINOR:
	if (m_ptr->m_type == DEV_WRITE) return(ir_write(m_ptr, user, count));
	return(pulse_read(m_ptr, user, count, 0));

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
	if (m_ptr->DEVICE == LED_MINOR) {
		lock();				/* a new colour: no rainbow */
		rb = 0;
		mt_arm(MT_LED, 0);
		unlock();
		if (count != 3) return(led_text(user, count));
	}
	if (count > 4) count = 4;
	if (count == 4 && m_ptr->DEVICE != BL_MINOR) return(EINVAL);
	phys_copy(user, vir2phys(buf), (phys_bytes) count);
	if (m_ptr->DEVICE == BL_MINOR) {
		/* brightness; 3 bytes: and the idle timeout in seconds
		 * (little endian, 0: never, at most 16383); 4: and the forced
		 * mode (R6.10: 0 auto, 1 off, 2 on).  Kept over resets, the
		 * mode until the next boot. */
		u32_t old = hal_bl_state(), st = old & 0xFFFF;

		if (count >= 3) {
			st = buf[1] | (u32_t) buf[2] << 8;
			if (st > 0x3FFF) st = 0x3FFF;
			st |= old & HAL_BL_MODES;
		} else if (count != 1) return(EINVAL);
		if (count == 4) {
			if (buf[3] > 2) return(EINVAL);
			st = (st & ~HAL_BL_MODES) | (u32_t) buf[3] << 14;
		}
		hal_bl_state_set((u32_t) buf[0] << 16 | st);	/* on */
		hal_backlight(buf[0]);
		k_input = get_uptime();		/* the timeout starts now */
	}
	else if (count == 3) {
		if (last_buttons & GE_SAVED) {	/* a gesture blinks: later */
			ge_led[0] = buf[0]; ge_led[1] = buf[1]; ge_led[2] = buf[2];
		} else hal_led(buf[0], buf[1], buf[2]);
	}
	else return(EINVAL);
	return(count);
  }
  return(ENXIO);
}


/*===========================================================================*
 *				rtc_ioctl				     *
 *===========================================================================*/
PRIVATE int rtc_ioctl(m_ptr)
message *m_ptr;
{
/* /dev/rtc (R7b.1, <minix/rtc.h>): the alarm and the calibration. */
  struct rtc_alarm a;
  phys_bytes user;
  unsigned lead, flags;
  int v, size, r = OK;

  switch (m_ptr->TTY_REQUEST) {
  case RTCIOCSALARM: case RTCIOCGALARM: size = sizeof(a); break;
  case RTCIOCSCAL: case RTCIOCGCAL: size = sizeof(int); break;
  default: return(ENOTTY);
  }
  if (!hal_rtc_ok()) return(ENXIO);
  user = numap(m_ptr->PROC_NR, (vir_bytes) m_ptr->ADDRESS, (vir_bytes) size);
  if (user == 0) return(EFAULT);
  switch (m_ptr->TTY_REQUEST) {
  case RTCIOCSALARM:
	phys_copy(user, vir2phys(&a), (phys_bytes) size);
	if (hal_rtc_alarm((u32_t) a.due, (unsigned) a.lead) < 0) r = EINVAL;
	break;
  case RTCIOCGALARM:
	a.due = hal_rtc_alarm_get(&lead, &flags);
	a.lead = lead;
	a.flags = flags;		/* HAL_RTC_* are RTC_* */
	phys_copy(vir2phys(&a), user, (phys_bytes) size);
	break;
  case RTCIOCSCAL:
	phys_copy(user, vir2phys(&v), (phys_bytes) size);
	if (v < -511 || v > 512) return(EINVAL);
	(void) hal_rtc_cal(1, v);
	break;
  case RTCIOCGCAL:
	v = hal_rtc_cal(0, 0);
	phys_copy(vir2phys(&v), user, (phys_bytes) size);
	break;
  }
  return(r);
}


/*===========================================================================*
 *				rb_step					     *
 *===========================================================================*/
PRIVATE void rb_step()
{
/* The sequence's next step (task work: the I2C writes).  The glide is
 * linear in PWM level, which the LP5562 shows on a logarithmic scale. */
  unsigned m, c, n, k, g, i, v[3];
  const u8_t *from, *to;

  lock();
  if (!(rb & RB_ON)) { rb = 0; unlock(); return; }
  m = RB_MODE(rb);
  c = RB_COLOUR(rb);
  k = RB_STEPOF(rb) + 1;
  g = rb_mode[m].glide;
  if (k > g) {				/* held: glide to the next colour */
	k = 1;
	if (++c == rb_mode[m].ncol) c = 0;
  }
  rb = RB_ON | m << 12 | c << 8 | k;
  mt_arm(MT_LED, k == g && rb_mode[m].hold ? rb_mode[m].hold : RB_MS);
  unlock();
  n = c == 0 ? rb_mode[m].ncol - 1 : c - 1;	/* from the one before */
  from = m == 0 ? rb_col[n] : rb_user[n];
  to = m == 0 ? rb_col[c] : rb_user[c];
  for (i = 0; i < 3; i++)
	v[i] = (from[i] * (g - k) + to[i] * k) / g;
  hal_led_pwm(v[0], v[1], v[2]);
}


/*===========================================================================*
 *				rtc_int					     *
 *===========================================================================*/
PRIVATE int rtc_int(irq)
int irq;
{
/* RTC Alarm A (also out of Stop2): INIT gets SIGRTC from the task. */
  if (hal_rtc_alarm_ack()) {
	rtc_fired = TRUE;
	interrupt(PRINTER);
  }
  return(1);			/* reenable the interrupt */
}


/*===========================================================================*
 *				flipdev_led_tick			     *
 *===========================================================================*/
PUBLIC void flipdev_led_tick()
{
/* The ms timer: the rainbow's next step is due (R7b.5). */
  if (rb & RB_ON) {
	rb |= RB_DUE;
	interrupt(PRINTER);
  }
}


/*===========================================================================*
 *				led_text				     *
 *===========================================================================*/
PRIVATE int led_streq(a, b)
const char *a, *b;
{
  while (*a == *b && *a != 0) a++, b++;
  return(*a == *b);
}

PRIVATE int led_colour(s, rgb)
char *s;
unsigned long *rgb;
{
/* A colour name or #rrggbb, as 0xRRGGBB; -1: neither. */
  static const char cname[7][8] = { "blue", "green", "cyan", "red",
				    "magenta", "yellow", "white" };
  int c, i, d;

  *rgb = 0;
  if (*s == '#') {
	for (i = 1; i <= 6; i++) {
		c = s[i] | 0x20;		/* lower case */
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else return(-1);
		*rgb = *rgb << 4 | d;
	}
	return(s[7] == 0 ? 0 : -1);
  }
  for (c = 0; c < 7 && !led_streq(s, cname[c]); c++) ;
  if (c++ == 7) return(-1);		/* bit 2 red, 1 green, 0 blue */
  *rgb = (c & 4 ? 0xFF0000L : 0) | (c & 2 ? 0xFF00L : 0) | (c & 1 ? 0xFFL : 0);
  return(0);
}

PRIVATE int led_text(user, count)
phys_bytes user;
int count;
{
/* R7b.5: "PATTERN [COLOUR [COLOUR]]" or "off", one line ("echo blink red
 * >/dev/led", "echo morph #ff8000 blue >/dev/led").  Three bytes stay
 * the binary R, G, B. */
  static const u8_t white[3] = { 200, 128, 96 };
  char line[40], *wd[4];
  unsigned long rgb[2];
  int n, nw, m, i, j;

  n = count < (int) sizeof(line) - 1 ? count : (int) sizeof(line) - 1;
  phys_copy(user, vir2phys(line), (phys_bytes) n);
  line[n] = 0;
  for (nw = 0, i = 0; nw < 4; nw++) {	/* words */
	while (line[i] == ' ' || line[i] == '\n') line[i++] = 0;
	if (line[i] == 0) break;
	wd[nw] = &line[i];
	while (line[i] != ' ' && line[i] != '\n' && line[i] != 0) i++;
  }
  if (nw == 0 || nw == 4) return(EINVAL);
  for (i = 1; i < nw; i++)
	if (led_colour(wd[i], &rgb[i-1]) < 0) return(EINVAL);
  if (nw == 1 && led_streq(wd[0], "off")) {
	hal_led(0, 0, 0);
	return(count);
  }
  for (m = 0; m < 4 && !led_streq(wd[0], rb_mode[m].name); m++) ;
  if (m < 4) {				/* a sequence */
	if (nw != (m == 0 ? 1 : 3)) return(EINVAL);
	for (j = 0; j < nw - 1; j++)	/* gamma 2 and white balance */
		for (i = 0; i < 3; i++) {
			unsigned long v = rgb[j] >> (16 - 8 * i) & 0xFF;

			rb_user[j][i] = (u8_t) ((v * v * white[i] + 32512)
								/ 65025);
		}
	hal_led(0, 0, 0);			/* engines off, direct */
	lock();
	rb = RB_ON | m << 12 | (rb_mode[m].ncol - 1) << 8 | rb_mode[m].glide;
	unlock();
	rb_step();				/* the first colour next */
	return(count);
  }
  if (nw != 2 || hal_led_pattern(wd[0], rgb[0]) < 0) return(EINVAL);
  return(count);
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
  clock_t t = (clock_t) HAL_BL_TIMEOUT(st) * HZ, idle = TDIFF(now, k_input);

  /* R6.10: forced on never times out; forced off stays dark, and on
   * battery the LCD sleeps LCD_SAVE_S after the last key */
  if (HAL_BL_MODE(st) == 2) return(0);
  if (HAL_BL_MODE(st) == 1)
	return(idle >= (clock_t) LCD_SAVE_S * HZ && !hal_usb_awake()
							? BL_SAVE : BL_GONE);
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

  if (last_buttons & (GE_FIRE | GE_CHORD | GE_SAVED)) ge_task();
  if (nt_st & NT_NEW) {		/* N.6: a card notice to show */
	lock();				/* the tick shares the byte */
	nt_st &= ~NT_NEW;
	unlock();
	nt_over();
	lc_dirty = TRUE;
	k_input = get_uptime();		/* the light on, as for a key */
	lock();
	mt_arm(MT_VIBRO, 150);		/* and a short buzz */
	unlock();
	hal_vibro(1);
  }
  if (fg_pre != 0) {		/* R7b.2: the alert exited? */
	r = FG_SLOT(last_buttons);
	if (r < 0 || (proc_addr(r)->p_flags & P_SLOT_FREE)) fg_release();
  }
  bl_apply();
  if (last_buttons & (FG_LONG | FG_KILL)) {	/* R4b.8, here: not in the
						 * interrupt handler */
	unsigned b, sec = (unsigned) ((unsigned long) get_uptime() / HZ);
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
  if (as_proc != 0 && as_done) adcs_end(TRUE);	/* the samples are in */
  if (rtc_fired) {		/* R7b.1: timed runs from INIT */
	rtc_fired = FALSE;
	cause_sig(INIT_PROC_NR, SIGRTC);
  }
  if (rb & RB_DUE) rb_step();
  if (lc_dirty) {		/* console text, or a frame the SD held up */
	lc_dirty = FALSE;
	if (hal_lcd_flush(fbuf) < 0) lc_dirty = TRUE;
  }
  if (sp_waiting && note_i >= note_n) {		/* the tune is over */
	sp_waiting = FALSE;
	reply(REVIVE, sp_caller, sp_proc, sp_count);
  }
  if (ir_rd && ir_done) ir_rx_end(OK);	/* a frame is in */
  if (tx_on) {
	if (tx_done) {				/* the frame is out */
		tx_on = FALSE;
		if (tx_rfid && !rf_held) rfid_tx_off();
		else if (tx_rfid) tx_rfid = FALSE;	/* field held: keep it */
		if (ir_rd && ir_paused) {	/* the reader's capture again */
			ir_paused = FALSE;
			if (ir_rx_arm() < 0) ir_rx_end(EIO);
		}
		reply(REVIVE, tx_caller, tx_proc, tx_err ? EIO : tx_count);
	} else
		ir_fill();
  }
  if (rd_waiting && ev_head != ev_tail) {
	rd_waiting = FALSE;
	LAT_EVENT(LE_BREPLY, rd_proc);
	r = deliver(rd_proc, rd_vir, rd_count);
	reply(REVIVE, rd_caller, rd_proc, r);
  }
  if (sel_wait) sel_check();		/* poll(), R4b.6 */
  hid_check();
}


/*===========================================================================*
 *				sp_play					     *
 *===========================================================================*/
PRIVATE void sp_play()
{
/* Start note note_i, after the volume notes before it (they take no time);
 * quiet at the end.  Called with interrupts off, or from the clock tick.
 */
  while (note_i < note_n && note_hz[note_i] == SPK_VOL)
	sp_quiet = 100 - note_ticks[note_i++];
  if (note_i < note_n) {
	note_left = note_ticks[note_i];
	hal_tone(note_hz[note_i], 100 - sp_quiet);
  } else
	hal_tone(0, 0);
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
	note_i++;
	sp_play();
	if (note_i >= note_n) interrupt(PRINTER);
  }
  if (gp_watch) {		/* R6.4: the watched header pins' edges */
	unsigned i, now = 0;

	for (i = 0; i < HAL_GPIO_PINS; i++)
		if ((gp_watch & 1 << i) && hal_gpio_read(i) == 1) now |= 1 << i;
	if ((now ^ gp_last) & gp_watch) {
		gp_chg |= (now ^ gp_last) & gp_watch;
		gp_last = now;
		if (sel_wait & SEL_GPIO) interrupt(PRINTER);
	}
  }
  /* N.6: the card-detect switch, one GPIO read (its EXTI line is Up's) */
  if (hal_disk_present()) {
	if (!(nt_st & NT_SEEN)) {		/* in again */
		nt_st |= NT_SEEN;
		if ((nt_st & NT_KIND) == NT_OUT) nt_st &= ~NT_KIND;
	}
  } else if (nt_st & NT_SEEN) {		/* just pulled out */
	nt_st = (nt_st & ~(NT_SEEN | NT_KIND)) | NT_OUT | NT_NEW;
	interrupt(PRINTER);
  }
  /* the backlight's idle timeout: the task does the I2C (bl_apply) */
  if (bl_want(st, uptime_irq()) != bl_state(st)) interrupt(PRINTER);
  if (!(last_buttons & BTN_EXTI)) {
	fg_hold(btn_sample());
	ge_hold();
  }
  if (FG_SLOT(last_buttons) >= 0
	&& (proc_addr(FG_SLOT(last_buttons))->p_flags & P_SLOT_FREE)) {
	/* The foreground app has exited: clear it now, before its slot is
	 * used again and the next process there seems to hold the screen
	 * (#192).  An alert's exit (R7b.2) is a release: the task's job. */
	if (fg_pre != 0) interrupt(PRINTER);
	else last_buttons &= ~(FG_BITS | FG_LONG | FG_KILL | GE_OFF);
  } else if (fg_pre != 0 && FG_SLOT(last_buttons) < 0)
	interrupt(PRINTER);
  if ((last_buttons & FG_KILL) && ((unsigned) ((unsigned long) uptime_irq() / HZ)
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
  clock_t t = (clock_t) HAL_BL_TIMEOUT(st) * HZ, idle = TDIFF(now, k_input), d;

  if (lc_dirty || note_i < note_n || tx_on) return(0);
  if (last_buttons & FG_KILL) return(HZ);	/* SIGKILL due soon */
  if (bl_want(st, now) != bl_state(st)) return(0);	/* bl_apply due */
  if (HAL_BL_MODE(st) == 2) return(LONG_MAX);		/* forced on */
  if (HAL_BL_MODE(st) == 1) {				/* forced off */
	if (bl_state(st) == BL_SAVE || hal_usb_awake()) return(LONG_MAX);
	return((clock_t) LCD_SAVE_S * HZ - idle);	/* > 0: see above */
  }
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
  if (m_ptr->TTY_REQUEST == BTNIOCGEST) {
	/* R6.10: the foreground app (only) turns gestures off or on */
	int on;

	if (FG_SLOT(last_buttons) != m_ptr->PROC_NR) return(EPERM);
	user = numap(m_ptr->PROC_NR, (vir_bytes) m_ptr->ADDRESS, sizeof(int));
	if (user == 0) return(EFAULT);
	phys_copy(user, vir2phys(&on), (phys_bytes) sizeof(int));
	lock();
	last_buttons = on ? last_buttons & ~GE_OFF
				: (last_buttons & ~GE_CHORD) | GE_OFF;
	unlock();
	return(OK);
  }
  if (m_ptr->TTY_REQUEST == FGIOCPREEMPT) return(fg_preempt(m_ptr->PROC_NR));
  if (m_ptr->TTY_REQUEST == FGIOCRELEASE) {
	if (fg_pre == 0 || FG_SLOT(last_buttons) != m_ptr->PROC_NR)
		return(EPERM);
	fg_release();
	return(OK);
  }
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
  last_buttons = (last_buttons & ~(FG_BITS | FG_LONG | FG_KILL | GE_OFF))
					| (unsigned) (fg + 1) << 16;
  unlock();
  return(OK);
}


/*===========================================================================*
 *				fg_preempt				     *
 *===========================================================================*/
PRIVATE int fg_preempt(proc)
int proc;
{
/* R7b.2 FGIOCPREEMPT: an alert takes the screen and the buttons from
 * whoever has them, until FGIOCRELEASE or its exit.  The preempted app
 * keeps running (its frames are dropped, its button reads wait).  A
 * reader waiting on the buttons now (the launcher, or the app) gets an
 * empty read: it reads again and is parked (pre_rd) until the release.
 * Another alert meanwhile: EBUSY (it waits its turn).  Who may: whoever
 * can open /dev/lcd or /dev/buttons (group gui, by the device modes).
 */
  int fg = FG_SLOT(last_buttons);

  if (proc <= INIT_PROC_NR) return(EPERM);
  if (fg_pre != 0) {
	if (fg == proc) return(OK);
	if (fg >= 0 && !(proc_addr(fg)->p_flags & P_SLOT_FREE)) return(EBUSY);
	fg_release();			/* the last one has gone */
	fg = FG_SLOT(last_buttons);
  }
  fg_pre = PRE_ON;
  if (fg >= 0 && fg != proc && !(proc_addr(fg)->p_flags & P_SLOT_FREE))
	fg_pre |= PRE_FG | (fg + 1);
  if (rd_waiting && rd_proc != proc) {	/* an empty read: read again */
	rd_waiting = FALSE;
	reply(REVIVE, rd_caller, rd_proc, 0);
  }
  lock();
  last_buttons = (last_buttons & ~(FG_BITS | FG_LONG | FG_KILL | GE_OFF))
					| (unsigned) (proc + 1) << 16;
  unlock();
  return(OK);
}


/*===========================================================================*
 *				fg_release				     *
 *===========================================================================*/
PRIVATE void fg_release()
{
/* R7b.2: the alert is done (FGIOCRELEASE, or it exited): the preempted
 * foreground app gets the screen and the buttons back, the parked reader
 * an empty read (redraw, read again).  The alert puts the old picture
 * back itself before it releases. */
  int prev = fg_pre & PRE_FG ? (fg_pre & PRE_SLOT) - 1 : -1, p;

  if (prev >= 0 && (proc_addr(prev)->p_flags & P_SLOT_FREE)) prev = -1;
  fg_pre = 0;
  lock();
  last_buttons = (last_buttons & ~(FG_BITS | FG_LONG | FG_KILL | GE_OFF))
					| (unsigned) (prev + 1) << 16;
  unlock();
  if (pre_rd != 0) {
	p = pre_rd - 1;
	pre_rd = 0;
	reply(REVIVE, FS_PROC_NR, p, 0);
  }
}

/*===========================================================================*
 *				gestures (R6.10)			     *
 *===========================================================================*/
PRIVATE int ge_match(now)
unsigned now;
{
/* The action of the gesture these buttons make exactly, or 0. */
  unsigned i;

  for (i = 0; i < GE_N; i++)
	if ((now & BTN_ALL) == (ge_tab[i] & BTN_ALL)) return(ge_tab[i] >> 6);
  return(0);
}

PRIVATE int ge_blink_on(e)
clock_t e;			/* ticks into the hold */
{
/* The red blink: half-periods max(30, 200 - 170 * t / hold) ms, the first
 * one lit; so faster and faster up to the threshold. */
  long ms = (long) e * 1000 / HZ, hold = (long) GE_HOLD * 1000 / HZ, t = 0, h;
  int k = 0;

  while (t <= ms) {
	h = 200 - 170 * t / hold;
	t += h < 30 ? 30 : h;
	k++;
  }
  return(k & 1);
}

PRIVATE void ge_hold()
{
/* At the button samples (8 ms, interrupt): the hold time reached, or the
 * blink must flip.  The I2C is the task's (ge_task). */
  unsigned b = last_buttons;
  clock_t e;

  if (!(b & GE_CHORD)) return;
  e = TDIFF(uptime_irq(), k_input);
  if (e >= GE_HOLD) {
	last_buttons = (b & ~GE_CHORD) | GE_FIRE;	/* once per chord */
	interrupt(PRINTER);
  } else if (ge_blink_on(e) != ((b & GE_BLINK) != 0))
	interrupt(PRINTER);
}

PRIVATE void ge_task()
{
/* The task's part: toggle the mode and buzz, blink the LED, put the LED
 * back. */
  unsigned b = last_buttons;
  u32_t st;
  int a, on;

  if (b & GE_FIRE) {
	lock();
	last_buttons &= ~GE_FIRE;
	unlock();
	if ((a = ge_match(b)) != 0) {
		st = hal_bl_state();
		a = HAL_BL_MODE(st) == (u32_t) a ? 0 : a;	/* again: auto */
		hal_bl_state_set((st & ~HAL_BL_MODES) | (u32_t) a << 14);
		k_input = get_uptime();	/* forced-off's LCD save: from now */
		lock();
		mt_arm(MT_VIBRO, 30);		/* a 30 ms buzz (R6.3) */
		unlock();
		hal_vibro(1);
	}
	b = last_buttons;
  } else if (b & GE_CHORD) {
	on = ge_blink_on(TDIFF(get_uptime(), k_input));
	if (on == ((b & GE_BLINK) != 0)) return;
	if (!(b & GE_SAVED)) hal_led_get(ge_led);
	hal_led(on ? GE_RED : 0, 0, 0);
	lock();
	last_buttons = (last_buttons & ~GE_BLINK) | GE_SAVED
						| (on ? GE_BLINK : 0);
	unlock();
	return;
  }
  if (b & GE_SAVED) {			/* over: the LED as it was */
	hal_led(ge_led[0], ge_led[1], ge_led[2]);
	lock();
	last_buttons &= ~(GE_SAVED | GE_BLINK);
	unlock();
  }
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
  if (ie.st != IE_OFF && hal_ib_edge()) {	/* R6.7: iButton line 14 */
	ib_edge();
	if (!hal_btn_pending()) return 1;	/* no button's edge too */
  }
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
	ge_hold();
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
  /* R6.10: a gesture chord starts (exactly its buttons) or ends; the
   * hold counts from k_input, the change just now */
  if (!(flags & GE_OFF) && ge_match(now)) flags |= GE_CHORD;
  else if (flags & GE_CHORD) {
	flags &= ~GE_CHORD;		/* cancelled: the LED back (task) */
	interrupt(PRINTER);
  }
  /* With the backlight off, a press only turns it on (owner): neither it
   * nor anything else reaches the apps until every button is up again.
   * Not in forced-off (R6.10: keys work in the dark), except the press
   * that wakes a blank LCD. */
  if (hal_bl_state() & (HAL_BL_OFF | HAL_BL_STEPS))
	interrupt(PRINTER);		/* the light back on, at once */
  if ((hal_bl_state() & HAL_BL_OFF) && (now & changed)
		&& (HAL_BL_MODE(hal_bl_state()) != 1
			|| bl_state(hal_bl_state()) == BL_SAVE))
	flags |= SWALLOW;
  if (flags & SWALLOW) {
	if (now == 0) flags &= ~SWALLOW;
	last_buttons = now | flags;
	return(now);
  }
  for (b = 1; b <= BTN_BACK; b <<= 1) {
	if (!(changed & b)) continue;
	if ((u8_t) (ev_head - ev_tail) < NEVENTS) {
		ev_key[ev_head % NEVENTS] = b;
		ev_down[ev_head % NEVENTS] = (now & b) != 0;
		if (now & b) LAT_EVENT(LE_BTN, b);
		ev_head++;
	}
  }
  last_buttons = now | flags;
  if (rd_waiting || (sel_wait & SEL_RD)) interrupt(PRINTER);
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
 *				flipdev_notice				     *
 *===========================================================================*/
PRIVATE void nt_text(s, y)
char *s;
int y;
{
  const unsigned char *g;
  int n = 0, x, row, col, px;

  while (s[n] != 0) n++;
  if (n > (LCD_W - 16) / 4) n = (LCD_W - 16) / 4;
  x = (LCD_W - n * 4) / 2;
  for (; n > 0; n--, s++, x += 4) {
	if (*s < ' ' || *s > '~') continue;
	g = font_4x6[*s - ' '];
	for (row = 0; row < 6; row++)
		for (col = 0, px = x; col < 4; col++, px++)
			if (g[row] & (0x80 >> col))
				fbuf[((y + row) >> 3) * LCD_W + px]
					|= 1 << ((y + row) & 7);
  }
}

PRIVATE void nt_box(l1, l2)
char *l1, *l2;
{
/* A framed box with two lines in the middle of the screen, into fbuf. */
  int x, y, edge;

  for (y = 17; y <= 46; y++)
	for (x = 4; x < LCD_W - 4; x++) {
		edge = y == 17 || y == 46 || x == 4 || x == LCD_W - 5;
		if (edge) fbuf[(y >> 3) * LCD_W + x] |= 1 << (y & 7);
		else fbuf[(y >> 3) * LCD_W + x] &= ~(1 << (y & 7));
	}
  nt_text(l1, 23);
  nt_text(l2, 34);
}

PRIVATE void nt_over()
{
/* N.6: while the card is out (or another one is in) the notice stays over
 * every frame, whoever draws it. */
  if ((nt_st & NT_KIND) == NT_OUT) nt_box("SD card removed", "Put it back");
  if ((nt_st & NT_KIND) == NT_OTHER)
	nt_box("Card not usable", "Put the original back");
}

PUBLIC void flipdev_notice(kind)
int kind;
{
/* From the disk task (N.6): NT_OTHER, another card or one that does not
 * answer: the notice stays until the card goes; 0, the card is back: the
 * notice goes, and "carrying on" shows until the app's next frame. */
  lock();			/* the tick changes NT_SEEN in the same byte */
  nt_st = (nt_st & ~NT_KIND) | (kind == NT_OTHER ? NT_OTHER | NT_NEW : 0);
  unlock();
  if (kind != NT_OTHER) {
	nt_box("SD card is back", "Carrying on");
	lc_dirty = TRUE;
  }
  k_input = get_uptime();		/* the light on, as for a key */
  interrupt(PRINTER);
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
 *				ir_rx_arm, ir_rx_end			     *
 *===========================================================================*/
PRIVATE int pulse_read(m_ptr, user, count, input)
message *m_ptr;
phys_bytes user;
int count, input;
{
/* /dev/ir and /dev/subghzraw: one frame that starts while the read waits,
 * timed by the pulse engine (R4b.7) straight into the reader's buffer;
 * it ends after a silence, or when the buffer is full. */
  if (count < 4) return(EINVAL);
  if (m_ptr->TTY_FLAGS & O_NONBLOCK) return(EAGAIN);  /* no frame kept */
  if (ir_rd) return(EBUSY);		/* one reader at a time (TIM2) */
  if (as_proc != 0) return(EBUSY);	/* TIM2: an ADC stream's */
  if (input != 0 && tx_on) return(EBUSY);
  ir_caller = m_ptr->m_source;
  ir_proc = m_ptr->PROC_NR;
  ir_buf = user;				/* count bytes, checked */
  ir_count = count > 0x7FFC ? 0x7FFC : count & ~3;
  ir_started = ir_done = ir_paused = FALSE;
  pu_input = input;
  proc_addr(ir_proc)->p_physio = TRUE;	/* R4b.5: DMA into it */
  ir_rd = TRUE;			/* first: TIM2's interrupt is the
				 * transmitter's otherwise, and stops it */
  if (tx_on) ir_paused = TRUE;		/* armed when it is out */
  else if (ir_rx_arm() < 0) {
	ir_rd = FALSE;
	proc_addr(ir_proc)->p_physio = FALSE;
	return(ENXIO);
  }
  return(SUSPEND);
}

PRIVATE int ir_rx_arm()
{
/* The pulse engine (R4b.7) captures the reader's frame straight into its
 * buffer: the durations between edges, in us, by DMA. */
  if (hal_pulse_rx(pu_input, ir_buf, (unsigned) ir_count / 4,
		(pu_input ? SG_GAP_MS : IR_GAP_MS) * 1000) < 0)
	return(-1);
  lock();
  pm_busy |= PM_PULSE;			/* TIM2 and DMA need the clocks */
  unlock();
  return(0);
}

PRIVATE void ir_rx_end(r)
int r;			/* OK: the frame is in; 1: cancelled (no reply);
			 * else an error for the reader */
{
  unsigned n = ir_count / 4 - hal_pulse_left();

  if (pu_input == 2) {			/* RFID: timestamps, made durations */
	if (rf_held) hal_rfid_capture_stop();	/* the field stays up */
	else hal_rfid_stop();		/* the DMA first, then the memory */
	lock();
	mt_arm(MT_PULSE, 0);
	unlock();
	n = rf_durations(n);
  } else hal_pulse_stop();
  lock();
  if (!rf_held) pm_busy &= ~PM_PULSE;	/* a held field still needs them */
  unlock();
  proc_addr(ir_proc)->p_physio = FALSE;
  ir_rd = FALSE;
  if (r != 1) reply(REVIVE, ir_caller, ir_proc, r == OK ? (int) n * 4 : r);
}


/*===========================================================================*
 *				rfid_read				     *
 *===========================================================================*/
/* /dev/rfid (R6.5): a read turns the 125 kHz field on and gets the times
 * between the changes of the tag's signal (the antenna's comparator), as
 * /dev/ir's durations, for RF_MS or until the buffer is full; then the
 * field goes off.  The level of the first one is not known (the decoder
 * tries both). */
#define RF_MS		500
#define RF_POLL_MS	20
/* A held read follows a T5577 command, whose field gaps leave the antenna's
 * envelope detector unsettled; the field stays up and the capture is armed
 * only after it recovers (stock T5577_READ_SETTLE_MS).  No busy wait: the
 * ms timer counts the delay out (R0.8b). */
#define RF_SETTLE_MS	450

PRIVATE int rfid_read(m_ptr, user, count)
message *m_ptr;
phys_bytes user;
int count;
{
  if (count < 8) return(EINVAL);
  if (ir_rd || tx_on || as_proc != 0) return(EBUSY);	/* TIM1, TIM2 */
  ir_caller = m_ptr->m_source;
  ir_proc = m_ptr->PROC_NR;
  ir_buf = user;
  ir_count = count > 0x7FFC ? 0x7FFC : count & ~3;
  ir_started = ir_done = ir_paused = FALSE;
  pu_input = 2;
  rf_polls = 0;
  rf_settle = rf_held ? (RF_SETTLE_MS / RF_POLL_MS) : 0;
  if (rf_held) hal_rfid_pull(0);	/* the read path: PA2 low before the
					 * settle, so the envelope settles low */
  proc_addr(ir_proc)->p_physio = TRUE;	/* R4b.5: DMA into it */
  ir_rd = TRUE;
  if (!rf_held && hal_rfid_read(ir_buf, (unsigned) ir_count / 4,
						rf_carrier) < 0) {
	ir_rd = FALSE;
	proc_addr(ir_proc)->p_physio = FALSE;
	return(ENXIO);
  }
  lock();
  pm_busy |= PM_PULSE;
  mt_arm(MT_PULSE, RF_POLL_MS);
  unlock();
  return(SUSPEND);
}

PUBLIC void flipdev_pulse_poll()
{
/* The ms timer (interrupt): a held read's field is settling, or the read's
 * buffer is full, or its time is up, or look again. */
  if (!ir_rd || pu_input != 2) return;
  if (rf_settle) {			/* field up, capture not yet armed */
	if (--rf_settle == 0
	    && hal_rfid_capture(ir_buf, (unsigned) ir_count / 4) < 0) {
		ir_done = TRUE;
		interrupt(PRINTER);
		return;
	}
	mt_arm(MT_PULSE, RF_POLL_MS);
	return;
  }
  if (hal_pulse_left() == 0 || ++rf_polls >= RF_MS / RF_POLL_MS) {
	ir_done = TRUE;
	interrupt(PRINTER);
  } else mt_arm(MT_PULSE, RF_POLL_MS);
}

PRIVATE void rfid_tx_off()
{
/* A T5577 write is over: the field off, the transmitter the IR LED's. */
  hal_rfid_stop();
  hal_ir_target(0);
  tx_rfid = FALSE;
}

/*===========================================================================*
 *				rfid_field_off				     *
 *===========================================================================*/
PRIVATE void rfid_field_off()
{
/* R6.5 part 3: drop a field held up by RFIDIOCFIELD (ioctl, close or a
 * cancel).  Any capture in flight is ended first. */
  if (ir_rd && pu_input == 2) ir_rx_end(1);	/* cancel the capture */
  hal_rfid_field(0);
  rf_held = FALSE;
  lock();
  pm_busy &= ~PM_PULSE;			/* Stop2 again, also after a close */
  unlock();
}

/*===========================================================================*
 *				rfid_ioctl				     *
 *===========================================================================*/
PRIVATE int rfid_ioctl(m_ptr)
message *m_ptr;
{
/* /dev/rfid (R6.5, <minix/rfid.h>): hold the 125 kHz field up across a
 * T5577 command and its reply, or drop it. */
  phys_bytes user;
  int on;

  if (m_ptr->TTY_REQUEST == RFIDIOCEMU) return(rfid_emu_start(m_ptr));
  if (m_ptr->TTY_REQUEST == RFIDIOCCARRIER) {	/* R9.0.4 */
	user = numap(m_ptr->PROC_NR, (vir_bytes) m_ptr->ADDRESS,
						(vir_bytes) sizeof(on));
	if (user == 0) return(EFAULT);
	phys_copy(user, vir2phys(&on), (phys_bytes) sizeof(on));
	if (on < RFID_CARRIER_125 || on > RFID_CARRIER_134) return(EINVAL);
	rf_carrier = on;		/* for the next read */
	return(OK);
  }
  if (m_ptr->TTY_REQUEST != RFIDIOCFIELD) return(ENOTTY);
  user = numap(m_ptr->PROC_NR, (vir_bytes) m_ptr->ADDRESS, (vir_bytes) sizeof(on));
  if (user == 0) return(EFAULT);
  phys_copy(user, vir2phys(&on), (phys_bytes) sizeof(on));
  if (on) {
	if (rf_held) return(OK);		/* already ours */
	if (ir_rd || tx_on || as_proc != 0 || rf_emu != 0)
		return(EBUSY);			/* TIM1, TIM2 */
	hal_rfid_field(1);
	rf_held = TRUE;
	lock();
	pm_busy |= PM_PULSE;			/* the carrier needs the clocks */
	unlock();
  } else if (rf_held) {
	if (tx_on || (ir_rd && pu_input == 2)) return(EBUSY);	/* in flight */
	rfid_field_off();
  }
  return(OK);
}

/*===========================================================================*
 *				rfid_emu_start				     *
 *===========================================================================*/
PRIVATE int rfid_emu_start(m_ptr)
message *m_ptr;
{
/* RFIDIOCEMU (R6.5 part 4): load the antenna in step with a reader's own
 * carrier, from the caller's pattern, until it asks to stop (n = 0), closes
 * or is cancelled.  Nothing runs on the CPU meanwhile: the reader's field
 * clocks the timer and the DMA walks the pattern (hal_rfid_emulate).  The
 * two buffers stay the caller's, pinned while this runs (R4b.5). */
  struct rfid_emu e;
  phys_bytes user, dur, pulse;

  user = numap(m_ptr->PROC_NR, (vir_bytes) m_ptr->ADDRESS,
						(vir_bytes) sizeof(e));
  if (user == 0) return(EFAULT);
  phys_copy(user, vir2phys(&e), (phys_bytes) sizeof(e));
  if (e.n == 0) {				/* stop */
	if (rf_emu == m_ptr->PROC_NR + 1) rfid_emu_off();
	return(OK);
  }
  if (e.n > 512) return(EINVAL);
  if (rf_emu != 0) return(EBUSY);		/* one tag at a time */
  if (ir_rd || tx_on || as_proc != 0 || rf_held) return(EBUSY);	/* TIM2 */
  dur = numap(m_ptr->PROC_NR, (vir_bytes) e.dur, (vir_bytes) (e.n * 4));
  pulse = numap(m_ptr->PROC_NR, (vir_bytes) e.pulse, (vir_bytes) (e.n * 4));
  if (dur == 0 || pulse == 0) return(EFAULT);
  proc_addr(m_ptr->PROC_NR)->p_physio = TRUE;	/* the DMA reads them */
  if (hal_rfid_emulate(dur, pulse, e.n) < 0) {
	proc_addr(m_ptr->PROC_NR)->p_physio = FALSE;
	return(ENXIO);
  }
  rf_emu = m_ptr->PROC_NR + 1;
  lock();
  pm_busy |= PM_PULSE;			/* the timer and DMA need the clocks */
  unlock();
  return(OK);
}

/*===========================================================================*
 *				rfid_emu_off				     *
 *===========================================================================*/
PRIVATE void rfid_emu_off()
{
/* Stop emulating and let the pattern's memory go (and Stop2 in again). */
  int pr;

  if (rf_emu == 0) return;
  hal_rfid_emulate_stop();
  pr = rf_emu - 1;
  rf_emu = 0;
  proc_addr(pr)->p_physio = FALSE;
  lock();
  pm_busy &= ~PM_PULSE;
  unlock();
}

PRIVATE unsigned rf_durations(n)
unsigned n;			/* timestamps in the reader's buffer */
{
/* Durations between them, in place: n - 1 of them. */
  u32_t t[16], prev = 0;
  unsigned i, k, j;

  if (n < 2) return(0);
  phys_copy(ir_buf, vir2phys(&prev), (phys_bytes) 4);
  for (i = 1; i < n; i += k) {
	k = n - i < 16 ? n - i : 16;
	phys_copy(ir_buf + i * 4, vir2phys(t), (phys_bytes) k * 4);
	for (j = 0; j < k; j++) {
		u32_t cur = t[j];

		t[j] = cur - prev;
		prev = cur;
	}
	phys_copy(vir2phys(t), ir_buf + (i - 1) * 4, (phys_bytes) k * 4);
  }
  return(n - 1);
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
  if (ir_rd && (ir_started || pu_input != 0))
	return(EBUSY);				/* TIM2 takes a frame in */
  if (as_proc != 0) return(EBUSY);		/* TIM2: an ADC stream's */
  if (sp_waiting) return(EBUSY);		/* sbuf: the speaker's */
  n = count / 4;
  if (n > IR_TX_MAX + 1) return(EINVAL);
  for (i = 0; i < n; i += 16) {			/* check everything first */
	k = n - i < 16 ? n - i : 16;
	phys_copy(user + i * 4, vir2phys(d), (phys_bytes) k * 4);
	for (j = 0; j < k; j++) {
		if (i + j == 0 && (d[0] & IR_FREQ_FLAG)) {
			if (tx_rfid) return(EINVAL);	/* 125 kHz, fixed */
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
  if (tx_rfid) {				/* R6.5: the 125 kHz field */
	hz = 125000;
	duty = 50;
  }
  hal_ir_target(tx_rfid);
  hal_ir_freq(hz, duty);
  first_us = IR_DEC(ir_txq[tx_t % IR_TXQ]);
  tx_t++;
  next_us = first_us;
  if (tx_total > 1) {
	next_us = IR_DEC(ir_txq[tx_t % IR_TXQ]);
	tx_t++;
  }
  if (ir_rd && !ir_paused) {		/* the reader waits: TIM2 is ours */
	hal_pulse_stop();
	ir_paused = TRUE;
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
/* TIM2: a mark or space is over, the next one (already loaded) starts
 * now; or, receiving, the pulse engine's capture (R4b.7). */
  int update;

  if (ir_rd && !tx_on) {
	int f = hal_pulse_ack();

	if (f & HAL_PULSE_START) ir_started = TRUE;
	if ((f & HAL_PULSE_GAP) && ir_started) {
		ir_done = TRUE;
		interrupt(PRINTER);
	}
	return 1;
  }
  update = hal_ir_tx_ack();

  if (!tx_on || tx_done) {			/* nothing to send: off */
	hal_ir_tx_stop();
	return 1;
  }
  if (!update) return 1;			/* not the timer's update */
  if (tx_seg >= tx_total) {			/* the last one ended */
	if (rf_held) {			/* keep the field up for the reply */
		hal_ir_tx_stop();	/* the tx timer only, not the carrier */
		hal_ir_carrier(1);	/* the 125 kHz field, continuous */
	} else
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
