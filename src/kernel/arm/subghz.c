/* /dev/subghz - the CC1101 Sub-GHz radio (Galtzagorriak MINIX)
 *
 * Minor 8 of the Flipper device task (flipdev.c).  The chip is driven by
 * the programs themselves, register by register, so they can set it up any
 * way they like (all modulations, filters, data rates of the CC1101); the
 * kernel only does the bus, picks the antenna filter for the frequency,
 * and keeps the transmitter inside the bands allowed in the device's
 * region (the stock firmware's tables, chosen by the factory OTP).
 *
 * write: records, each a length byte and that many bytes sent as one SPI
 *	transaction (CS low around it), in order; a length of 0 is followed
 *	by one byte: wait that many times 10 us.  At most 256 bytes.
 * read: what came back from the last write's transactions, concatenated
 *	(the chip's status byte for each header, then the data).
 *
 * The transmitter: a transaction is refused (EPERM) if it would send
 * outside the allowed bands - an STX strobe, or the state machine set to
 * go to TX by itself (MCSM1), while the frequency (FREQ, CHANNR, channel
 * spacing, FREQOFF) is not allowed.  Writes that move the frequency out of
 * the bands put the chip to IDLE first, so a running transmitter stops.
 * Receiving is allowed on every frequency.
 */
#include "../kernel.h"
#include "hal.h"

#define RF_MAXW		256		/* bytes per write */
#define NREG		0x2F		/* configuration registers */
#define XOSC		26000000ULL	/* the CC1101's crystal */

#define R_IOCFG2	0x00
#define R_CHANNR	0x0A
#define R_FSCTRL0	0x0C
#define R_FREQ2		0x0D
#define R_FREQ1		0x0E
#define R_FREQ0		0x0F
#define R_MDMCFG1	0x13
#define R_MDMCFG0	0x14
#define R_MCSM1		0x17
#define S_SRES		0x30
#define S_STX		0x35
#define S_SIDLE		0x36
#define A_PATABLE	0x3E
#define A_FIFO		0x3F
#define RD		0x80
#define BURST		0x40

#define IOCFG_HW	0x2F		/* GDO2 constant low ... */
#define IOCFG_INV	0x40		/* ... or high */

struct band { u32_t lo, hi; };

/* Where the stock firmware lets each region transmit (furi_hal_region.c);
 * an unknown or "world" device gets the European bands. */
PRIVATE struct band eu[] = { { 433050000, 434790000 },
			     { 868150000, 868550000 }, { 0, 0 } };
PRIVATE struct band us[] = { { 304100000, 321950000 },
			     { 433050000, 434790000 },
			     { 915000000, 928000000 }, { 0, 0 } };
PRIVATE struct band jp[] = { { 312000000, 315250000 },
			     { 920500000, 923500000 }, { 0, 0 } };
PRIVATE struct band *bands;

PRIVATE u8_t reg[NREG];			/* as written to the chip */
PRIVATE u8_t rf_buf[RF_MAXW];		/* the write, then what came back */
PRIVATE int rf_nresp;
PRIVATE int rf_path = -1;

FORWARD _PROTOTYPE( void rf_defaults, (void)				);
FORWARD _PROTOTYPE( u32_t rf_freq, (u8_t *r)				);
FORWARD _PROTOTYPE( int rf_allowed, (u32_t f)				);
FORWARD _PROTOTYPE( int freq_reg, (int a)				);
FORWARD _PROTOTYPE( void rf_setpath, (void)				);
FORWARD _PROTOTYPE( int rf_check, (u8_t *p, int n, int *idle)		);


/*===========================================================================*
 *				subghz_init				     *
 *===========================================================================*/
PUBLIC void subghz_init()
{
  static u8_t sres[1] = { S_SRES };
  static u8_t spwd[1] = { 0x39 };

  switch (hal_rf_region()) {
  case 2: bands = us; break;
  case 3: bands = jp; break;
  default: bands = eu;
  }
  if (hal_rf_xfer(sres, NULL, 1) < 0) return;	/* no radio */
  rf_defaults();
  rf_setpath();
  hal_rf_xfer(spwd, NULL, 1);			/* sleep until used */
}


/*===========================================================================*
 *				subghz_rw				     *
 *===========================================================================*/
PUBLIC int subghz_rw(write, user, count)
int write;
phys_bytes user;
int count;
{
  static u8_t sidle[1] = { S_SIDLE };
  u8_t saved[NREG];
  u8_t *p, n;
  int i, idle = 0, r;
  u32_t t;
  u32_t cyc_us = hal_cpu_hz() / 1000000;

  if (!write) {
	if (count > rf_nresp) count = rf_nresp;
	phys_copy(vir2phys(rf_buf), user, (phys_bytes) count);
	return(count);
  }
  if (count > RF_MAXW) return(EINVAL);
  phys_copy(user, vir2phys(rf_buf), (phys_bytes) count);

  /* Check the whole write first: nothing is sent if any of it is bad. */
  memcpy(saved, reg, sizeof(reg));
  for (i = 0; i < count; i += 1 + n) {
	n = rf_buf[i];
	r = OK;
	if (n == 0) {
		if (++i >= count) r = EINVAL;
	} else if (i + 1 + n > count) {
		r = EINVAL;
	} else {
		r = rf_check(rf_buf + i + 1, n, &idle);
	}
	if (r != OK) {
		memcpy(reg, saved, sizeof(reg));
		return(r);
	}
  }
  if (idle) hal_rf_xfer(sidle, NULL, 1);

  /* Send; the answers replace the records' contents, in place. */
  rf_nresp = 0;
  for (i = 0; i < count; i += 1 + n) {
	n = rf_buf[i];
	if (n == 0) {
		t = hal_cycles() + rf_buf[++i] * 10 * cyc_us;
		while ((i32_t) (hal_cycles() - t) < 0) ;
		continue;
	}
	p = rf_buf + i + 1;
	if (hal_rf_xfer(p, p, n) < 0) return(EIO);
	memmove(rf_buf + rf_nresp, p, n);	/* behind the read position */
	rf_nresp += n;
  }
  rf_setpath();
  return(count);
}


