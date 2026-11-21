/* SPDX-License-Identifier: GPL-3.0-or-later
 * nfcloop - the software cards behind `nfc -L` / `nfc -t` (spec Phase 14,
 * issue #412 item 4): an nfc_xfer with a card in the field, so the nfc
 * app's screens and flow run in QEMU (and on a Flipper without the
 * /dev/nfc driver, Phase 13) as infrared -L does for IR.
 *
 * Two cards, chosen by nfcloop_card(): a MIFARE Classic 1K (4-byte UID,
 * key A FFFFFFFFFFFF on every sector, key B one no dictionary has, every
 * block a pattern) and an NTAG215 (7-byte UID, 135 pages).  The same
 * models as flipper/tests/nfc/nfctest.c, cut to what the app needs.
 * State: one struct (~1.6 KB), malloc'd by the app only while it runs.
 */
#include <stdlib.h>
#include <string.h>
#include "nfcloop.h"
#include "crypto1.h"
#include "mf_classic.h"
#include "mf_ultralight.h"

#define LB_KEYA	0xFFFFFFFFFFFFULL
#define LB_KEYB	0x1A2B3C4D5E6FULL

struct nfcloop {
	int kind;			/* NFCLOOP_* */
	uint8_t uid[7], uid_len, atqa[2], sak;
	/* Classic */
	uint8_t blk[64][16];
	int phase;			/* 0 idle, 1 {nr,ar} next, 2 authed */
	uint8_t sector, keyb;
	uint32_t nt, rng;
	crypto1_t c;
	/* NTAG215 */
	uint8_t page[135][4];
};

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16
		| (uint32_t) p[2] << 8 | p[3];
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t) (v >> 24); p[1] = (uint8_t) (v >> 16);
	p[2] = (uint8_t) (v >> 8); p[3] = (uint8_t) v;
}

static void key6(uint8_t *p, uint64_t k)
{
	int i;

	for (i = 5; i >= 0; i--) { p[i] = (uint8_t) k; k >>= 8; }
}

void nfcloop_card(struct nfcloop *k, int kind)
{
	int b, i;

	memset(k, 0, sizeof *k);
	k->kind = kind;
	if (kind == NFCLOOP_NONE) return;
	if (kind == NFCLOOP_MFC1K) {
		static const uint8_t u[4] = { 0xA2, 0x45, 0x07, 0x11 };

		memcpy(k->uid, u, 4);
		k->uid_len = 4; k->atqa[1] = 0x04; k->sak = 0x08;
		for (b = 0; b < 64; b++)
			for (i = 0; i < 16; i++) k->blk[b][i] = (uint8_t) (b * 16 + i);
		memcpy(k->blk[0], u, 4);
		k->blk[0][4] = u[0] ^ u[1] ^ u[2] ^ u[3];
		k->blk[0][5] = 0x08; k->blk[0][6] = 0x04; k->blk[0][7] = 0x00;
		for (b = 3; b < 64; b += 4) {
			key6(k->blk[b], LB_KEYA);
			k->blk[b][6] = 0xFF; k->blk[b][7] = 0x07;
			k->blk[b][8] = 0x80; k->blk[b][9] = 0x69;
			key6(k->blk[b] + 10, LB_KEYB);
		}
		return;
	}
	{	/* NTAG215 */
		static const uint8_t u[7] = { 0x04, 0x6A, 0x3E, 0x12, 0x8C, 0x5D, 0x80 };

		memcpy(k->uid, u, 7);
		k->uid_len = 7; k->atqa[1] = 0x44; k->sak = 0x00;
		k->page[0][0] = u[0]; k->page[0][1] = u[1]; k->page[0][2] = u[2];
		k->page[0][3] = 0x88 ^ u[0] ^ u[1] ^ u[2];
		memcpy(k->page[1], u + 3, 4);
		k->page[2][0] = u[3] ^ u[4] ^ u[5] ^ u[6];
		k->page[2][1] = 0x48;
		memcpy(k->page[3], "\xE1\x10\x3E\x00", 4);	/* CC: NDEF, 496 B */
		/* an empty NDEF message TLV, then the terminator */
		memcpy(k->page[4], "\x03\x00\xFE\x00", 4);
		memcpy(k->page[130], "\x04\x00\x00\xBD", 4);	/* dynamic lock */
		memcpy(k->page[131], "\x04\x00\x00\xFF", 4);	/* CFG0 */
		memcpy(k->page[132], "\x00\x05\x00\x00", 4);	/* CFG1 */
		memcpy(k->page[133], "\xFF\xFF\xFF\xFF", 4);	/* PWD */
	}
}

struct nfcloop *nfcloop_new(void)
{
	return calloc(1, sizeof(struct nfcloop));
}

int nfcloop_kind(const struct nfcloop *k)
{
	return k->kind;
}

