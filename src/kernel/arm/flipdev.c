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
 *
 * Access is controlled with the permissions of the device files.  The
 * buttons are sampled on every clock tick (flipdev_tick, from clock.c).
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
#define FB_SIZE		(LCD_W * LCD_H / 8)
#define NEVENTS		32		/* button event ring (power of 2) */

#define O_NONBLOCK	04000		/* from <fcntl.h> */

PRIVATE u8_t fbuf[FB_SIZE];
PRIVATE message fm;

/* Button events, filled by flipdev_tick() at interrupt level. */
PRIVATE u8_t ev_key[NEVENTS], ev_down[NEVENTS];
PRIVATE volatile unsigned ev_head, ev_tail;
PRIVATE unsigned last_buttons;

/* A process blocked in a read of /dev/buttons. */
PRIVATE int rd_waiting;
PRIVATE int rd_caller, rd_proc, rd_count;
PRIVATE vir_bytes rd_vir;

FORWARD _PROTOTYPE( void reply, (int code, int replyee, int proc, int status) );
FORWARD _PROTOTYPE( int do_rw, (message *m_ptr)			);
FORWARD _PROTOTYPE( int deliver, (int proc, vir_bytes vir, int count)	);
FORWARD _PROTOTYPE( void do_hardint, (void)				);

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
		r = (unsigned) fm.DEVICE <= LED_MINOR ? OK : ENXIO;
		break;
	case DEV_READ:
	case DEV_WRITE:
		r = do_rw(&fm);
		break;
	case CANCEL:
		if (rd_waiting && rd_proc == fm.PROC_NR) rd_waiting = FALSE;
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