/*===========================================================================*
 *				rf_check				     *
 *===========================================================================*/
PRIVATE int rf_check(p, n, idle)
u8_t *p;
int n;
int *idle;
{
/* Follow one transaction through the chip's SPI protocol: a header byte,
 * then one data byte (single access) or all the rest (burst); strobes take
 * none.  Apply the register writes to reg[] and see where the transmitter
 * would end up.  IOCFG2 belongs to the antenna switch: writes to it are
 * changed to its current setting. */
  int k = 0, a, j, sent_tx = 0, moved;
  u8_t h;

  while (k < n) {
	h = p[k++];
	a = h & 0x3F;
	moved = 0;
	if (h & RD) {				/* reads change nothing */
		if (h & BURST) break;
		k++;
		continue;
	}
	if (a >= S_SRES && a < A_PATABLE) {	/* a strobe */
		if (h & BURST) return(EINVAL);
		if (a == S_SRES) rf_defaults();
		if (a == S_SRES || a == S_SIDLE) sent_tx = 0;
		if (a == S_STX) {
			if (!rf_allowed(rf_freq(reg))) return(EPERM);
			sent_tx = 1;
		}
		continue;
	}
	if (a >= NREG) {			/* PATABLE, FIFO: data */
		if (h & BURST) break;
		k++;
		continue;
	}
	for (j = a; k < n; j++) {		/* register writes */
		if (j < NREG) {
			if (j == R_IOCFG2) p[k] = reg[R_IOCFG2];
			if (freq_reg(j)) moved = 1;
			reg[j] = p[k];
		}
		k++;
		if (!(h & BURST)) break;
	}
	if (moved && !rf_allowed(rf_freq(reg))) {
		if (sent_tx) return(EPERM);	/* moving while sending */
		*idle = 1;
	}
	/* RXOFF_MODE or TXOFF_MODE 10: go to TX by itself */
	if (((reg[R_MCSM1] >> 2) & 3) == 2 || (reg[R_MCSM1] & 3) == 2)
		if (!rf_allowed(rf_freq(reg))) return(EPERM);
  }
  return(OK);
}


/*===========================================================================*
 *				rf_defaults				     *
 *===========================================================================*/
PRIVATE void rf_defaults()
{
/* The registers that matter here, as after a reset (CC1101 data sheet). */
  memset(reg, 0, sizeof(reg));
  reg[R_IOCFG2] = 0x29;
  reg[R_FREQ2] = 0x1E;
  reg[R_FREQ1] = 0xC4;
  reg[R_FREQ0] = 0xEC;
  reg[R_MDMCFG1] = 0x22;
  reg[R_MDMCFG0] = 0xF8;
  reg[R_MCSM1] = 0x30;
  rf_path = -1;
}


/*===========================================================================*
 *				rf_freq					     *
 *===========================================================================*/
PRIVATE u32_t rf_freq(r)
u8_t *r;
{
/* The carrier: FREQ * fxosc / 2^16, plus CHANNR channel spacings
 * (fxosc / 2^18 * (256 + CHANSPC_M) * 2^CHANSPC_E), plus the FREQOFF
 * correction (signed, fxosc / 2^14). */
  unsigned long long f, sp;

  f = ((unsigned long long) r[R_FREQ2] << 16 | r[R_FREQ1] << 8 | r[R_FREQ0]) * XOSC >> 16;
  sp = (XOSC * (256 + r[R_MDMCFG0]) << (r[R_MDMCFG1] & 3)) >> 18;
  f += sp * r[R_CHANNR];
  f += ((i32_t) (signed char) r[R_FSCTRL0] * (i32_t) (XOSC >> 8)) >> 6;
  return (u32_t) f;
}


/*===========================================================================*
 *				rf_allowed				     *
 *===========================================================================*/
PRIVATE int rf_allowed(f)
u32_t f;
{
  struct band *b;

  for (b = bands; b->hi != 0; b++)
	if (f >= b->lo && f <= b->hi) return(TRUE);
  return(FALSE);
}


PRIVATE int freq_reg(a)
int a;
{
  return a == R_CHANNR || a == R_FSCTRL0 || (a >= R_FREQ2 && a <= R_FREQ0)
	|| a == R_MDMCFG1 || a == R_MDMCFG0;
}


/*===========================================================================*
 *				rf_setpath				     *
 *===========================================================================*/
PRIVATE void rf_setpath()
{
/* The antenna filter for the band (as the stock firmware's
 * furi_hal_subghz_set_path): 315 MHz: switch 1, GDO2 low; 433 MHz: switch
 * 0, GDO2 high; 868 MHz: switch 1, GDO2 high.  Between the bands, the
 * nearest one. */
  u32_t f = rf_freq(reg);
  int path = f < 370000000 ? 0 : f < 600000000 ? 1 : 2;
  u8_t w[2];

  if (path == rf_path) return;
  rf_path = path;
  hal_rf_switch(path != 1);
  w[0] = R_IOCFG2;
  w[1] = reg[R_IOCFG2] = path == 0 ? IOCFG_HW : IOCFG_HW | IOCFG_INV;
  hal_rf_xfer(w, NULL, 2);
}
