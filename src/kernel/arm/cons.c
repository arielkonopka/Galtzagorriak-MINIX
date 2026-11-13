/* Console driver for the Flipper Zero port: the TTY device behind
 * /dev/console.  This plays the role of console.c + keyboard.c on the PC.
 *
 * Output goes to the debug serial port (polled).  Input arrives by the UART
 * receive interrupt into a small ring buffer, from which the TTY task takes
 * it with cons_read().  The LCD tty and USB serial join later (milestone 5).
 */
#include "../kernel.h"
#include <termios.h>
#include <minix/com.h>
#include "../tty.h"
#include "../proc.h"
#include "armcpu.h"
#include "hal.h"
#include "fbcon.h"

#define CONS_IN_BYTES	256	/* serial input buffer (power of 2): pasted lines */

PRIVATE char ibuf[CONS_IN_BYTES];
PRIVATE unsigned ihead, itail;	/* free-running; ihead written by intr */
PRIVATE int uart_irq;
PRIVATE int usb_irq = -1;
PRIVATE int uart_held;		/* UART receive interrupt off: ibuf full */

FORWARD _PROTOTYPE( void cons_write, (tty_t *tp)			);
FORWARD _PROTOTYPE( void cons_echo, (tty_t *tp, int c)			);
FORWARD _PROTOTYPE( void cons_read, (tty_t *tp)				);
FORWARD _PROTOTYPE( int cons_int, (int irq)				);
FORWARD _PROTOTYPE( void out_char, (int c)				);
FORWARD _PROTOTYPE( int usb_int, (int irq)				);
FORWARD _PROTOTYPE( void usb_out, (char *buf, unsigned n)		);
FORWARD _PROTOTYPE( int usb_fill, (void)				);

/*===========================================================================*
 *				scr_init				     *
 *===========================================================================*/
PUBLIC void scr_init(tp)
tty_t *tp;
{
  if (tp != &tty_table[0]) return;	/* one console */

  tp->tty_devread = cons_read;
  tp->tty_devwrite = cons_write;
  tp->tty_echo = cons_echo;

  ihead = itail = 0;
  uart_irq = hal_uart_irq();
  put_irq_handler(uart_irq, cons_int);
  hal_uart_rx_irq(1);
  enable_irq(uart_irq);

  /* The USB serial port is a second way into the same console. */
  if (hal_usb_init() == 0) {
	usb_irq = hal_usb_irq();
	put_irq_handler(usb_irq, usb_int);
	enable_irq(usb_irq);
  }

}


/*===========================================================================*
 *				usb_int					     *
 *===========================================================================*/
PRIVATE int usb_int(irq)
int irq;
{
/* USB interrupt: run the USB stack, then collect received characters. */
  hal_usb_poll();
  if (usb_fill()) {
	tty_table[0].tty_events = 1;
	force_timeout();
  }
  return 1;
}


/*===========================================================================*
 *				usb_fill				     *
 *===========================================================================*/
PRIVATE int usb_fill()
{
/* Move received USB characters into ibuf, but only as many as fit: the rest
 * stays queued in the USB layer, which then holds the host off (flow
 * control).  Returns true if anything is waiting in ibuf.
 */
  int c;

  if (usb_irq < 0) return ihead != itail;
  while (ihead - itail < CONS_IN_BYTES && (c = hal_usb_getc()) >= 0) {
	ibuf[ihead % CONS_IN_BYTES] = c;
	ihead++;
  }
  return ihead != itail;
}


/*===========================================================================*
 *				usb_out					     *
 *===========================================================================*/
PRIVATE void usb_out(buf, n)
char *buf;
unsigned n;
{
/* Queue output for USB.  If a terminal is attached, wait for room (the USB
 * interrupt drains the ring); with nobody listening, excess is dropped.
 */
  unsigned k;

  if (usb_irq < 0) return;
  while (n > 0) {
	k = hal_usb_write(buf, n);
	buf += k;
	n -= k;
	if (n > 0 && (!hal_usb_connected()
			|| k_reenter != (unsigned char) -1)) break;	/* not a task */
  }
}


/*===========================================================================*
 *				cons_int				     *
 *===========================================================================*/
