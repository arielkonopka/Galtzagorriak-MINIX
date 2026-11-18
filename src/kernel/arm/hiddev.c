/* The USB HID device files (spec R10.2, R11.3.1, <minix/hid.h>), part of
 * the Flipper device task (flipdev.c includes this file): /dev/hidkbd,
 * /dev/hidmouse and /dev/hidcons, minors 17-19 of major 6.
 *
 * The task checks and forwards whole reports to the HAL backend
 * (hal_hid_send: QEMU prints a console line; the Flipper's USB stack,
 * T28b, sends it).  It keeps no report buffer: a write's reports go out
 * from the task's stack one by one.  It owns the rules the backends
 * share: report sizes and ranges, the one-persona-at-a-time lock (EBUSY),
 * poll() on a state change, and the reader of the host's LED byte.
 *
 * RAM: 8 B (the spec allows 32 for the QEMU backend; the Flipper's adds
 * its own under R10.1's budget).
 */

#define HID_FIRST	HID_KBD_MINOR
#define IS_HID(m)	((unsigned) ((m) - HID_FIRST) < 3)

#define HF_STATE	HAL_HID_EV_STATE	/* the state changed (POLLIN) */
#define HF_LED		HAL_HID_EV_LED		/* the LED byte changed */
PRIVATE volatile u8_t hid_fl;	/* HF_*; set by hid_event() (interrupt) */
PRIVATE u8_t hid_sel;		/* poll(): bit 2*k waits to read, 2*k+1 to
				 * write, k = minor - HID_FIRST */
PRIVATE u8_t hid_pers;		/* the persona holding the bus, HIDP_* */
PRIVATE u8_t hid_hold;		/*   its holder's slot + 1 */
PRIVATE u8_t hid_rdp;		/* the LED reader waiting, slot + 1 */
PRIVATE vir_bytes hid_rdvir;	/*   its buffer */

PRIVATE const u8_t hid_len[3] = { HID_KBD_LEN, HID_MOUSE_LEN, HID_CONS_LEN };

FORWARD _PROTOTYPE( int hid_ready, (void)				);
FORWARD _PROTOTYPE( int hid_rd_ready, (int k)				);
FORWARD _PROTOTYPE( void hid_led_out, (void)				);


/*===========================================================================*
 *				hid_event				     *
 *===========================================================================*/
PUBLIC void hid_event(what)
unsigned what;
{
/* The backend, at interrupt level: the state (HAL_HID_EV_STATE) or the
 * host's LED byte (HAL_HID_EV_LED) changed.  Wake the task if somebody
 * waits for it. */
  hid_fl |= what & (HF_STATE | HF_LED);
  if (hid_sel != 0 || hid_rdp != 0) interrupt(PRINTER);
}


/*===========================================================================*
 *				hid_ready				     *
 *===========================================================================*/
PRIVATE int hid_ready()
{
/* A report would go out now: configured, not suspended, no persona. */
  return hid_pers == HIDP_NONE && (hal_hid_state() & (HAL_HID_CONFIGURED
		| HAL_HID_SUSPENDED)) == HAL_HID_CONFIGURED;
}


/*===========================================================================*
 *				hid_rd_ready				     *
 *===========================================================================*/
PRIVATE int hid_rd_ready(k)
int k;				/* minor - HID_FIRST */
{
  return (hid_fl & HF_STATE) || (k == 0 && (hid_fl & HF_LED));
}


/*===========================================================================*
 *				hid_open				     *
 *===========================================================================*/
PRIVATE int hid_open(m_ptr)
message *m_ptr;
{
/* DEV_OPEN and DEV_CLOSE of a HID minor. */
  if (m_ptr->m_type == DEV_OPEN)
	return(hid_pers != HIDP_NONE ? EBUSY : OK);	/* R11.3.1 */
  if (hid_hold == m_ptr->PROC_NR + 1) {		/* the holder let go */
	hid_pers = HIDP_NONE;
	hid_hold = 0;
	lock();
	hid_fl |= HF_STATE;
	unlock();
  }
  return(OK);
}


/*===========================================================================*
 *				hid_rw					     *
 *===========================================================================*/
PRIVATE int hid_rw(m_ptr, user, count)
message *m_ptr;
phys_bytes user;		/* the caller's buffer, mapped */
int count;
{
  u8_t rep[HID_MAXWRITE];
  int k = m_ptr->DEVICE - HID_FIRST, len = hid_len[k], i;

  if (m_ptr->m_type == DEV_READ) {
	if (k != 0) return(EINVAL);		/* only the keyboard's LEDs */
	if (hid_fl & HF_LED) {
		lock();
		hid_fl &= ~HF_LED;
		unlock();
		rep[0] = hal_hid_state() >> 16;
		phys_copy(vir2phys(rep), user, (phys_bytes) 1);
		return(1);
	}
	if (m_ptr->TTY_FLAGS & O_NONBLOCK) return(EAGAIN);
	if (hid_rdp != 0) return(EIO);		/* one reader at a time */
	hid_rdp = m_ptr->PROC_NR + 1;
	hid_rdvir = (vir_bytes) m_ptr->ADDRESS;
	return(SUSPEND);
  }
  if (count % len != 0 || count > HID_MAXWRITE) return(EINVAL);
  if (hid_pers != HIDP_NONE) return(EBUSY);
  phys_copy(user, vir2phys(rep), (phys_bytes) count);
  if (k == 1) {			/* dx, dy, wheel: -127..127 (R11.3.1) */
	for (i = 0; i < count; i += len)
		if (rep[i + 1] == 0x80 || rep[i + 2] == 0x80
					|| rep[i + 3] == 0x80) return(EINVAL);
  }
  for (i = 0; i < count; i += len)
	if (hal_hid_send((unsigned) k, rep + i, (unsigned) len) < 0)
		return(i > 0 ? i : EAGAIN);
  return(count);
}


