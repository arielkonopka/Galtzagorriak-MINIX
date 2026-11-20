/* mf_ultralight.c - MIFARE Ultralight / NTAG page reader (spec Phase 14,
 * issue #387).  See mf_ultralight.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * READ (0x30, page) returns the four pages starting at `page`, wrapping at
 * the card's last page, with a two-byte CRC the front end checks.  The chip
 * is named from its GET_VERSION bytes (NXP's product/type/subtype/storage
 * fields); the common NTAG and Ultralight storage sizes are tabled below.
 */
#include "mf_ultralight.h"
#include <string.h>

/* The storage-size byte of GET_VERSION encodes 2^(n/2) user bytes; rather
 * than decode it we match the exact (product, storage) pairs stock knows. */
static const mful_type_t types[] = {
	{ "NTAG213",  45 },
	{ "NTAG215", 135 },
	{ "NTAG216", 231 },
	{ "NTAG210",  20 },
	{ "NTAG212",  41 },
	{ "MF0UL11",  20 },	/* Ultralight EV1, 48 bytes */
	{ "MF0UL21",  41 },	/* Ultralight EV1, 128 bytes */
};
static const mful_type_t type_ul = { "MF0ICU1", 16 };	/* original Ultralight */

const mful_type_t *mful_type_from_version(const uint8_t ver[8])
{
	/* ver[2] product type (0x03 NTAG, 0x04 Ultralight), ver[6] storage. */
	uint8_t product = ver[2], storage = ver[6];
	if(product == 0x04) {		/* NTAG */
		switch(storage) {
		case 0x0F: return &types[0];	/* NTAG213 */
		case 0x11: return &types[1];	/* NTAG215 */
		case 0x13: return &types[2];	/* NTAG216 */
		case 0x0B: return &types[3];	/* NTAG210 */
		case 0x0E: return &types[4];	/* NTAG212 */
		}
	} else if(product == 0x03) {	/* Ultralight EV1 */
		if(storage == 0x0B) return &types[5];
		if(storage == 0x0E) return &types[6];
	}
	return NULL;
}

const mful_type_t *mful_get_version(const nfc_xfer_t *x)
{
	uint8_t tx = MFUL_CMD_GET_VERSION;
	uint8_t rx[16];
	size_t rxb = 0;
	const mful_type_t *t;

	if(nfc_transceive(x, &tx, NFC_BITS(1), rx, sizeof rx * 8, &rxb, 0,
			  NFC_XF_CRC_TX | NFC_XF_CRC_RX) != NFC_OK)
		return NULL;
	if(rxb < NFC_BITS(8)) return NULL;
	t = mful_type_from_version(rx);
	return t ? t : &type_ul;
}

int mful_read_page(const nfc_xfer_t *x, uint8_t page, uint8_t out[MFUL_PAGE_SIZE])
{
	uint8_t tx[2], rx[20];
	size_t rxb = 0;
	int r;

	tx[0] = MFUL_CMD_READ;
	tx[1] = page;
	r = nfc_transceive(x, tx, NFC_BITS(2), rx, sizeof rx * 8, &rxb, 0,
			   NFC_XF_CRC_TX | NFC_XF_CRC_RX);
	if(r != NFC_OK) return r;
	if(rxb < NFC_BITS(MFUL_PAGE_SIZE)) return NFC_ERR_PROTO;
	memcpy(out, rx, MFUL_PAGE_SIZE);	/* first of the 4 returned pages */
	return NFC_OK;
}

int mful_read_card(const nfc_xfer_t *x, uint16_t pages,
		   uint8_t page[][MFUL_PAGE_SIZE], uint16_t *pages_out)
{
	uint16_t p = 0;
	uint8_t tx[2], rx[20];

	if(pages > MFUL_MAX_PAGES) pages = MFUL_MAX_PAGES;
	while(p < pages) {
		size_t rxb = 0;
		int r, i, got;

		tx[0] = MFUL_CMD_READ;
		tx[1] = (uint8_t)p;
		r = nfc_transceive(x, tx, NFC_BITS(2), rx, sizeof rx * 8, &rxb,
				   0, NFC_XF_CRC_TX | NFC_XF_CRC_RX);
		if(r != NFC_OK) break;
		got = (int)(rxb / NFC_BITS(MFUL_PAGE_SIZE));
		if(got < 1) break;
		if(got > 4) got = 4;		/* READ returns at most 4 pages */
		for(i = 0; i < got && p < pages; i++, p++)
			memcpy(page[p], rx + i * MFUL_PAGE_SIZE, MFUL_PAGE_SIZE);
	}
	if(pages_out) *pages_out = p;
	return p ? NFC_OK : NFC_ERR_PROTO;
}
