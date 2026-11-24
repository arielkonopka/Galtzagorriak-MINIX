/* t4t.h - NFC Forum Type 4 Tag: the NDEF message of an ISO 14443-4 card.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #451 (NFC part 7).
 *
 * Read only, as the NFC Forum Type 4 Tag specification (version 2.0 and
 * later) lays it out, over iso_dep:
 *   SELECT the NDEF Tag Application  00 A4 04 00 07 D2760000850101 00
 *   SELECT the Capability Container  00 A4 00 0C 02 E103
 *   READ BINARY 15 bytes             -> CCLEN, version, MLe, MLc and the
 *                                       NDEF File Control TLV (04 06 ...)
 *   SELECT the NDEF file             00 A4 00 0C 02 <file id>
 *   READ BINARY 2 bytes (NLEN), then the message in MLe-sized pieces.
 * No write, no password (the NDEF file's read access must be 00, open).
 * The struct is the caller's (malloc'd per read, ~0.3 KB).
 */
#ifndef _LIBNFC_T4T_H
#define _LIBNFC_T4T_H

#include <stddef.h>
#include <stdint.h>
#include "iso_dep.h"

#define T4T_NDEF_MAX	256	/* the message bytes kept (the rest counted) */

typedef struct t4t {
	uint8_t ver;			/* mapping version, 0x20 = 2.0 */
	uint16_t mle, mlc;		/* the card's max R-APDU / C-APDU data */
	uint16_t fid;			/* the NDEF file's id */
	uint16_t fmax;			/* its size */
	uint8_t rd, wr;			/* access: 00 open, FF never */
	uint16_t nlen;			/* the NDEF message's length */
	uint16_t got;			/* bytes of it in ndef[] */
	uint8_t ndef[T4T_NDEF_MAX];
} t4t_t;

/* The NDEF Tag Application's AID (NFC Forum T4T 2.0). */
extern const uint8_t t4t_ndef_aid[7];

/* Read the CC and the NDEF message of an activated card (after
 * iso_dep_activate).  NFC_OK; NFC_ERR_NOTSUP if the card has no NDEF
 * application or the file is not open for reading; NFC_ERR_PROTO on a
 * malformed CC; or a transfer error. */
int t4t_read(iso_dep_t *d, t4t_t *t);

/* The first NDEF record as one line of text: "URI https://...",
 * "Text en: ...", "MIME text/plain", "Empty", or "Type <type>".
 * out gets at most n bytes (cut, always terminated). */
void t4t_ndef_summary(const uint8_t *m, size_t len, char *out, size_t n);

#endif /* _LIBNFC_T4T_H */
