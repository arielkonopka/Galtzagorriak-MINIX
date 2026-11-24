/* mf_dict.h - the MIFARE Classic key dictionary, read as a stream.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #387.
 *
 * Keys are read one line at a time from /home/flipper/nfc/assets (the
 * system dictionary stock ships, mf_classic_dict.nfc, and the user's
 * mf_classic_dict_user.nfc) - never the whole file at once (R14 memory
 * rule).  A line is 12 hex digits; "#" starts a comment, blank lines are
 * skipped.  D.3: the system dictionary is public data stock itself ships,
 * so it stays; the user's own file works as in stock.
 */
#ifndef _LIBNFC_MF_DICT_H
#define _LIBNFC_MF_DICT_H

#include <stdio.h>
#include "mf_classic.h"

#define MF_DICT_SYS	"/home/flipper/nfc/assets/mf_classic_dict.nfc"
#define MF_DICT_USER	"/home/flipper/nfc/assets/mf_classic_dict_user.nfc"

typedef struct mf_dict {
	FILE *fp;
	unsigned long line;	/* line number, for an error message */
} mf_dict_t;

/* Open a dictionary file.  Returns 0 on success, -1 (errno set) if the
 * file is missing - the caller then says which file, as stock does. */
int mf_dict_open(mf_dict_t *d, const char *path);
void mf_dict_close(mf_dict_t *d);
void mf_dict_rewind(mf_dict_t *d);

/* Read the next key.  Returns 1 and fills *k, 0 at end of file, -1 on a
 * malformed line (d->line is the line number).  Malformed lines may be
 * skipped by the caller (stock warns and goes on). */
int mf_dict_next(mf_dict_t *d, mf_key_t *k);

/* Count the keys without holding them (one streaming pass, then rewind). */
unsigned long mf_dict_count(mf_dict_t *d);

#endif /* _LIBNFC_MF_DICT_H */
