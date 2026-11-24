/* SPDX-License-Identifier: GPL-3.0-or-later
 * nfcloop.h - the software cards of `nfc -L` (nfcloop.c). */
#ifndef NFCLOOP_H
#define NFCLOOP_H

#include "nfc_xfer.h"

#define NFCLOOP_NONE	0		/* nothing in the field */
#define NFCLOOP_MFC1K	1		/* MIFARE Classic 1K */
#define NFCLOOP_NTAG215	2		/* NTAG215 */
#define NFCLOOP_T4T	3		/* ISO 14443-4 Type 4 Tag (#451) */

struct nfcloop;
struct nfcloop *nfcloop_new(void);	/* malloc'd, NFCLOOP_NONE */
void nfcloop_card(struct nfcloop *k, int kind);	/* put a card in the field */
int nfcloop_kind(const struct nfcloop *k);
void nfcloop_xfer(struct nfcloop *k, nfc_xfer_t *x);	/* x talks to it */

#endif
