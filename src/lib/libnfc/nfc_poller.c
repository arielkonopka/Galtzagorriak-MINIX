/* nfc_poller.c - the NFC-A poller (spec Phase 14, issue #387).
 * See nfc_poller.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "nfc_poller.h"
#include "iso14443_3a.h"
#include "mf_ultralight.h"
#include <string.h>

int nfc_poll_identify(const nfc_xfer_t *x, nfc_device_t *d)
{
	iso14443_3a_tag_t t;
	int r, mt;

	memset(d, 0, sizeof *d);
	r = iso14443_3a_poll(x, 0, &t);
	if(r != NFC_OK) return r;

	memcpy(d->uid, t.uid, t.uid_len);
	d->uid_len = t.uid_len;
	d->atqa[0] = t.atqa[0];
	d->atqa[1] = t.atqa[1];
	d->sak = t.sak;

	mt = mf_type_from_sak(t.sak);
	if(mt >= 0) {
		d->type = NFC_DEV_MF_CLASSIC;
		d->mfc.type = (mf_type_t)mt;
		d->mfc.block_count = mf_block_count((mf_type_t)mt);
		memcpy(d->mfc.uid, t.uid, t.uid_len);
		d->mfc.uid_len = t.uid_len;
		d->mfc.atqa[0] = t.atqa[0];
		d->mfc.atqa[1] = t.atqa[1];
		d->mfc.sak = t.sak;
	} else if(t.sak == 0x00) {
		d->type = NFC_DEV_MF_ULTRALIGHT;	/* Ultralight / NTAG */
	} else {
		d->type = NFC_DEV_ISO14443_3A;		/* UID only */
	}
	return NFC_OK;
}

/* The 4-byte cascade-1 UID the Crypto-1 nonce is mixed with. */
static uint32_t uid_cl1(const uint8_t *uid, uint8_t uid_len)
{
	const uint8_t *p = uid;
	if(uid_len == 7) p = uid;	/* 7-byte: the first 4 are used as-is */
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}

int nfc_poll_read_mfc(const nfc_xfer_t *x, nfc_device_t *d, const mf_found_t *f)
{
	uint32_t uid = uid_cl1(d->uid, d->uid_len);
	uint16_t blk;
	int read = 0;

	for(blk = 0; blk < d->mfc.block_count; blk++) {
		uint8_t sector = mf_block_to_sector(blk);
		uint8_t trailer = (uint8_t)mf_sector_trailer(sector);
		const mf_key_t *key;
		enum mf_key_type kt;
		crypto1_t c;
		mf_block_t b;

		if(f->have_a[sector]) { key = &f->key_a[sector]; kt = MF_KEY_A; }
		else if(f->have_b[sector]) { key = &f->key_b[sector]; kt = MF_KEY_B; }
		else continue;			/* no key for this sector */

		/* One authentication per block keeps the mock simple and is
		 * what a careful reader does after a read resets the cipher. */
		if(mf_authenticate(x, uid, trailer, key, kt, &c) != NFC_OK)
			continue;
		if(mf_read_block(x, &c, (uint8_t)blk, &b) != NFC_OK)
			continue;
		mf_halt(x, &c);
		d->mfc.block[blk] = b;
		if(blk < 64) d->mfc.block_read |= (uint64_t)1 << blk;
		read++;
	}
	return read;
}

int nfc_poll_read_ul(const nfc_xfer_t *x, nfc_device_t *d)
{
	const mful_type_t *t = mful_get_version(x);
	uint16_t pages = t ? t->total_pages : 16, got = 0;
	int r;

	if(pages > NFC_UL_MAX_PAGES) pages = NFC_UL_MAX_PAGES;
	r = mful_read_card(x, pages, d->ul_page, &got);
	d->ul_pages = got;
	if(t) {
		size_t n = strlen(t->name);
		if(n >= sizeof d->ul_subtype) n = sizeof d->ul_subtype - 1;
		memcpy(d->ul_subtype, t->name, n);
		d->ul_subtype[n] = '\0';
	}
	return r;
}
