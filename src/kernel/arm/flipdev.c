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
 *				read: one received frame, up to 512 durations
 *				(it ends after 30 ms without carrier); blocks
 *				unless O_NONBLOCK
 *				write: up to 2048 durations, sent on a 38 kHz
 *				carrier with a third on; returns when sent.
 *				A first word with bit 31 set chooses the
 *				carrier instead: bits 0-19 the frequency
 *				(10000..60000 Hz), 20-26 the duty in %
 *				(0: 33 %)
 *	minor 8  /dev/subghz	the CC1101 radio, register access: see
 *				arm/subghz.c
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

#define LCD_MINOR	0
#define BTN_MINOR	1
#define BL_MINOR	2
#define LED_MINOR	3
#define VIBRO_MINOR	4
#define SPK_MINOR	5
#define GPIO_MINOR	6
#define IR_MINOR	7
#define SUBGHZ_MINOR	8		/* arm/subghz.c */
#define LAST_MINOR	8
#define IR_MAX		512		/* durations per received frame */
#define IR_TX_MAX	2048		/* durations per write */
#define IR_CHUNK	64		/* durations copied at a time */
#define IR_FREQ_FLAG	0x80000000	/* first word: carrier, not a time */
#define IR_HZ		38000		/* default carrier */
#define IR_DUTY		33		/* default duty, % */
#define IR_GAP_MS	30		/* silence that ends a frame */
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

/* Infrared receive, filled by ir_int() at interrupt level. */
PRIVATE u32_t ir_buf[IR_MAX];
PRIVATE volatile unsigned ir_n;		/* durations in ir_buf */
PRIVATE volatile int ir_active;		/* a frame is coming in */
PRIVATE volatile int ir_done;		/* a whole frame, not read yet */
PRIVATE volatile int ir_level = 1;	/* receiver output, 0: mark */
PRIVATE volatile u32_t ir_last;		/* hal_cycles() of the last edge */
PRIVATE u32_t cyc_us;			/* cycles per microsecond */
PRIVATE int ir_waiting, ir_caller, ir_proc, ir_count;
PRIVATE vir_bytes ir_vir;

/* A process blocked in a read of /dev/buttons. */
PRIVATE int rd_waiting;
PRIVATE int rd_caller, rd_proc, rd_count;
PRIVATE vir_bytes rd_vir;

FORWARD _PROTOTYPE( void reply, (int code, int replyee, int proc, int status) );
FORWARD _PROTOTYPE( int do_rw, (message *m_ptr)			);
FORWARD _PROTOTYPE( int deliver, (int proc, vir_bytes vir, int count)	);
FORWARD _PROTOTYPE( void do_hardint, (void)				);
FORWARD _PROTOTYPE( int ir_int, (int irq)				);
FORWARD _PROTOTYPE( int ir_deliver, (int proc, vir_bytes vir, int count) );
FORWARD _PROTOTYPE( int ir_send, (phys_bytes user, int count)		);

/*===========================================================================*
 *				flipdev_task				     *
 *===========================================================================*/