/* REQA/WUPA, the anticollision cascade and SELECT. */
static int activate(struct nfcloop *k, const uint8_t *tx, size_t tx_bits,
		    uint8_t *rx, size_t *rx_bits)
{
	int lvl;

	if (tx_bits == 7 && (tx[0] == 0x26 || tx[0] == 0x52)) {
		k->phase = 0;
		rx[0] = k->atqa[0]; rx[1] = k->atqa[1];
		*rx_bits = 16;
		return NFC_OK;
	}
	if (tx_bits < 16) return NFC_ERR_TIMEOUT;
	lvl = tx[0] == 0x93 ? 0 : tx[0] == 0x95 ? 1 : 2;
	if (tx_bits == 16 && tx[1] == 0x20) {
		if (k->uid_len == 4 && lvl == 0) memcpy(rx, k->uid, 4);
		else if (k->uid_len == 7 && lvl == 0) {
			rx[0] = 0x88;
			memcpy(rx + 1, k->uid, 3);
		} else if (k->uid_len == 7 && lvl == 1) memcpy(rx, k->uid + 3, 4);
		else return NFC_ERR_TIMEOUT;
		rx[4] = rx[0] ^ rx[1] ^ rx[2] ^ rx[3];
		*rx_bits = 40;
		return NFC_OK;
	}
	if (tx_bits == 56 && tx[1] == 0x70) {
		rx[0] = (k->uid_len == 7 && lvl == 0) ? 0x04 : k->sak;
		*rx_bits = 8;
		return NFC_OK;
	}
	return NFC_ERR_TIMEOUT;
}

static int mfc(struct nfcloop *k, const uint8_t *tx, size_t tx_bits,
	       uint8_t *rx, size_t *rx_bits)
{
	int i;

	if (k->phase == 2) {		/* an encrypted READ */
		uint8_t cmd, b;

		k->phase = 0;
		if (tx_bits != 16) return NFC_ERR_PROTO;
		cmd = tx[0] ^ crypto1_byte(&k->c, tx[0], 1);
		b = tx[1] ^ crypto1_byte(&k->c, tx[1], 1);
		if (cmd != 0x30 || b >= 64 || mf_block_to_sector(b) != k->sector)
			return NFC_ERR_PROTO;
		for (i = 0; i < 16; i++) {
			uint8_t v = k->blk[b][i];

			if (mf_sector_trailer(k->sector) == b && i < 6) v = 0;
			rx[i] = v ^ crypto1_byte(&k->c, 0, 0);
		}
		*rx_bits = NFC_BITS(16);
		return NFC_OK;
	}
	if (k->phase == 1) {		/* the reader's {nr}{ar} */
		uint32_t ar, at;
		uint64_t key;
		const uint8_t *t = k->blk[mf_sector_trailer(k->sector)];

		k->phase = 0;
		if (tx_bits != 64) return NFC_ERR_PROTO;
		key = 0;
		for (i = 0; i < 6; i++)
			key = key << 8 | t[k->keyb ? 10 + i : i];
		crypto1_init(&k->c, key);
		(void) crypto1_word(&k->c, be32(k->uid) ^ k->nt, 0);
		(void) crypto1_word(&k->c, be32(tx), 1);
		ar = be32(tx + 4) ^ crypto1_word(&k->c, 0, 0);
		if (ar != crypto1_prng_successor(k->nt, 64)) return NFC_ERR_TIMEOUT;
		at = crypto1_prng_successor(k->nt, 96);
		put32(rx, at ^ crypto1_word(&k->c, 0, 0));
		*rx_bits = 32;
		k->phase = 2;
		return NFC_OK;
	}
	if (tx_bits == 16 && (tx[0] == MF_KEY_A || tx[0] == MF_KEY_B)) {
		if (tx[1] >= 64) return NFC_ERR_TIMEOUT;
		k->sector = mf_block_to_sector(tx[1]);
		k->rng = k->rng * 1103515245u + 12345u;
		k->nt = k->rng ? k->rng : 0x1234abcdu;
		k->keyb = tx[0] == MF_KEY_B;
		k->phase = 1;
		put32(rx, k->nt);
		*rx_bits = 32;
		return NFC_OK;
	}
	return activate(k, tx, tx_bits, rx, rx_bits);
}

static int ntag(struct nfcloop *k, const uint8_t *tx, size_t tx_bits,
		uint8_t *rx, size_t *rx_bits)
{
	int i;

	if (tx_bits == 8 && tx[0] == MFUL_CMD_GET_VERSION) {
		memcpy(rx, "\x00\x04\x04\x02\x01\x00\x11\x03", 8);
		*rx_bits = 64;
		return NFC_OK;
	}
	if (tx_bits == 16 && tx[0] == MFUL_CMD_READ) {
		if (tx[1] >= 135) return NFC_ERR_TIMEOUT;
		for (i = 0; i < 4; i++)
			memcpy(rx + i * 4, k->page[(tx[1] + i) % 135], 4);
		*rx_bits = NFC_BITS(16);
		return NFC_OK;
	}
	return activate(k, tx, tx_bits, rx, rx_bits);
}

static int xfer(void *ctx, const uint8_t *tx, size_t tx_bits, uint8_t *rx,
		size_t rx_room, size_t *rx_bits, uint32_t fwt, uint32_t flags)
{
	struct nfcloop *k = ctx;

	(void) fwt; (void) flags; (void) rx_room;
	if (k->kind == NFCLOOP_MFC1K) return mfc(k, tx, tx_bits, rx, rx_bits);
	if (k->kind == NFCLOOP_NTAG215) return ntag(k, tx, tx_bits, rx, rx_bits);
	return NFC_ERR_TIMEOUT;		/* an empty field */
}

void nfcloop_xfer(struct nfcloop *k, nfc_xfer_t *x)
{
	memset(x, 0, sizeof *x);
	x->transceive = xfer;
	x->ctx = k;
}
