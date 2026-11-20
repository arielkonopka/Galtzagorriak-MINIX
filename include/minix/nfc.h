/* nfc.h - the NFC front end, /dev/nfc (spec Phase 13, R13.1-R13.3;
 * kernel/arm/flipdev.c on the ST25R3916).  Group `nfc`.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This is the one small interface the ST25R3916 driver (lane:local) gives
 * the NFC stack (src/lib/libnfc, lane:cloud).  libnfc speaks only through
 * the `nfc_xfer` transceiver (<libnfc/nfc_xfer.h>); on the Flipper that
 * transceiver is this device, in a host test it is a C mock, in QEMU a
 * mock front end.  So the stack above is written and tested now and meets
 * the driver here later.  The driver is NOT written by the cloud worker
 * (Autonomy: a new hardware path merges off by default).  Full notes:
 * docs/devguide/nfc-driver-interface.md.
 *
 * The chip moves frames between its 512 B FIFO and the user buffer with no
 * SRAM copy; the kernel keeps only a small mode word, so the device stays
 * within the Phase 13 budget (kernel <= +200 B).  Exclusive with
 * /dev/lfrfid (shared SPI1 and antenna path).
 *
 * Poller (reader) use:
 *	NFCIOCFIELD 1		raise the field
 *	NFCIOCPOLL  &tech	technologies seen (bitmask), 0 none
 *	write(fd, frame, n)  +  NFCIOCXFER / read(fd, ...)   transceive
 *	NFCIOCFIELD 0		drop the field
 * Listener (card emulation) use: NFCIOCLISTEN with the anticollision
 * values; the chip answers REQA/ATQA/SELECT by itself (R13.2) and read()
 * then returns each later reader frame, write() sends the response within
 * the frame-wait time.  Transparent mode (R13.3) is for MIFARE Classic
 * emulation, where crypto1 needs bit-level parity.
 */
#ifndef _MINIX_NFC_H
#define _MINIX_NFC_H

#include <sys/ioctl.h>

/* Technologies, a bitmask for NFCIOCPOLL and the field modes. */
#define NFC_TECH_A	0x01		/* ISO 14443-A (and MIFARE) */
#define NFC_TECH_B	0x02		/* ISO 14443-B */
#define NFC_TECH_F	0x04		/* FeliCa (NFC-F) */
#define NFC_TECH_V	0x08		/* ISO 15693 (NFC-V) */

/* NFCIOCFIELD: !=0 raise the RF field and make this a poller, 0 drop it.
 * The argument is the technologies to poll for (NFC_TECH_*); 0 with a
 * non-zero still-truthy value is rejected EINVAL, use NFC_TECH_A at least. */
#define NFCIOCFIELD	_IOW('N', 1, int)

/* NFCIOCPOLL: run one poll cycle for the technologies set by NFCIOCFIELD;
 * returns the bitmask of those that answered (0 none).  Non-blocking:
 * the chip's own timers bound it.  EIO if the field is down. */
#define NFCIOCPOLL	_IOR('N', 2, int)

/* A transceive: write() the frame to send, then NFCIOCXFER to send it and
 * wait for the answer, then read() the answer.  Or NFCIOCXFER alone with
 * the buffer in `tx`/`rx` for one call.  `flags` are NFC_XF_*; `tx_bits`
 * and `rx_bits` count bits, not bytes, so 7-bit short frames (REQA) and
 * odd parity work.  `rx_bits` is set on return.  Timeout in the chip's
 * fc units (NFC_FC) or 0 for the technology default. */
struct nfc_xfer_io {
	void *tx;			/* frame to send (caller's memory) */
	void *rx;			/* where the answer goes */
	unsigned tx_bits;		/* bits to send */
	unsigned rx_bits;		/* in: room in rx; out: bits received */
	unsigned fwt;			/* frame-wait time, fc units, 0 = default */
	unsigned flags;			/* NFC_XF_* */
};
#define NFCIOCXFER	_IOW('N', 3, struct nfc_xfer_io)

/* xfer flags (also the `flags` of nfc_xfer() in <libnfc/nfc_xfer.h>). */
#define NFC_XF_CRC_TX	0x01		/* append CRC to the sent frame */
#define NFC_XF_CRC_RX	0x02		/* check and strip CRC on receive */
#define NFC_XF_PARITY	0x04		/* keep per-byte parity (transparent) */
#define NFC_XF_NO_RX	0x08		/* send only, do not wait for a reply */
#define NFC_XF_SHORT	0x10		/* a 7-bit short frame (REQA/WUPA) */

/* 1 fc = 1 / 13.56 MHz; the ISO 14443 timing is counted in fc units. */
#define NFC_FC		13560000UL

/* NFCIOCLISTEN: become a card (R13.2).  The chip answers the anticollision
 * from these registers by itself; read()/write() then carry the later
 * frames.  A zero `uid_len` (or a close) stops listening. */
struct nfc_listen {
	unsigned char atqa[2];		/* answer to REQA */
	unsigned char sak;		/* select acknowledge */
	unsigned char uid[10];		/* 4, 7 or 10 bytes */
	unsigned char uid_len;
	unsigned char tech;		/* NFC_TECH_* to listen as */
};
#define NFCIOCLISTEN	_IOW('N', 4, struct nfc_listen)

/* NFCIOCTRANSP: raw transparent mode (R13.3) for MIFARE Classic emulation,
 * where crypto1 needs every bit and its parity.  !=0 on, 0 off; while on,
 * NFCIOCXFER with NFC_XF_PARITY moves bits unframed. */
#define NFCIOCTRANSP	_IOW('N', 5, int)

/* NFCIOCFEAT: what the front end under this fd can do (a mock reports less
 * than the chip).  A bitmask of NFC_TECH_* OR'd with the NFC_CAN_* bits. */
#define NFC_CAN_LISTEN	0x100		/* card emulation (R13.2) */
#define NFC_CAN_TRANSP	0x200		/* transparent mode (R13.3) */
#define NFC_CAN_FIELDDET 0x400		/* external-field detect (R14.2.1) */
#define NFCIOCFEAT	_IOR('N', 6, int)

/* Test hooks, QEMU and the host mock only (ENOTTY on the Flipper): the
 * next transceive's answer is taken from this buffer instead of the air,
 * so the stack runs end to end under QEMU against a scripted card. */
struct nfc_fake {
	const void *rx;			/* bytes the next read/xfer returns */
	unsigned rx_bits;
};
#define NFCIOCFAKE	_IOW('N', 7, struct nfc_fake)

#endif /* _MINIX_NFC_H */
