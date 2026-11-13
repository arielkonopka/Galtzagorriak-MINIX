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

#define CONS_IN_BYTES	64	/* serial input buffer (power of 2) */

PRIVATE char ibuf[CONS_IN_BYTES];
PRIVATE unsigned ihead, itail;	/* free-running; ihead written by intr */
PRIVATE int uart_irq;

FORWARD _PROTOTYPE( void cons_write, (tty_t *tp)			);
FORWARD _PROTOTYPE( void cons_echo, (tty_t *tp, int c)			);
FORWARD _PROTOTYPE( void cons_read, (tty_t *tp)				);
FORWARD _PROTOTYPE( int cons_int, (int irq)				);
FORWARD _PROTOTYPE( void out_char, (int c)				);

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
}


/*===========================================================================*
 *				cons_int				     *
 *===========================================================================*/
PRIVATE int cons_int(irq)
int irq;
{
/* Serial input interrupt: move characters into the ring buffer and wake the
 * TTY task.  Characters that do not fit are dropped.
 */
  int c;

  while ((c = hal_uart_getc()) >= 0) {
	if (ihead - itail < CONS_IN_BYTES) {
		ibuf[ihead % CONS_IN_BYTES] = c;
		ihead++;
	}
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

  while (itail != ihead) {
	c = ibuf[itail % CONS_IN_BYTES];
	itail++;
	(void) in_process(tp, &c, 1);
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
/* Output post-processing is done here like on the PC console: NL -> CR NL. */
  if (c == '\n') hal_uart_putc('\r');
  hal_uart_putc(c);
}


/*===========================================================================*
 *				putk					     *
 *===========================================================================*/
PUBLIC void putk(c)
int c;
{
/* Kernel printf() output.  c == 0 means flush, a no-op here. */
  if (c != 0) out_char(c);
}