/*===========================================================================*
 *				hid_ioctl				     *
 *===========================================================================*/
PRIVATE int hid_ioctl(m_ptr)
message *m_ptr;
{
  phys_bytes user;
  u32_t st;
  int v;

  switch (m_ptr->TTY_REQUEST) {
  case HIDIOCSTATE: case HIDIOCSPERSONA: case HIDIOCFAKE: case HIDIOCLINE:
	break;
  default:
	return(ENOTTY);
  }
  user = numap(m_ptr->PROC_NR, (vir_bytes) m_ptr->ADDRESS, sizeof(int));
  if (user == 0) return(EFAULT);
  switch (m_ptr->TTY_REQUEST) {
  case HIDIOCSTATE:
	lock();
	hid_fl &= ~HF_STATE;
	unlock();
	st = hal_hid_state();
	v = (int) ((st & 0xFF0000) | (u32_t) hid_pers << 8 | (st & 0xFF));
	break;
  case HIDIOCSPERSONA:
	phys_copy(user, vir2phys(&v), (phys_bytes) sizeof(int));
	if (v < HIDP_NONE || v > 0xFF) return(EINVAL);
	if (hid_hold != 0 && hid_hold != m_ptr->PROC_NR + 1) return(EBUSY);
	hid_pers = v;
	hid_hold = v == HIDP_NONE ? 0 : m_ptr->PROC_NR + 1;
	lock();
	hid_fl |= HF_STATE;
	unlock();
	return(OK);
  case HIDIOCFAKE:			/* QEMU: play the host */
	phys_copy(user, vir2phys(&v), (phys_bytes) sizeof(int));
	st = hal_hid_state();
	if (hal_hid_fake((u32_t) v) < 0) return(ENOTTY);
	v = (int) (st ^ hal_hid_state());
	lock();
	if (v & 0xFF) hid_fl |= HF_STATE;
	if (v & 0xFF0000) hid_fl |= HF_LED;
	unlock();
	return(OK);
  case HIDIOCLINE:
	if (hal_hid_line(&st) < 0) return(ENOTTY);
	v = (int) st;
	break;
  }
  phys_copy(vir2phys(&v), user, (phys_bytes) sizeof(int));
  return(OK);
}


/*===========================================================================*
 *				hid_select				     *
 *===========================================================================*/
PRIVATE int hid_select(m_ptr)
message *m_ptr;
{
/* DEV_SELECT (R4b.6) of a HID minor: readable on a change, writable when
 * a report would go out; with SEL_NOTIFY remember what is not ready. */
  int k = m_ptr->DEVICE - HID_FIRST, ops = m_ptr->COUNT, r;

  if (ops == 0) {
	hid_sel &= ~(3 << 2 * k);
	return(0);
  }
  r = ops & (SEL_RD | SEL_WR);
  if (!hid_rd_ready(k)) r &= ~SEL_RD;
  if (!hid_ready()) r &= ~SEL_WR;
  if (ops & SEL_NOTIFY) {
	if ((ops & SEL_RD) && !(r & SEL_RD)) hid_sel |= 1 << 2 * k;
	if ((ops & SEL_WR) && !(r & SEL_WR)) hid_sel |= 2 << 2 * k;
  }
  return(r);
}


/*===========================================================================*
 *				hid_check				     *
 *===========================================================================*/
PRIVATE void hid_check()
{
/* In the task, after each message and interrupt: what the pollers and the
 * LED reader wait for has happened, tell them once. */
  int k, w;

  if (hid_rdp != 0 && (hid_fl & HF_LED)) hid_led_out();
  if (hid_sel == 0) return;
  w = hid_ready();
  for (k = 0; k < 3; k++) {
	if ((hid_sel & 1 << 2 * k) && hid_rd_ready(k)) {
		hid_sel &= ~(1 << 2 * k);
		reply(DEV_SELECTED, FS_PROC_NR, HID_FIRST + k, SEL_RD);
	}
	if ((hid_sel & 2 << 2 * k) && w) {
		hid_sel &= ~(2 << 2 * k);
		reply(DEV_SELECTED, FS_PROC_NR, HID_FIRST + k, SEL_WR);
	}
  }
}


/*===========================================================================*
 *				hid_led_out				     *
 *===========================================================================*/
PRIVATE void hid_led_out()
{
/* The LED byte changed: the waiting reader gets it. */
  phys_bytes user;
  int p = hid_rdp - 1, r = 1;
  u8_t led;

  hid_rdp = 0;
  lock();
  hid_fl &= ~HF_LED;
  unlock();
  led = hal_hid_state() >> 16;
  if ((user = numap(p, hid_rdvir, (vir_bytes) 1)) == 0) r = EFAULT;
  else phys_copy(vir2phys(&led), user, (phys_bytes) 1);
  reply(REVIVE, FS_PROC_NR, p, r);
}
