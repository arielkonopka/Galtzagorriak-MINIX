/* <minix/ibutton.h> - the iButton contacts (Galtzagorriak MINIX, R6.7)
 *
 * read(/dev/ibutton) gives the 8-byte ROM of a Dallas key on the contacts
 * (ENODEV: none).  IBIOCRUN runs a few 1-Wire master steps in a row, for
 * programs that talk to other 1-Wire parts (writing an RW1990 blank: the
 * ikey command).  Each bit slot masks interrupts for at most ~70 us; pauses
 * the parts need (EEPROM programming) are the caller's, between calls.
 * IBIOCEMU makes the Flipper the key instead (R6.7 part 3).
 * IBIOCCAPT and IBIOCPLAY carry the Cyfral and Metakom keys (part 3b):
 * the line's levels as durations, decoded and encoded by the caller
 * (src/lib/ibkey, as /dev/ir and /dev/rfid leave the protocols to user
 * space).
 */
#ifndef _MINIX_IBUTTON_H
#define _MINIX_IBUTTON_H

#include <sys/ioctl.h>

/* op[]: one byte each, IB_WBYTE takes the next byte as its value */
#define IB_W0		0x00	/* write a 0 bit */
#define IB_W1		0x01	/* write a 1 bit */
#define IB_RESET	0x80	/* reset; out[]: 1 a part answered, 0 none */
#define IB_WBYTE	0x81	/* write the next byte, LSB first */
#define IB_RBYTE	0x82	/* read a byte, LSB first, into out[] */
#define IB_MAXOP	32
#define IB_MAXOUT	16

struct ib_run {
	unsigned char op[IB_MAXOP];
	unsigned char nop;		/* ops used */
	unsigned char nout;		/* out: bytes put in out[] */
	unsigned char out[IB_MAXOUT];
};
#define IBIOCRUN	_IOW('i', 1, struct ib_run)

/* IBIOCEMU: be a Dallas key (DS1990A) for a reader on the contacts, until
 * on = 0 or the caller's close.  The ROM is 8 bytes, family code first,
 * CRC last (as read() gives it).  The reader may use READ ROM or SEARCH
 * ROM.  served (out) counts the readers that got the whole ROM, mod 256.
 * EBUSY: another process emulates; read() and IBIOCRUN give EBUSY while
 * a key is emulated. */
struct ib_emu {
	unsigned char rom[8];
	unsigned char on;
	unsigned char served;		/* out */
	unsigned char pad[2];
};
#define IBIOCEMU	_IOW('i', 2, struct ib_emu)

/* IBIOCCAPT (R6.7 part 3b): what a key on the contact sends, as the
 * levels of the line and how long each lasted.  The kernel powers the key
 * as a reader does and writes up to n durations (us, 1..65535; longer
 * ones are cut to 65535) into dur[], in the caller's memory, within about
 * `ms` ms; got (out) is how many, level (out) the level of dur[0] (1 high,
 * 0 low), the others alternate.  n: 16..IB_MAXCAPT; ms: 1..1000.
 * ENODEV: the line did not move (no key).  EBUSY: a Dallas key is being
 * emulated.  ENXIO: not on this board yet (the Flipper's comparator
 * capture is the device side's, R6.7 part 3b).
 * QEMU has no contact: there the capture sees the list another program
 * plays with IBIOCPLAY (a loopback, as if a second Flipper emulated the
 * key), from some point in it, and that player's `served` goes up. */
struct ib_capt {
	unsigned short *dur;
	unsigned n;			/* room in dur[] */
	unsigned ms;			/* at most this long */
	unsigned got;			/* out: durations written */
	int level;			/* out: the level of dur[0] */
};
#define IB_MAXCAPT	1024
#define IBIOCCAPT	_IOW('i', 3, struct ib_capt)

/* IBIOCPLAY (R6.7 part 3b): be a Cyfral or Metakom key: the contact plays
 * dur[0..n-1] (us, 1..65535) round and round, starting with `level` (1
 * released, high; 0 pulled low), the levels alternating; n even, 2 ..
 * IB_MAXPLAY.  dur[] stays in the caller's memory, pinned, and must not
 * change while it plays (ask again with a new list instead).  n = 0, the
 * caller's close or exit stops it.  served (out) as IBIOCEMU's: how many
 * times a reader read the key, mod 256 (QEMU: an IBIOCCAPT).  EBUSY:
 * another program emulates a key; read(), IBIOCRUN and IBIOCEMU give
 * EBUSY while this plays.  ENXIO: not on this board yet. */
struct ib_play {
	unsigned short *dur;
	unsigned n;			/* durations; 0 stops */
	int level;			/* the level of dur[0] */
	unsigned char served;		/* out */
	unsigned char pad[3];
};
#define IB_MAXPLAY	256
#define IBIOCPLAY	_IOW('i', 4, struct ib_play)

#endif /* _MINIX_IBUTTON_H */