PRIVATE int cons_int(irq)
int irq;
{
/* Serial input interrupt: move characters into the ring buffer and wake the
 * TTY task.  When the buffer is full the receive interrupt is switched off
 * and the rest stays in the UART, which holds the sender back (QEMU does;
 * a real line overruns); cons_read() switches it on again.
 */
  int c;

  while (ihead - itail < CONS_IN_BYTES && (c = hal_uart_getc()) >= 0) {
	ibuf[ihead % CONS_IN_BYTES] = c;
	ihead++;
  }
  if (ihead - itail >= CONS_IN_BYTES) {
	hal_uart_rx_irq(0);
	uart_held = TRUE;
  }
  tty_table[0].tty_events = 1;
  force_timeout();
  return 1;			/* reenable the interrupt */
}


/*===========================================================================*
 *				cons_read				     *
 *===========================================================================*/
PRIVATE void cons_read(tp)
tty_t *tp;
{
/* Hand buffered input characters to the line discipline. */
  char c;

  for (;;) {
	if (itail == ihead) {
		lock();
		(void) usb_fill();	/* more may wait in the USB layer */
		unlock();
		if (itail == ihead) break;
	}
	c = ibuf[itail % CONS_IN_BYTES];
	/* In raw mode a full input queue refuses the character; keep it
	 * for the next read instead of dropping it (ZMODEM needs every
	 * byte). */
	if (in_process(tp, &c, 1) == 0) break;
	itail++;
  }
  if (uart_held && ihead - itail < CONS_IN_BYTES) {
	/* Take what waits in the UART ourselves: switching the interrupt
	 * back on does not necessarily raise it again for a byte that is
	 * already there.
	 */
	int ch;

	/* Interrupt on first, then drain: a byte arriving in between still
	 * raises it (the other way round it could be stranded for good). */
	lock();
	uart_held = FALSE;
	hal_uart_rx_irq(1);
	while (ihead - itail < CONS_IN_BYTES && (ch = hal_uart_getc()) >= 0) {
		ibuf[ihead % CONS_IN_BYTES] = ch;
		ihead++;
	}
	if (ihead - itail >= CONS_IN_BYTES) {
		hal_uart_rx_irq(0);
		uart_held = TRUE;
	}
	unlock();
  }
}


/*===========================================================================*
 *				cons_write				     *
 *===========================================================================*/
PRIVATE void cons_write(tp)
register tty_t *tp;
{
/* Copy output from the writing process and send it to the serial port. */
  int count, i;
  char buf[64];
  phys_bytes user_phys;

  if ((count = tp->tty_outleft) == 0 || tp->tty_inhibited) return;

  do {
	if (count > sizeof(buf)) count = sizeof(buf);
	user_phys = proc_vir2phys(proc_addr(tp->tty_outproc), tp->tty_out_vir);
	phys_copy(user_phys, vir2phys(buf), (phys_bytes) count);

	tp->tty_out_vir += count;
	tp->tty_outcum += count;
	tp->tty_outleft -= count;

	for (i = 0; i < count; i++) out_char(buf[i]);
  } while ((count = tp->tty_outleft) != 0 && !tp->tty_inhibited);

  if (tp->tty_outleft == 0) {
	tty_reply(tp->tty_outrepcode, tp->tty_outcaller, tp->tty_outproc,
							tp->tty_outcum);
	tp->tty_outcum = 0;
  }
}


/*===========================================================================*
 *				cons_echo				     *
 *===========================================================================*/
PRIVATE void cons_echo(tp, c)
tty_t *tp;
int c;
{
  out_char(c);
}


/*===========================================================================*
 *				out_char				     *
 *===========================================================================*/
PRIVATE void out_char(c)
int c;
{
/* Send one byte as it is.  NL -> CR NL is TTY's job (OPOST/ONLCR), so
 * binary output in raw mode (rz/sz) passes untouched.
 */
  char ch = c;

  hal_uart_putc(c);
  usb_out(&ch, 1);
}


/*===========================================================================*
 *				putk					     *
 *===========================================================================*/
PUBLIC void putk(c)
int c;
{
/* Kernel printf() output.  c == 0 means flush, a no-op here. */
  if (c == '\n') out_char('\r');
  if (c != 0) out_char(c);
}
