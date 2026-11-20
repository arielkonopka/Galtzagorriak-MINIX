/* mf_attack.h - the MIFARE Classic dictionary attack, as a streaming loop.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14 (AC14.2), issue #387.
 *
 * Stock's "read" tries each dictionary key on every sector whose key is not
 * yet known, key A then key B.  Keys come from mf_dict (streamed), so no
 * key list stays resident; the only state is the found-keys table below,
 * which the caller owns.  The attack itself holds no card buffer.
 */
#ifndef _LIBNFC_MF_ATTACK_H
#define _LIBNFC_MF_ATTACK_H

#include "mf_classic.h"
#include "mf_dict.h"

#define MF_MAX_SECTORS	40	/* 4K: 40 sectors */

typedef struct mf_found {
	uint8_t sectors;		/* 5 (mini), 16 (1K), 40 (4K) */
	uint8_t have_a[MF_MAX_SECTORS];	/* 1 = key_a known */
	uint8_t have_b[MF_MAX_SECTORS];
	mf_key_t key_a[MF_MAX_SECTORS];
	mf_key_t key_b[MF_MAX_SECTORS];
	unsigned long tried;		/* auth attempts made (for keys/s) */
} mf_found_t;

void mf_found_init(mf_found_t *f, mf_type_t type);
/* All A and B keys known? */
int mf_found_complete(const mf_found_t *f);

/* Try one key on every sector slot still missing (A then B).  Returns the
 * number of sector keys newly recovered.  Each try is one authentication
 * over `x`; f->tried counts them. */
unsigned mf_attack_key(const nfc_xfer_t *x, uint32_t uid,
		       mf_found_t *f, const mf_key_t *key);

/* Stream a whole dictionary file, applying mf_attack_key to each key, until
 * the file ends or every key is found.  Returns the number of keys read;
 * -1 (errno) if the file cannot be opened.  Malformed lines are skipped. */
long mf_attack_dict(const nfc_xfer_t *x, uint32_t uid,
		    mf_found_t *f, const char *dict_path);

#endif /* _LIBNFC_MF_ATTACK_H */
