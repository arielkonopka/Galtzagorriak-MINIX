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
#define LAST_MINOR	6
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

/* Notes being played (/dev/speaker), timed by flipdev_tick(). */
PRIVATE u16_t note_hz[NNOTES], note_ticks[NNOTES];
PRIVATE volatile int note_n, note_i;	/* count, current; note_i == note_n:
					 * done */
PRIVATE volatile unsigned note_left;	/* ticks left of the current one */
PRIVATE int sp_waiting, sp_caller, sp_proc, sp_count;

/* A process blocked in a read of /dev/buttons. */
PRIVATE int rd_waiting;
PRIVATE int rd_caller, rd_proc, rd_count;
PRIVATE vir_bytes rd_vir;

FORWARD _PROTOTYPE( void reply, (int code, int replyee, int proc, int status) );
FORWARD _PROTOTYPE( int do_rw, (message *m_ptr)			);
FORWARD _PROTOTYPE( int deliver, (int proc, vir_bytes vir, int count)	);
FORWARD _PROTOTYPE( void do_hardint, (void)				);
FORWARD _PROTOTYPE( void lc_newline, (void)				);

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
  if ((user = numap(m_ptr->PROC_NR, (vir_bytes) m_ptr->ADDRESS, count)) == 0)
	return(EFAULT);

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
		lc_on = FALSE;
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

  if (sp_waiting && note_i >= note_n) {		/* the tune is over */
	sp_waiting = FALSE;
	reply(REVIVE, sp_caller, sp_proc, sp_count);
  }
  if (lc_on && lc_dirty) {
	lc_dirty = FALSE;
	hal_lcd_flush(fbuf);
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

  if (note_i < note_n && --note_left == 0) {	/* next note, or quiet */
	if (++note_i < note_n) {
		note_left = note_ticks[note_i];
		hal_tone(note_hz[note_i]);
	} else {
		hal_tone(0);
		interrupt(PRINTER);
	}
  }
  if (lc_dirty && ++lc_wait >= LC_TICKS) {
	lc_wait = 0;
	interrupt(PRINTER);
  }
  if (changed == 0) return;
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
