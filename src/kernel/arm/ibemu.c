/* The 1-Wire slave of a Dallas key (R6.7 part 3): the protocol, apart
 * from the pin.  flipdev.c calls ie_step() at every edge of the iButton
 * contact (EXTI, both edges) and does what it answers; the host test
 * (flipper/tests/host/ibemu_test.c) feeds it a reader's slots.
 *
 * Standard speed, as a reader drives it (times are the low part):
 *   reset      >= 480 us low; the key answers with presence after it
 *              (we: 20 us, then 120 us low)
 *   write 1    1..15 us low;   write 0   60..120 us low
 *   read       1..15 us low; the key holds the line low until ~30 us
 *              after the start for a 0, leaves it for a 1
 * Commands: READ ROM (0x33, old 0x0F): the 64 ROM bits, LSB first.
 * SEARCH ROM (0xF0): per ROM bit the key sends the bit, then its
 * complement, then reads the reader's choice and stays only if it is its
 * own bit.  Anything else: quiet until the next reset.
 *
 * Edges: a falling edge starts a slot (a bit the key sends is decided
 * then: a 0 is held low at once); the rising edge ends it, and its low
 * time says what the reader wrote.  The slot's bit counter moves at the
 * rising edge, so the key's own release (end of a held 0) is that edge.
 * A rising edge without its falling one (the interrupt came after a short
 * pulse was over) counts as a short low: a 1 written, or a read slot the
 * key could not answer with a 0 - then it is quiet until the next reset,
 * and the reader tries again.
 */
#define IE_OFF		0	/* not emulating */
#define IE_IDLE		1	/* waiting for a reset */
#define IE_CMD		2	/* reading the command byte */
#define IE_SEND		3	/* READ ROM: sending 64 bits */
#define IE_SEARCH	4	/* SEARCH ROM: 64 x (bit, ~bit, choice) */
#define IE_PLAY		5	/* not a Dallas key: a Cyfral or Metakom
				 * pulse list plays (IBIOCPLAY, R6.7 part 3b) */

#define IE_RESET_US	380	/* longer low: a reset (standard: 480) */
#define IE_ONE_US	15	/* shorter low: a 1 written */

#define IE_HOLD0	1	/* ie_step: hold the line low IE_HOLD_US now */
#define IE_PRESENCE	2	/* ie_step: answer the reset with presence */
#define IE_HOLD_US	30
#define IE_PRES_WAIT_US	20
#define IE_PRES_US	120

struct ie_state {
  u8_t rom[8];		/* the key's ROM, family code first */
  u8_t st;		/* IE_* above */
  u8_t bit;		/* READ ROM: bit 0..63; SEARCH: 3 * bit + phase */
  u8_t cmd;		/* command bits so far */
  u8_t low;		/* a falling edge was seen, its rising one not yet */
  u8_t served;		/* ROMs sent whole (READ ROM or a search to its
			 * end), mod 256 */
  u8_t proc;		/* who emulates, slot + 1 (its close ends it) */
  u16_t fall;		/* hal_ow_now() at the falling edge (us, mod 2^16) */
};

#define IE_ROMBIT(e, i)	((e)->rom[(i) >> 3] >> ((i) & 7) & 1)

/* One edge: `rising` 0 for a falling edge, else the end of a low that
 * lasted `us` microseconds (0 if its falling edge was missed).  Returns
 * what the caller must do on the line at once: 0, IE_HOLD0, IE_PRESENCE. */
PRIVATE int ie_step(e, rising, us)
struct ie_state *e;
int rising;
unsigned us;
{
  int b;

  if (e->st == IE_OFF || e->st == IE_PLAY) return(0);
  if (!rising) {			/* a slot starts */
	e->low = 1;
	if (e->st == IE_SEND) b = IE_ROMBIT(e, e->bit);
	else if (e->st == IE_SEARCH && e->bit % 3 != 2) {
		b = IE_ROMBIT(e, e->bit / 3);
		if (e->bit % 3 == 1) b = !b;		/* the complement */
	} else return(0);			/* the reader writes */
	return(b ? 0 : IE_HOLD0);
  }

  if (!e->low) us = 0;			/* the fall was missed: short */
  e->low = 0;
  if (us >= IE_RESET_US) {
	e->st = IE_CMD;
	e->bit = e->cmd = 0;
	return(IE_PRESENCE);
  }
  switch (e->st) {
  case IE_CMD:
	if (us < IE_ONE_US) e->cmd |= (u8_t) (1 << e->bit);
	if (++e->bit < 8) break;
	e->bit = 0;
	e->st = e->cmd == 0x33 || e->cmd == 0x0F ? IE_SEND
		: e->cmd == 0xF0 ? IE_SEARCH : IE_IDLE;
	break;
  case IE_SEND:
	if (us == 0 && !IE_ROMBIT(e, e->bit)) { e->st = IE_IDLE; break; }
	if (++e->bit == 64) { e->st = IE_IDLE; e->served++; }
	break;
  case IE_SEARCH:
	b = e->bit % 3;
	if (b == 2) {			/* the reader's choice */
		if ((us < IE_ONE_US) != IE_ROMBIT(e, e->bit / 3)) {
			e->st = IE_IDLE;	/* another key's branch */
			break;
		}
	} else if (us == 0) {		/* a 0 we could not hold */
		int want = IE_ROMBIT(e, e->bit / 3);

		if (b == 1) want = !want;
		if (!want) { e->st = IE_IDLE; break; }
	}
	if (++e->bit == 192) { e->st = IE_IDLE; e->served++; }
	break;
  }
  return(0);
}