PUBLIC void flipdev_task()
{
  int r;

  last_buttons = hal_buttons();
  cyc_us = hal_cpu_hz() / 1000000;
  subghz_init();
  if ((r = hal_ir_irq()) >= 0) {
	put_irq_handler(r, ir_int);
	hal_ir_rx_on(1);
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
		break;
	case DEV_READ:
	case DEV_WRITE:
		r = do_rw(&fm);
		break;
	case CANCEL:
		if (rd_waiting && rd_proc == fm.PROC_NR) rd_waiting = FALSE;
		if (ir_waiting && ir_proc == fm.PROC_NR) ir_waiting = FALSE;
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
  case LCD_MINOR:
	if (count > FB_SIZE) count = FB_SIZE;
	if (m_ptr->m_type == DEV_READ) {
		phys_copy(vir2phys(fbuf), user, (phys_bytes) count);
	} else {
		phys_copy(user, vir2phys(fbuf), (phys_bytes) count);
		hal_lcd_flush(fbuf);
	}
	return(count);

  case BTN_MINOR:
	if (m_ptr->m_type != DEV_READ) return(EINVAL);
	if (count < 2) return(EINVAL);
	if (ev_head != ev_tail)
		return(deliver(m_ptr->PROC_NR, (vir_bytes) m_ptr->ADDRESS, count));
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

  case SUBGHZ_MINOR:
	return(subghz_rw(m_ptr->m_type == DEV_WRITE, user, count));

  case IR_MINOR:
	if (m_ptr->m_type == DEV_WRITE) return(ir_send(user, count));
	if (count < 4) return(EINVAL);
	if (ir_done)
		return(ir_deliver(m_ptr->PROC_NR, (vir_bytes) m_ptr->ADDRESS,
								count));
	if (m_ptr->TTY_FLAGS & O_NONBLOCK) return(EAGAIN);
	if (ir_waiting) return(EIO);		/* one reader at a time */
	ir_waiting = TRUE;
	ir_caller = m_ptr->m_source;
	ir_proc = m_ptr->PROC_NR;
	ir_vir = (vir_bytes) m_ptr->ADDRESS;
	ir_count = count;
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
/* New button events: finish a suspended read. */
  int r;

  if (sp_waiting && note_i >= note_n) {		/* the tune is over */
	sp_waiting = FALSE;
	reply(REVIVE, sp_caller, sp_proc, sp_count);
  }
  if (ir_waiting && ir_done) {			/* a frame came in */
	ir_waiting = FALSE;
	r = ir_deliver(ir_proc, ir_vir, ir_count);
	reply(REVIVE, ir_caller, ir_proc, r);
  }
  if (!rd_waiting || ev_head == ev_tail) return;
  rd_waiting = FALSE;
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
  if (ir_active && ir_level == 1
	&& hal_cycles() - ir_last > IR_GAP_MS * 1000 * cyc_us) {
	ir_active = FALSE;			/* silence: the frame is over */
	if (ir_n > 0) {
		ir_done = TRUE;
		if (ir_waiting) interrupt(PRINTER);
	}
  }
  if (changed == 0) return;
  for (b = 1; b <= BTN_BACK; b <<= 1) {
	if (!(changed & b)) continue;
	if (ev_head - ev_tail < NEVENTS) {
		ev_key[ev_head % NEVENTS] = b;
		ev_down[ev_head % NEVENTS] = (now & b) != 0;
		ev_head++;
	}
  }
  last_buttons = now;
  if (rd_waiting) interrupt(PRINTER);
}


/*===========================================================================*
 *				ir_int					     *
 *===========================================================================*/
PRIVATE int ir_int(irq)
int irq;
{
/* An edge from the infrared receiver: note the duration since the last. */
  u32_t now = hal_cycles();
  int level = hal_ir_rx_edge();

  if (level == ir_level) return 1;	/* glitch, nothing changed */
  ir_level = level;
  if (ir_done) return 1;		/* last frame not read yet: drop */
  if (!ir_active) {
	if (level == 0) {		/* a frame starts with a mark */
		ir_active = TRUE;
		ir_n = 0;
		ir_last = now;
	}
	return 1;
  }
  if (ir_n < IR_MAX) ir_buf[ir_n++] = (now - ir_last) / cyc_us;
  ir_last = now;
  return 1;
}


/*===========================================================================*
 *				ir_deliver				     *
 *===========================================================================*/
PRIVATE int ir_deliver(proc, vir, count)
int proc;
vir_bytes vir;
int count;
{
/* Hand the received frame to a reader; the receiver may take the next. */
  phys_bytes user;
  int n = ir_n * 4;

  if (n > count) n = count & ~3;
  if ((user = numap(proc, vir, (vir_bytes) n)) == 0) return(EFAULT);
  phys_copy(vir2phys(ir_buf), user, (phys_bytes) n);
  ir_done = FALSE;
  return(n);
}


/*===========================================================================*
 *				ir_send					     *
 *===========================================================================*/
PRIVATE int ir_send(user, count)
phys_bytes user;
int count;
{
/* Send marks and spaces.  Timed by the cycle counter against the start,
 * so interrupts in between delay an edge a little but never add up.  The
 * durations come from the caller in chunks, copied while a mark or space
 * runs (a copy takes microseconds, the shortest duration hundreds). */
  static u32_t d[IR_CHUNK];
  u32_t t, hz = IR_HZ, duty = IR_DUTY;
  int i, j, k, n, first = 0;

  if (count % 4 != 0 || count == 0) return(EINVAL);
  n = count / 4;
  if (n > IR_TX_MAX) return(EINVAL);
  for (i = 0; i < n; i += IR_CHUNK) {		/* check everything first */
	k = n - i < IR_CHUNK ? n - i : IR_CHUNK;
	phys_copy(user + i * 4, vir2phys(d), (phys_bytes) k * 4);
	for (j = 0; j < k; j++) {
		if (i + j == 0 && (d[0] & IR_FREQ_FLAG)) {
			hz = d[0] & 0xFFFFF;
			duty = (d[0] >> 20) & 0x7F;
			if (duty == 0) duty = IR_DUTY;
			if (hz < 10000 || hz > 60000 || duty > 90)
				return(EINVAL);
			first = 1;
		} else if (d[j] > 1000000) return(EINVAL); /* over a second */
	}
  }
  if (n - first == 0 || (n - first) % 2 == 0) return(EINVAL);
  hal_ir_freq(hz, duty);
  t = hal_cycles();
  for (i = first; i < n; i++) {
	hal_ir_carrier(((i - first) & 1) == 0);
	if (i == first || i % IR_CHUNK == 0) {
		k = i - i % IR_CHUNK;
		phys_copy(user + k * 4, vir2phys(d),
		     (phys_bytes) (n - k < IR_CHUNK ? n - k : IR_CHUNK) * 4);
	}
	t += d[i % IR_CHUNK] * cyc_us;
	while ((i32_t) (hal_cycles() - t) < 0) ;
  }
  hal_ir_carrier(0);
  return(count);
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
