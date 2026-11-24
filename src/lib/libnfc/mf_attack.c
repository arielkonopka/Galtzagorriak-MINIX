/* mf_attack.c - the MIFARE Classic dictionary attack (spec Phase 14,
 * AC14.2), issue #387.  See mf_attack.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mf_attack.h"
#include <string.h>

void mf_found_init(mf_found_t *f, mf_type_t type)
{
	memset(f, 0, sizeof *f);
	switch(type) {
	case MF_TYPE_MINI: f->sectors = 5; break;
	case MF_TYPE_4K: f->sectors = 40; break;
	default: f->sectors = 16; break;
	}
}

int mf_found_complete(const mf_found_t *f)
{
	int s;
	for(s = 0; s < f->sectors; s++)
		if(!f->have_a[s] || !f->have_b[s]) return 0;
	return 1;
}

unsigned mf_attack_key(const nfc_xfer_t *x, uint32_t uid,
		       mf_found_t *f, const mf_key_t *key)
{
	unsigned got = 0;
	int s;
	for(s = 0; s < f->sectors; s++) {
		uint8_t trailer = (uint8_t)mf_sector_trailer((uint8_t)s);
		crypto1_t cs;
		if(!f->have_a[s]) {
			f->tried++;
			if(mf_authenticate(x, uid, trailer, key, MF_KEY_A, &cs) == NFC_OK) {
				f->have_a[s] = 1;
				f->key_a[s] = *key;
				got++;
				mf_halt(x, &cs);
			}
		}
		if(!f->have_b[s]) {
			f->tried++;
			if(mf_authenticate(x, uid, trailer, key, MF_KEY_B, &cs) == NFC_OK) {
				f->have_b[s] = 1;
				f->key_b[s] = *key;
				got++;
				mf_halt(x, &cs);
			}
		}
	}
	return got;
}

long mf_attack_dict(const nfc_xfer_t *x, uint32_t uid,
		    mf_found_t *f, const char *dict_path)
{
	mf_dict_t d;
	mf_key_t k;
	long keys = 0;
	int r;

	if(mf_dict_open(&d, dict_path) != 0) return -1;
	while(!mf_found_complete(f) && (r = mf_dict_next(&d, &k)) != 0) {
		if(r < 0) continue;	/* skip a malformed line */
		keys++;
		mf_attack_key(x, uid, f, &k);
	}
	mf_dict_close(&d);
	return keys;
}
