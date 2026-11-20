/* nfc_xfer.h - the one interface the NFC protocol stack sits on.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14 (R14.*), issue #387.
 *
 * Every protocol in libnfc (ISO 14443-3A/4A, MIFARE Classic, Ultralight/
 * NTAG, DESFire, ISO 15693) talks to the card only through one function
 * pointer, `nfc_xfer`, plus a few timing hooks.  Three things implement it:
 *
 *   - the ST25R3916 driver on the Flipper (/dev/nfc, <minix/nfc.h>),
 *     lane:local, written later against this header;
 *   - a software card model on the host (flipper/tests/nfc), for the unit
 *     tests of this phase;
 *   - a QEMU mock front end, so `nfc` runs end to end there.
 *
 * The flags are the NFC_XF_* of <minix/nfc.h> (same values), so the driver
 * passes them straight to the ioctl.  Bits, not bytes, are the unit: a REQA
 * is 7 bits, MIFARE Classic parity is per byte.
 */
#ifndef _LIBNFC_NFC_XFER_H
#define _LIBNFC_NFC_XFER_H

#include <stddef.h>
#include <stdint.h>
#include <minix/nfc.h>		/* NFC_XF_*, NFC_TECH_*, NFC_FC */

/* Result of a transceive. */
enum nfc_err {
	NFC_OK = 0,
	NFC_ERR_TIMEOUT = -1,	/* no answer within the frame-wait time */
	NFC_ERR_CRC = -2,	/* CRC or parity wrong */
	NFC_ERR_PROTO = -3,	/* framing / collision / NAK */
	NFC_ERR_NOTSUP = -4,	/* the front end cannot do this */
	NFC_ERR_OVERFLOW = -5,	/* answer longer than rx room */
	NFC_ERR_IO = -6,	/* lower layer / device error */
};

/* The transceiver.  `ctx` is the front end's own state (a /dev/nfc fd on
 * the Flipper, a struct mf_classic* in a host test).  Send tx_bits bits
 * from tx, wait fwt fc units (0 = technology default), put the answer in
 * rx (rx_room bits of room) and the count in *rx_bits.  Returns nfc_err.
 * No allocation: both buffers are the caller's. */
typedef struct nfc_xfer {
	int (*transceive)(
		void *ctx,
		const uint8_t *tx, size_t tx_bits,
		uint8_t *rx, size_t rx_room, size_t *rx_bits,
		uint32_t fwt, uint32_t flags);
	/* Field control and timing hooks (a host mock may leave them NULL). */
	int (*field)(void *ctx, int on, uint32_t tech);	/* NFCIOCFIELD */
	int (*poll)(void *ctx, uint32_t tech);		/* -> techs seen */
	void (*delay_us)(void *ctx, uint32_t us);	/* guard times */
	void *ctx;
} nfc_xfer_t;

/* Thin wrappers so the protocols read well. */
static inline int nfc_transceive(
	const nfc_xfer_t *x,
	const uint8_t *tx, size_t tx_bits,
	uint8_t *rx, size_t rx_room, size_t *rx_bits,
	uint32_t fwt, uint32_t flags)
{
	if(!x || !x->transceive) return NFC_ERR_NOTSUP;
	return x->transceive(x->ctx, tx, tx_bits, rx, rx_room, rx_bits, fwt, flags);
}

static inline int nfc_field(const nfc_xfer_t *x, int on, uint32_t tech)
{
	if(!x || !x->field) return NFC_ERR_NOTSUP;
	return x->field(x->ctx, on, tech);
}

static inline void nfc_delay_us(const nfc_xfer_t *x, uint32_t us)
{
	if(x && x->delay_us) x->delay_us(x->ctx, us);
}

/* bytes <-> bits */
#define NFC_BITS(bytes)	((bytes) * 8u)
#define NFC_BYTES(bits)	(((bits) + 7u) / 8u)

#endif /* _LIBNFC_NFC_XFER_H */
