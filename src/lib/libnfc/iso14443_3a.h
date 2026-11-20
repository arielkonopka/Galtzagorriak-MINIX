/* iso14443_3a.h - the ISO 14443-3A (NFC-A) poller: REQA, the bit-frame
 * anticollision cascade and SELECT.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14 (R14.*), issue #387.
 *
 * The reader side of ISO 14443-3A, as stock's lib/nfc/protocols/iso14443_3a
 * does it, over one nfc_xfer_t.  It raises no field and frees nothing: the
 * caller owns the field (NFCIOCFIELD) and the small result struct.  A
 * single, double or triple UID cascade is walked (4, 7 or 10-byte UIDs);
 * the ATQA and SAK come back so the caller can tell MIFARE Classic from
 * Ultralight/NTAG and hand off to the right protocol.
 */
#ifndef _LIBNFC_ISO14443_3A_H
#define _LIBNFC_ISO14443_3A_H

#include <stdint.h>
#include "nfc_xfer.h"

/* Short frame commands (7 bits, NFC_XF_SHORT). */
#define ISO14443_3A_REQA	0x26
#define ISO14443_3A_WUPA	0x52

/* SELECT cascade level commands. */
#define ISO14443_3A_SEL_CL1	0x93
#define ISO14443_3A_SEL_CL2	0x95
#define ISO14443_3A_SEL_CL3	0x97
#define ISO14443_3A_CT		0x88	/* cascade tag, prefixes a partial UID */

typedef struct iso14443_3a_tag {
	uint8_t atqa[2];	/* answer to REQA */
	uint8_t sak;		/* final SELECT acknowledge */
	uint8_t uid[10];	/* 4, 7 or 10 bytes */
	uint8_t uid_len;
} iso14443_3a_tag_t;

/* REQA (or WUPA) -> ATQA.  Returns NFC_OK and fills atqa[2], or an error.
 * `wake` != 0 sends WUPA (wakes a halted card) instead of REQA. */
int iso14443_3a_request(const nfc_xfer_t *x, int wake, uint8_t atqa[2]);

/* The whole activation: REQA, anticollision + SELECT over every cascade
 * level, into *tag.  Returns NFC_OK on a clean single-card activation.
 * (Bit-wise collision resolution of several cards is left for later; one
 * card in the field is the common case and what the host tests exercise.) */
int iso14443_3a_poll(const nfc_xfer_t *x, int wake, iso14443_3a_tag_t *tag);

#endif /* _LIBNFC_ISO14443_3A_H */
