/* iso_dep.h - ISO 14443-4 (ISO-DEP, "T=CL") over NFC-A for the reader:
 * RATS/ATS, I-blocks with chaining both ways, S(WTX), S(DESELECT).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #451 (NFC part 7).
 *
 * The hand-written twin of stock's iso14443_4a poller, small enough for
 * the nfc app on the device: one struct (the caller's, malloc'd per use,
 * ~0.35 KB), no globals, no allocation.  It sits on nfc_xfer (CRC_A done by
 * the front end: NFC_XF_CRC_TX | NFC_XF_CRC_RX).  No CID, no NAD: one card
 * in the field, as the rest of libnfc.  The 3A activation (iso14443_3a)
 * comes first and is the caller's; iso_dep_activate() sends RATS.
 *
 * Block numbers follow ISO/IEC 14443-4:2018 7.5.4: the reader toggles its
 * block number on every I-block or R(ACK) it receives with the current
 * number.  On a timeout or a CRC error the reader sends R(NAK) once
 * (rule 4), then gives up and returns the error; the caller decides
 * whether to re-activate.
 */
#ifndef _LIBNFC_ISO_DEP_H
#define _LIBNFC_ISO_DEP_H

#include <stddef.h>
#include <stdint.h>
#include "nfc_xfer.h"

#define ISO_DEP_ATS_MAX		20	/* TL is at most 20 with FSD 256 */
#define ISO_DEP_FSD		256	/* we announce FSDI 8 (256 bytes) */
#define ISO_DEP_RATS		0xE0
#define ISO_DEP_TX_MAX		64	/* our blocks out: commands are short,
					 * longer ones are chained */

typedef struct iso_dep {
	const nfc_xfer_t *x;
	uint8_t ats[ISO_DEP_ATS_MAX];	/* TL first, as received (no CRC) */
	uint8_t ats_len;
	uint8_t bn;			/* the reader's block number, 0/1 */
	uint16_t fsc;			/* the card's frame size (bytes) */
	uint32_t fwt;			/* frame waiting time, fc units */
	unsigned wtx;			/* S(WTX) requests seen (statistics) */
	uint8_t tx[ISO_DEP_TX_MAX];	/* one block out (PCB + INF) */
	uint8_t rx[ISO_DEP_FSD];	/* one block in (PCB + INF, no CRC) */
} iso_dep_t;

/* The FSC of an FSCI (ISO 14443-4 Table 1); 256 for FSCI >= 8. */
uint16_t iso_dep_fsc(uint8_t fsci);

/* Parse an ATS (TL first) into fsc and fwt.  NFC_OK or NFC_ERR_PROTO. */
int iso_dep_parse_ats(iso_dep_t *d, const uint8_t *ats, size_t n);

/* RATS (FSDI 8, CID 0) on an activated 3A card -> ATS in d->ats.
 * Returns NFC_OK, or the transceive error. */
int iso_dep_activate(iso_dep_t *d, const nfc_xfer_t *x);

/* One command (an APDU or a native command) in, the whole answer out:
 * the command is split into I-blocks of at most fsc - 3 bytes, the
 * answer's chain is gathered (R(ACK) per block), S(WTX) is answered.
 * resp gets at most room bytes; *rn the answer's length.  A longer answer
 * gives NFC_ERR_OVERFLOW, *rn = room, and resp holds its first room - 2
 * bytes and its last two (an APDU's status word stays readable).
 * Returns NFC_OK or an nfc_err. */
int iso_dep_xchg(iso_dep_t *d, const uint8_t *cmd, size_t n,
		 uint8_t *resp, size_t room, size_t *rn);

/* S(DESELECT): the card goes to HALT.  Errors are ignored. */
void iso_dep_deselect(iso_dep_t *d);

/* An APDU's status word (the last two bytes), or 0 if rn < 2. */
static inline uint16_t iso_dep_sw(const uint8_t *resp, size_t rn)
{
	return rn < 2 ? 0 : (uint16_t)(resp[rn - 2] << 8 | resp[rn - 1]);
}

#endif /* _LIBNFC_ISO_DEP_H */
