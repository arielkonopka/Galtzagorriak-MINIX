/* <minix/ibutton.h> - the iButton contacts (Galtzagorriak MINIX, R6.7)
 *
 * read(/dev/ibutton) gives the 8-byte ROM of a Dallas key on the contacts
 * (ENODEV: none).  IBIOCRUN runs a few 1-Wire master steps in a row, for
 * programs that talk to other 1-Wire parts (writing an RW1990 blank: the
 * ikey command).  Each bit slot masks interrupts for at most ~70 us; pauses
 * the parts need (EEPROM programming) are the caller's, between calls.
 * IBIOCEMU makes the Flipper the key instead (R6.7 part 3).
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

#endif /* _MINIX_IBUTTON_H */
