/* The MTP test line (spec R10.4, T29a), QEMU only: /dev/mtpline, minor 21
 * of major 6, part of the Flipper device task (flipdev.c includes this
 * file under BOARD_QEMU).  Raw bytes both ways on QEMU's second serial
 * line (the HAL's CMSDK UART1), for mtpd's stream transport: the PC side,
 * tools/mtphost.py, sits on the other end of QEMU's socket.  No tty: no
 * echo, no line editing, no special characters.
 *
 * A read returns what has arrived (at least one byte, at most 64; it waits
 * for one, or EAGAIN with O_NONBLOCK); one reader at a time.  A write sends all of
 * it, polled (QEMU's UART takes a byte at once).  Not built for the
 * Flipper, where MTP goes over USB (T29b): RAM there 0 B.
 *
 * RAM (QEMU): the 64 B receive ring and the reader's state, ~84 B.
 */

#define LINE_MINOR	21
#define LINE_RING	64		/* divides 256: u8_t indices wrap */

PRIVATE u8_t ln_ring[LINE_RING];
PRIVATE volatile u8_t ln_head, ln_tail;	/* free-running */
PRIVATE volatile u8_t ln_full;		/* RX interrupt off: ring was full */
PRIVATE u8_t ln_rd;			/* a reader waits */
PRIVATE int ln_caller, ln_proc, ln_count;
PRIVATE vir_bytes ln_vir;

/*===========================================================================*
 *				line_fill				     *
 *===========================================================================*/
PRIVATE void line_fill()
{
/* Move received bytes into the ring; with the ring full, the RX interrupt
 * goes off and the byte stays in the UART (QEMU then waits). */
  int c;

  while ((u8_t) (ln_head - ln_tail) < LINE_RING) {
	if ((c = hal_line_getc()) < 0) return;
	ln_ring[ln_head % LINE_RING] = c;
	ln_head++;
  }
  hal_line_rx_irq(0);
  ln_full = TRUE;
}


/*===========================================================================*
 *				line_int				     *
 *===========================================================================*/
PRIVATE int line_int(irq)
int irq;
{
  line_fill();
  if (ln_rd) interrupt(PRINTER);
  return(1);
}


/*===========================================================================*
 *				line_init				     *
 *===========================================================================*/
PRIVATE void line_init()
{
  hal_line_init();
  put_irq_handler(hal_line_irq(), line_int);
  enable_irq(hal_line_irq());
}


/*===========================================================================*
 *				line_give				     *
 *===========================================================================*/
PRIVATE int line_give(proc, vir, count)
int proc;
vir_bytes vir;
int count;
{
/* Copy what the ring holds (up to count) to the reader; the byte count. */
  u8_t buf[LINE_RING];
  phys_bytes user;
  int n = 0;

  /* #438: at most one ring's worth.  The RX interrupt is on while this
   * loop runs (the ring was not full) and refills the ring behind it, so
   * ln_head - ln_tail alone does not bound n: a long read ran past buf
   * into the saved registers (the caller's r4, PRINTER's &fm) and
   * flipdev_task took a bus fault.  A short read is the rule here. */
  if (count > LINE_RING) count = LINE_RING;
  while (ln_tail != ln_head && n < count) {
	buf[n++] = ln_ring[ln_tail % LINE_RING];
	ln_tail++;
  }
  if (ln_full) {		/* room again: take what QEMU holds */
	lock();
	ln_full = FALSE;
	hal_line_rx_irq(1);
	line_fill();
	unlock();
  }
  if ((user = numap(proc, vir, (vir_bytes) n)) == 0) return(EFAULT);
  phys_copy(vir2phys(buf), user, (phys_bytes) n);
  return(n);
}


/*===========================================================================*
 *				line_rw					     *
 *===========================================================================*/
PRIVATE int line_rw(m_ptr, user, count)
message *m_ptr;
phys_bytes user;
int count;
{
  u8_t buf[LINE_RING];
  int k, i, done;

  if (m_ptr->m_type == DEV_WRITE) {
	for (done = 0; done < count; done += k) {
		k = count - done < LINE_RING ? count - done : LINE_RING;
		phys_copy(user + done, vir2phys(buf), (phys_bytes) k);
		for (i = 0; i < k; i++) hal_line_putc(buf[i]);
	}
	return(count);
  }
  /* The empty test and ln_rd under lock: an RX interrupt between them
   * would fill the ring without a wakeup (ln_rd still FALSE), and with
   * the last bytes of a command in it the reader would sleep for good,
   * since the PC waits for the answer (M2's 60 s mtpd hang, #330). */
  lock();
  if (ln_tail != ln_head) {
	unlock();
	return(line_give(m_ptr->PROC_NR, (vir_bytes) m_ptr->ADDRESS, count));
  }
  if (m_ptr->TTY_FLAGS & O_NONBLOCK) { unlock(); return(EAGAIN); }
  if (ln_rd) { unlock(); return(EIO); }	/* one reader at a time */
  ln_rd = TRUE;
  ln_caller = m_ptr->m_source;
  ln_proc = m_ptr->PROC_NR;
  ln_vir = (vir_bytes) m_ptr->ADDRESS;
  ln_count = count;
  unlock();
  return(SUSPEND);
}


/*===========================================================================*
 *				line_check				     *
 *===========================================================================*/
PRIVATE void line_check()
{
/* From do_hardint: bytes for the waiting reader. */
  if (ln_rd && ln_tail != ln_head) {
	ln_rd = FALSE;
	reply(REVIVE, ln_caller, ln_proc, line_give(ln_proc, ln_vir, ln_count));
  }
}
