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

#define CONS_IN_BYTES	64	/* UART input buffer (power of 2); USB input is
				 * read from the USB layer's own ring (R2a.3) */

PRIVATE char ibuf[CONS_IN_BYTES];
PRIVATE unsigned ihead, itail;	/* free-running; ihead written by intr */
PRIVATE int uart_irq;
PRIVATE int usb_irq = -1;
PRIVATE u8_t uart_held;		/* UART receive interrupt off: ibuf full */
PRIVATE u8_t usb_stuck;		/* a wait for USB room timed out: no more
				 * waits until the host reads again (K1) */
#define USB_CHUNK	64	/* cons_write waits for this much ring room */
#define USB_WAIT_MS	20	/* echo/printf: longest wait for room */

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
  if (usb_fill()
	|| (tty_table[0].tty_outleft != 0 && hal_usb_room() >= USB_CHUNK)) {
	tty_table[0].tty_events = 1;	/* input, or room for waiting output */
	force_timeout();
  }
  return 1;
}


/*===========================================================================*
 *				usb_fill				     *
 *===========================================================================*/
PRIVATE int usb_fill()
{
/* Is there input?  USB characters stay in the USB layer's ring until the
 * line discipline takes them (cons_read); a full ring holds the host off
 * (flow control).
 */
  if (usb_irq >= 0 && hal_usb_peekc() >= 0) return TRUE;
  return ihead != itail;
}


/*===========================================================================*
 *				usb_out					     *
 *===========================================================================*/
PRIVATE void usb_out(buf, n)
char *buf;
unsigned n;
{
/* Queue output for USB; with nobody listening, excess is dropped.  Writes
 * from processes never get here without room (cons_write).  Echo and kernel
 * printf() from a task wait for room at most USB_WAIT_MS, then drop: a
 * terminal that is open but not read must not hold a kernel task (rule
 * 0.8; the watchdog would reset the board after 2 s).  After one such
 * timeout the next bytes are dropped at once, until the host makes room
 * again: else every echoed character of a flood waited its 20 ms, and a
 * host that sends but never reads held the TTY task for good (security
 * review 2026-11-16, K1).
 */
  unsigned k;
  u32_t start = hal_cycles();

  if (usb_irq < 0) return;
  if (usb_stuck && hal_usb_room() >= USB_CHUNK) usb_stuck = FALSE;
  while (n > 0) {
	k = hal_usb_write(buf, n);
	buf += k;
	n -= k;
	if (n == 0 || !hal_usb_connected() || usb_stuck
			|| k_reenter != (unsigned char) -1)	/* not a task */
		break;
	if (hal_cycles() - start > USB_WAIT_MS * (hal_cpu_hz() / 1000)) {
		usb_stuck = TRUE;
		break;
	}
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
  u32_t start = hal_cycles();

  /* In raw mode a full input queue refuses a character; it is kept for
   * the next read instead of dropped (ZMODEM needs every byte): looked at
   * first, taken only when accepted. */
  for (;;) {
	int ch;

	/* A host that sends without pause (USB refills the ring while we
	 * take from it) kept the TTY task here for good: no process ran,
	 * and the watchdog reset the board (security review 2026-11-16,
	 * K1).  After a slice the rest waits: handle_events() comes back
	 * on the next clock tick. */
	if (hal_cycles() - start > TTY_SLICE_MS * (hal_cpu_hz() / 1000)) {
		tp->tty_events = 1;
		break;
	}

	if (itail != ihead) {			/* UART input */
		c = ibuf[itail % CONS_IN_BYTES];
		if (in_process(tp, &c, 1) == 0) break;
		itail++;
		k_input = get_uptime();
		continue;
	}
	if (usb_irq < 0 || (ch = hal_usb_peekc()) < 0) break;
	c = ch;
	if (in_process(tp, &c, 1) == 0) break;
	(void) hal_usb_getc();
	k_input = get_uptime();		/* no background flash work now */
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

  unsigned room;

  if ((count = tp->tty_outleft) == 0 || tp->tty_inhibited) return;

  do {
	if (count > sizeof(buf)) count = sizeof(buf);
	/* Only what the USB ring takes now; the rest when the USB interrupt
	 * sees room (usb_int), the writer stays suspended meanwhile.  No
	 * waiting in the task for a host that does not read. */
	if (usb_irq >= 0 && (room = hal_usb_room()) < (unsigned) count) {
		if (room < USB_CHUNK) break;
		count = room;
	}
	user_phys = proc_vir2phys(proc_addr(tp->tty_outproc), tp->tty_out_vir);
	phys_copy(user_phys, vir2phys(buf), (phys_bytes) count);

	tp->tty_out_vir += count;
	tp->tty_outcum += count;
	tp->tty_outleft -= count;

	for (i = 0; i < count; i++) {
		/* ^G beeps, unless the output is raw (binary, rz/sz). */
		if (buf[i] == '\007' && (tp->tty_termios.c_oflag & OPOST))
			bell();
		out_char(buf[i]);
	}
  } while ((count = tp->tty_outleft) != 0 && !tp->tty_inhibited);
  /* (break: the USB interrupt wakes TTY when there is room) */

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
  lcd_cons_putc(c);		/* and on the LCD, if it shows the console */
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
