/* SPDX-License-Identifier: GPL-3.0-or-later
 * nfcloop - the software cards behind `nfc -L` / `nfc -t` (spec Phase 14,
 * issue #412 item 4): an nfc_xfer with a card in the field, so the nfc
 * app's screens and flow run in QEMU (and on a Flipper without the
 * /dev/nfc driver, Phase 13) as infrared -L does for IR.
 *
 * Three cards, chosen by nfcloop_card(): a MIFARE Classic 1K (4-byte UID,
 * key A FFFFFFFFFFFF on every sector, key B one no dictionary has, every
 * block a pattern), an NTAG215 (7-byte UID, 135 pages) and an ISO
 * 14443-4 Type 4 Tag (#451: an NDEF URI, and three more applications
 * that answer SELECT: Visa, FIDO U2F and the payment directory PPSE).  The same
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
	/* Type 4 Tag */
	int rats, sel;
	uint16_t file;
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
	if (kind == NFCLOOP_T4T) {
		static const uint8_t u[7] = { 0x04, 0x51, 0x7A, 0x22, 0x31, 0x6E, 0x80 };

		memcpy(k->uid, u, 7);
		k->uid_len = 7; k->atqa[0] = 0x44; k->atqa[1] = 0x03;
		k->sak = 0x20;
		k->sel = -1;
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

	if (k->phase == 2) {		/* an encrypted READ, 9-bit frames */
		uint8_t pl[18], f[24], blk;
		size_t pos = 0;
		unsigned crc;

		k->phase = 0;
		if (mf_open(&k->c, tx, tx_bits, 0, pl, 4) != 4)
			return NFC_ERR_TIMEOUT;
		crc = mf_crc_a(pl, 2);
		if (pl[0] != 0x30 || pl[1] >= 64
		    || mf_block_to_sector(pl[1]) != k->sector
		    || (pl[2] | pl[3] << 8) != (int) crc)
			return NFC_ERR_TIMEOUT;
		blk = pl[1];
		for (i = 0; i < 16; i++) {
			uint8_t v = k->blk[blk][i];

			if (mf_sector_trailer(k->sector) == blk && i < 6) v = 0;
			pl[i] = v;
		}
		crc = mf_crc_a(pl, 16);
		pl[16] = (uint8_t) crc;
		pl[17] = (uint8_t) (crc >> 8);
		memset(f, 0, sizeof f);
		mf_seal(&k->c, pl, 18, 0, f, &pos);
		memcpy(rx, f, (pos + 7) / 8);
		*rx_bits = pos;
		return NFC_OK;
	}
	if (k->phase == 1) {		/* the reader's {nr}{ar}, 9-bit frames */
		uint32_t ar, at;
		uint64_t key;
		uint8_t pl[8], f[5];
		size_t pos = 0;
		const uint8_t *t = k->blk[mf_sector_trailer(k->sector)];

		k->phase = 0;
		if (tx_bits != 72) return NFC_ERR_PROTO;
		key = 0;
		for (i = 0; i < 6; i++)
			key = key << 8 | t[k->keyb ? 10 + i : i];
		crypto1_init(&k->c, key);
		(void) crypto1_word(&k->c, be32(k->uid) ^ k->nt, 0);
		/* the reader's nonce is fed in as sent, then {ar} */
		if (mf_open(&k->c, tx, 36, 1, pl, 4) != 4) return NFC_ERR_TIMEOUT;
		{
			uint8_t in2[5];
			size_t b, o = 0;

			memset(in2, 0, sizeof in2);
			for (b = 36; b < 72; b++, o++)
				if (tx[b / 8] >> (b % 8) & 1) in2[o / 8] |= 1 << (o % 8);
			if (mf_open(&k->c, in2, 36, 0, pl + 4, 4) != 4)
				return NFC_ERR_TIMEOUT;
		}
		ar = be32(pl + 4);
		if (ar != crypto1_prng_successor(k->nt, 64)) return NFC_ERR_TIMEOUT;
		at = crypto1_prng_successor(k->nt, 96);
		put32(pl, at);
		memset(f, 0, sizeof f);
		mf_seal(&k->c, pl, 4, 0, f, &pos);
		memcpy(rx, f, 5);
		*rx_bits = pos;
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

/* ---- the Type 4 Tag: RATS, ISO-DEP I-blocks, ISO 7816-4 ---- */

static const uint8_t t4_ats[5] = { 0x05, 0x78, 0x80, 0x70, 0x02 };
static const uint8_t t4_aid[][15] = {	/* length first */
	{ 7, 0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01 },	/* NDEF */
	{ 7, 0xA0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10 },	/* Visa */
	{ 8, 0xA0, 0x00, 0x00, 0x06, 0x47, 0x2F, 0x00, 0x01 },	/* FIDO U2F */
	{ 14, '2', 'P', 'A', 'Y', '.', 'S', 'Y', 'S', '.', 'D', 'D', 'F',
	  '0', '1' },						/* PPSE */
};
static const uint8_t t4_cc[15] = {
	0x00, 0x0F, 0x20, 0x00, 0x3B, 0x00, 0x34,
	0x04, 0x06, 0xE1, 0x04, 0x00, 0x40, 0x00, 0x00,
};
static const uint8_t t4_ndef[] = {	/* NLEN, then one URI record */
	0x00, 0x18, 0xD1, 0x01, 0x14, 'U', 0x04,
	'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'o', 'r', 'g', '/', 'm', 'i',
	'n', 'i', 'x', '/', 't',
};

static size_t sw(uint8_t *r, size_t n, unsigned v)
{
	r[n] = (uint8_t) (v >> 8);
	r[n + 1] = (uint8_t) v;
	return n + 2;
}

static size_t t4_app(struct nfcloop *k, const uint8_t *c, size_t n, uint8_t *r)
{
	size_t i;

	if (n >= 5 && c[0] == 0 && c[1] == 0xA4 && c[2] == 0x04) {
		k->file = 0;
		for (i = 0; i < sizeof t4_aid / sizeof t4_aid[0]; i++)
			if (c[4] == t4_aid[i][0] && n >= 5u + c[4]
			    && !memcmp(c + 5, t4_aid[i] + 1, c[4])) {
				k->sel = (int) i;
				return sw(r, 0, 0x9000);
			}
		k->sel = -1;
		return sw(r, 0, 0x6A82);
	}
	if (n >= 7 && c[0] == 0 && c[1] == 0xA4 && c[2] == 0x00 && c[4] == 2) {
		unsigned f = (unsigned) c[5] << 8 | c[6];

		if (k->sel != 0 || (f != 0xE103 && f != 0xE104))
			return sw(r, 0, 0x6A82);
		k->file = (uint16_t) f;
		return sw(r, 0, 0x9000);
	}
	if (n >= 5 && c[0] == 0 && c[1] == 0xB0 && k->file) {
		const uint8_t *f = k->file == 0xE103 ? t4_cc : t4_ndef;
		size_t fs = k->file == 0xE103 ? sizeof t4_cc : sizeof t4_ndef;
		size_t off = (size_t) c[2] << 8 | c[3], le = c[4] ? c[4] : 256;

		if (off >= fs) return sw(r, 0, 0x6B00);
		if (le > fs - off) le = fs - off;
		if (le > 200) le = 200;
		memcpy(r, f + off, le);
		return sw(r, le, 0x9000);
	}
	return sw(r, 0, 0x6D00);
}

static int t4(struct nfcloop *k, const uint8_t *tx, size_t tx_bits,
	      uint8_t *rx, size_t *rx_bits)
{
	size_t n = tx_bits / 8;

	if (tx_bits == 7) k->rats = 0;
	if (!k->rats && tx_bits == 16 && tx[0] == 0xE0) {
		k->rats = 1;
		memcpy(rx, t4_ats, sizeof t4_ats);
		*rx_bits = NFC_BITS(sizeof t4_ats);
		return NFC_OK;
	}
	if (k->rats && n >= 1 && (tx[0] & 0xEE) == 0x02) {	/* I-block */
		rx[0] = tx[0];
		*rx_bits = NFC_BITS(1 + t4_app(k, tx + 1, n - 1, rx + 1));
		return NFC_OK;
	}
	if (k->rats && n == 1 && tx[0] == 0xC2) {		/* DESELECT */
		rx[0] = 0xC2;
		*rx_bits = 8;
		k->rats = 0;
		return NFC_OK;
	}
	if (k->rats) return NFC_ERR_TIMEOUT;
	return activate(k, tx, tx_bits, rx, rx_bits);
}

static int xfer(void *ctx, const uint8_t *tx, size_t tx_bits, uint8_t *rx,
		size_t rx_room, size_t *rx_bits, uint32_t fwt, uint32_t flags)
{
	struct nfcloop *k = ctx;

	(void) fwt; (void) flags; (void) rx_room;
	if (k->kind == NFCLOOP_MFC1K) return mfc(k, tx, tx_bits, rx, rx_bits);
	if (k->kind == NFCLOOP_NTAG215) return ntag(k, tx, tx_bits, rx, rx_bits);
	if (k->kind == NFCLOOP_T4T) return t4(k, tx, tx_bits, rx, rx_bits);
	return NFC_ERR_TIMEOUT;		/* an empty field */
}

void nfcloop_xfer(struct nfcloop *k, nfc_xfer_t *x)
{
	memset(x, 0, sizeof *x);
	x->transceive = xfer;
	x->ctx = k;
}
