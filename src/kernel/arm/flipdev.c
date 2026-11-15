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
 *	minor 2  /dev/backlight	write: one byte, brightness 0..255
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

#define LCD_MINOR	0
#define BTN_MINOR	1
#define BL_MINOR	2
#define LED_MINOR	3
#define VIBRO_MINOR	4
#define SPK_MINOR	5
#define GPIO_MINOR	6
#define LAST_MINOR	7
#define IR_MINOR	7
#define NNOTES		64
#define FB_SIZE		(LCD_W * LCD_H / 8)
#define NEVENTS		32		/* button event ring (power of 2) */

#define O_NONBLOCK	04000		/* from <fcntl.h> */

PRIVATE u8_t fbuf[FB_SIZE];
PRIVATE message fm;

/* Button events, filled by flipdev_tick() at interrupt level. */
PRIVATE u8_t ev_key[NEVENTS], ev_down[NEVENTS];
PRIVATE volatile unsigned ev_head, ev_tail;
PRIVATE unsigned last_buttons;

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
	if (m_ptr->m_type == DEV_READ) {
		if (lc_on) return(0);	/* console text is not for apps */
		phys_copy(vir2phys(fbuf), user, (phys_bytes) count);
	} else if (count == 1) {
		phys_copy(user, vir2phys(buf), (phys_bytes) 1);
		if (buf[0] != 'C') return(EINVAL);
		memset(fbuf, 0, sizeof(fbuf));	/* the console from now on */
		lc_x = lc_y = lc_esc = 0;
		lc_on = TRUE;
		hal_lcd_flush(fbuf);
	} else {
		if (lc_on) {	/* a short frame must not leave console text
				 * behind in fbuf for a later read */
			memset(fbuf, 0, sizeof(fbuf));
			lc_on = FALSE;
		}
		phys_copy(user, vir2phys(fbuf), (phys_bytes) count);
		LAT_EVENT(LE_LCD, m_ptr->PROC_NR);
		hal_lcd_flush(fbuf);
	}
	return(count);

  case BTN_MINOR:
	if (m_ptr->m_type != DEV_READ) return(EINVAL);
	if (count < 2) return(EINVAL);
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

  case VIBRO_MINOR:
	if (m_ptr->m_type != DEV_WRITE) return(EINVAL);
	phys_copy(user, vir2phys(buf), (phys_bytes) 1);
	hal_vibro(buf[0] != 0 && buf[0] != '0');
	return(count);

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
  case LED_MINOR:
	if (m_ptr->m_type != DEV_WRITE) return(EINVAL);
	if (count > 3) count = 3;
	phys_copy(user, vir2phys(buf), (phys_bytes) count);
	if (m_ptr->DEVICE == BL_MINOR) hal_backlight(buf[0]);
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
 *				do_hardint				     *
 *===========================================================================*/
PRIVATE void do_hardint()
{
/* New button events: finish a suspended read.  Console text on the LCD:
 * show it. */
  int r;

  if (lc_on && lc_dirty) {
	lc_dirty = FALSE;
	hal_lcd_flush(fbuf);
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
/* Called by the clock interrupt handler: sample the buttons, queue the
 * changes, wake the task if somebody is waiting.
 */
  unsigned now = hal_buttons(), changed = now ^ last_buttons, b;

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
  if (changed == 0) return;
  k_input = uptime_irq();		/* no background flash work now */
  for (b = 1; b <= BTN_BACK; b <<= 1) {
	if (!(changed & b)) continue;
	if (ev_head - ev_tail < NEVENTS) {
		ev_key[ev_head % NEVENTS] = b;
		ev_down[ev_head % NEVENTS] = (now & b) != 0;
		if (now & b) LAT_EVENT(LE_BTN, b);
		ev_head++;
	}
  }
  last_buttons = now;
  if (rd_waiting) interrupt(PRINTER);
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
  u32_t d[16], hz = IR_HZ, duty = IR_DUTY, first_us;
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
  first_us = IR_DEC(ir_txq[tx_t++ % IR_TXQ]);
  tx_seg = 1;
  hal_ir_carrier(1);				/* the first mark */
  hal_ir_tx_start(first_us, tx_total > 1 ? IR_DEC(ir_txq[tx_t++ % IR_TXQ])
								: first_us);
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
	if (RING_N(tx_h, tx_t) == IR_TXQ / 2 && tx_fetch < tx_total)
		interrupt(PRINTER);		/* half empty: refill */
  }
  return 1;
}


/*===========================================================================*
 *				ir_stop_tx				     *
 *===========================================================================*/
PRIVATE void ir_stop_tx()
{
  hal_ir_tx_stop();
  hal_ir_carrier(0);
}
