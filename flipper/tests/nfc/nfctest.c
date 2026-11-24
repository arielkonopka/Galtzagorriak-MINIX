/* nfctest.c - host tests for libnfc (src/lib/libnfc), spec Phase 14,
 * issue #387.  A software MIFARE Classic 1K model with Crypto-1 sits under
 * the nfc_xfer transceiver; the dictionary attack runs against it and the
 * recovered keys are checked, with keys/s reported (AC14.2, to be judged
 * on the device later).  Also: crypto1 self-consistency, the dictionary
 * streamer, and a wrong-key authentication.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *	cc -I src/lib/libnfc nfctest.c <lib .c> -> run; under ASan/UBSan.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "crypto1.h"
#include "mf_classic.h"
#include "mf_dict.h"
#include "mf_attack.h"
#include "nfc_file.h"
#include "iso14443_3a.h"
#include "mf_ultralight.h"
#include "nfc_poller.h"
#include "nfc_sha1.h"

static int fails;
#define CHECK(c, msg) do { if(!(c)) { printf("FAIL: %s\n", msg); fails++; } } while(0)

/* ---- the software MIFARE Classic 1K card (the mock front end) ---- */

typedef struct card {
	uint32_t uid;
	mf_key_t keya[16], keyb[16];
	/* auth exchange state */
	int phase;			/* 0 idle, 1 awaiting {nr}{ar} */
	int authed;
	uint8_t sector;
	enum mf_key_type ktype;
	uint32_t nt;
	crypto1_t c;
	uint32_t rng;
} card_t;

static uint32_t be32(const uint8_t *p)
{ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static void put32(uint8_t *p, uint32_t v)
{ p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }

static uint32_t card_nonce(card_t *k)
{ k->rng = k->rng * 1103515245u + 12345u; return k->rng ? k->rng : 0x1234abcdu; }

static int card_xfer(void *ctx,
	const uint8_t *tx, size_t tx_bits,
	uint8_t *rx, size_t rx_room, size_t *rx_bits,
	uint32_t fwt, uint32_t flags)
{
	card_t *k = ctx;
	(void)fwt; (void)flags; (void)rx_room;

	if(k->phase == 0) {
		if(tx_bits == 16 && (tx[0] == MF_KEY_A || tx[0] == MF_KEY_B)) {
			uint8_t block = tx[1];
			k->sector = (uint8_t)mf_block_to_sector(block);
			if(k->sector >= 16) return NFC_ERR_PROTO;
			k->ktype = (enum mf_key_type)tx[0];
			k->nt = card_nonce(k);
			k->phase = 1;
			put32(rx, k->nt);
			*rx_bits = 32;
			return NFC_OK;
		}
		return NFC_ERR_TIMEOUT;	/* nothing else before auth */
	}
	/* phase 1: the reader's {nr}{ar}, 9-bit frames. */
	k->phase = 0;
	if(tx_bits != 72) return NFC_ERR_PROTO;
	{
		const mf_key_t *key = (k->ktype == MF_KEY_A)
			? &k->keya[k->sector] : &k->keyb[k->sector];
		uint8_t pl[8], in2[5], f[5];
		size_t b, o = 0, pos = 0;
		uint32_t ar, at;

		crypto1_init(&k->c, mf_key_to_num(key));
		(void)crypto1_word(&k->c, k->uid ^ k->nt, 0);
		if(mf_open(&k->c, tx, 36, 1, pl, 4) != 4) return NFC_ERR_TIMEOUT;
		memset(in2, 0, sizeof in2);
		for(b = 36; b < 72; b++, o++)
			if(tx[b / 8] >> (b % 8) & 1) in2[o / 8] |= 1 << (o % 8);
		if(mf_open(&k->c, in2, 36, 0, pl + 4, 4) != 4) return NFC_ERR_TIMEOUT;
		ar = be32(pl + 4);
		if(ar != crypto1_prng_successor(k->nt, 64))
			return NFC_ERR_TIMEOUT;		/* wrong key: silent */
		at = crypto1_prng_successor(k->nt, 96);
		put32(pl, at);
		memset(f, 0, sizeof f);
		mf_seal(&k->c, pl, 4, 0, f, &pos);
		memcpy(rx, f, 5);
		*rx_bits = pos;
		k->authed = 1;
		return NFC_OK;
	}
}

static void card_1k_init(card_t *k)
{
	int s;
	memset(k, 0, sizeof *k);
	k->uid = 0xDEADBEEFu;
	k->rng = 0xACE1u;
	/* give each sector a key that is in the test dictionary, except
	 * sector 7 key B, which is not, so it must stay unfound. */
	for(s = 0; s < 16; s++) {
		mf_num_to_key(0xFFFFFFFFFFFFULL, &k->keya[s]);
		mf_num_to_key(0xA0A1A2A3A4A5ULL, &k->keyb[s]);
	}
	mf_num_to_key(0xD3F7D3F7D3F7ULL, &k->keya[1]);
	mf_num_to_key(0x4D3A99C351DDULL, &k->keya[4]);
	mf_num_to_key(0x1A982C7E459AULL, &k->keyb[9]);
	mf_num_to_key(0x0102030405A6ULL, &k->keyb[7]);	/* not in the dict */
}

/* ---- tests ---- */

static void test_crypto1(void)
{
	crypto1_t c;
	uint32_t a, b;
	crypto1_init(&c, 0xffffffffffffULL);
	a = crypto1_word(&c, 0, 0);
	crypto1_init(&c, 0xffffffffffffULL);
	b = crypto1_word(&c, 0, 0);
	CHECK(a == b, "crypto1 keystream reproducible");
	CHECK(crypto1_prng_successor(0x12345678u, 0) == 0x12345678u, "prng n=0 identity");
	CHECK(crypto1_prng_successor(0xAABBCCDDu, 64)
	      == crypto1_prng_successor(crypto1_prng_successor(0xAABBCCDDu, 32), 32),
	      "prng 64 == 32+32");
}

static void test_dict_stream(void)
{
	mf_dict_t d;
	mf_key_t k;
	int r, n = 0, bad = 0;
	CHECK(mf_dict_open(&d, "mf_dict.txt") == 0, "open dict");
	while((r = mf_dict_next(&d, &k)) != 0) {
		if(r == 1) n++;
		else bad++;
	}
	CHECK(n == 8, "8 good keys in the fixture");
	CHECK(bad >= 1, "malformed lines reported");
	CHECK(mf_dict_count(&d) == 8, "mf_dict_count agrees");
	mf_dict_close(&d);
}

static void test_wrong_key(void)
{
	card_t card;
	nfc_xfer_t x;
	crypto1_t cs;
	mf_key_t wrong, right;
	int r;
	card_1k_init(&card);
	memset(&x, 0, sizeof x);
	x.transceive = card_xfer; x.ctx = &card;

	mf_num_to_key(0x010203040506ULL, &wrong);
	r = mf_authenticate(&x, card.uid, 3, &wrong, MF_KEY_A, &cs);
	CHECK(r != NFC_OK, "wrong key A is rejected");
	mf_num_to_key(0xFFFFFFFFFFFFULL, &right);
	r = mf_authenticate(&x, card.uid, 3, &right, MF_KEY_A, &cs);
	CHECK(r == NFC_OK, "right key A authenticates");
}

static void test_attack(void)
{
	card_t card;
	nfc_xfer_t x;
	mf_found_t f;
	struct timespec t0, t1;
	long keys;
	double secs;
	int s, missing = 0;

	card_1k_init(&card);
	memset(&x, 0, sizeof x);
	x.transceive = card_xfer; x.ctx = &card;
	mf_found_init(&f, MF_TYPE_1K);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	keys = mf_attack_dict(&x, card.uid, &f, "mf_dict.txt");
	clock_gettime(CLOCK_MONOTONIC, &t1);
	CHECK(keys > 0, "dictionary read");

	/* every sector key that IS in the dictionary must be found and
	 * match the card; sector 7 key B is the one planted outside it. */
	for(s = 0; s < 16; s++) {
		if(f.have_a[s])
			CHECK(mf_key_to_num(&f.key_a[s]) == mf_key_to_num(&card.keya[s]),
			      "recovered key A matches");
		else missing++;
		if(f.have_b[s])
			CHECK(mf_key_to_num(&f.key_b[s]) == mf_key_to_num(&card.keyb[s]),
			      "recovered key B matches");
		else missing++;
	}
	CHECK(f.have_a[0] && f.have_a[1] && f.have_a[4], "known A keys found");
	CHECK(f.have_b[0] && f.have_b[9], "known B keys found");
	CHECK(!f.have_b[7], "key B outside the dictionary stays unfound");
	CHECK(missing == 1, "exactly one key missing (the planted one)");

	/* keys/s: repeat the attack to get a stable rate. */
	secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	{
		int reps = 2000;
		unsigned long tries0;
		mf_found_t g;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		mf_found_init(&g, MF_TYPE_1K);
		tries0 = 0;
		for(int i = 0; i < reps; i++) {
			mf_found_t h;
			mf_found_init(&h, MF_TYPE_1K);
			mf_attack_dict(&x, card.uid, &h, "mf_dict.txt");
			tries0 += h.tried;
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
		printf("  attack: %lu auths in %.3f s = %.0f auths/s "
		       "(host model; stock-equivalent baseline is this same "
		       "model, ratio ~1.0 - AC14.2 is judged on the device)\n",
		       tries0, secs, tries0 / secs);
		(void)keys;
	}
}

/* ---- .nfc file format (AC14.1) ---- */

static int dev_data_eq(const nfc_device_t *a, const nfc_device_t *b)
{
	if(a->type != b->type) return 0;
	if(a->uid_len != b->uid_len || memcmp(a->uid, b->uid, a->uid_len)) return 0;
	if(memcmp(a->atqa, b->atqa, 2) || a->sak != b->sak) return 0;
	if(a->type == NFC_DEV_MF_CLASSIC) {
		if(a->mfc.block_count != b->mfc.block_count) return 0;
		return memcmp(a->mfc.block, b->mfc.block,
			      a->mfc.block_count * sizeof(mf_block_t)) == 0;
	}
	if(a->type == NFC_DEV_MF_ULTRALIGHT) {
		if(a->ul_pages != b->ul_pages) return 0;
		return memcmp(a->ul_page, b->ul_page, a->ul_pages * 4) == 0;
	}
	return 1;
}

static void test_nfc_file(void)
{
	static nfc_device_t a, b, v2, v3;
	const char *err = NULL;
	const char *tmp = "build/roundtrip.nfc";

	/* a stock-format v4 MIFARE Classic 1K loads */
	CHECK(nfc_file_load(&a, "vectors/mfc_1k_v4.nfc", &err) == 0, "load mfc v4");
	CHECK(a.type == NFC_DEV_MF_CLASSIC, "device type mifare classic");
	CHECK(a.uid_len == 4 && a.uid[0] == 0xA2 && a.uid[3] == 0x11, "mfc UID");
	CHECK(a.atqa[1] == 0x04 && a.sak == 0x08, "mfc ATQA/SAK");
	CHECK(a.mfc.type == MF_TYPE_1K && a.mfc.block_count == 64, "mfc 1K 64 blocks");
	CHECK(a.mfc.block[1].data[0] == 0x10, "mfc block 1 data");

	/* write it back and reload: the data is identical (round trip) */
	CHECK(nfc_file_save(&a, tmp) == 0, "save mfc");
	CHECK(nfc_file_load(&b, tmp, &err) == 0, "reload mfc");
	CHECK(dev_data_eq(&a, &b), "mfc round-trips");

	/* older versions load to the same data as v4 (AC14.1 reverse) */
	CHECK(nfc_file_load(&v2, "vectors/mfc_1k_v2.nfc", &err) == 0, "load mfc v2");
	CHECK(nfc_file_load(&v3, "vectors/mfc_1k_v3.nfc", &err) == 0, "load mfc v3");
	CHECK(dev_data_eq(&a, &v2), "v2 == v4 data");
	CHECK(dev_data_eq(&a, &v3), "v3 == v4 data");

	/* NTAG215 (Ultralight family) loads and round-trips */
	CHECK(nfc_file_load(&a, "vectors/ntag215_v4.nfc", &err) == 0, "load ntag215");
	CHECK(a.type == NFC_DEV_MF_ULTRALIGHT, "device type ultralight");
	CHECK(a.ul_pages == 135, "ntag215 135 pages");
	CHECK(a.uid_len == 7, "ntag215 7-byte UID");
	CHECK(nfc_file_save(&a, tmp) == 0, "save ntag215");
	CHECK(nfc_file_load(&b, tmp, &err) == 0, "reload ntag215");
	CHECK(dev_data_eq(&a, &b), "ntag215 round-trips");

	/* a non-NFC file is refused */
	{
		FILE *f = fopen("build/bad.nfc", "w");
		fputs("hello: world\n", f); fclose(f);
		CHECK(nfc_file_load(&b, "build/bad.nfc", &err) != 0, "non-NFC refused");
	}
}

/* ---- ISO 14443-3A activation (poller) ---- */

/* A stateless 3A mock: REQA, the anticollision cascade and SELECT for a
 * 4- or 7-byte UID.  atqa/sak/uid are the card's. */
typedef struct card_a {
	uint8_t uid[10], uid_len, atqa[2], sak;
} card_a_t;

static int card_a_activate(card_a_t *k,
	const uint8_t *tx, size_t tx_bits,
	uint8_t *rx, size_t *rx_bits)
{
	if(tx_bits == 7 && (tx[0] == 0x26 || tx[0] == 0x52)) {	/* REQA/WUPA */
		rx[0] = k->atqa[0]; rx[1] = k->atqa[1];
		*rx_bits = 16;
		return NFC_OK;
	}
	if(tx_bits == 16 && tx[1] == 0x20) {		/* anticollision */
		int lvl = (tx[0] == 0x93) ? 0 : (tx[0] == 0x95) ? 1 : 2;
		if(k->uid_len == 4 && lvl == 0) {
			memcpy(rx, k->uid, 4);
		} else if(k->uid_len == 7 && lvl == 0) {
			rx[0] = 0x88;			/* cascade tag */
			memcpy(rx + 1, k->uid, 3);
		} else if(k->uid_len == 7 && lvl == 1) {
			memcpy(rx, k->uid + 3, 4);
		} else return NFC_ERR_TIMEOUT;
		rx[4] = rx[0] ^ rx[1] ^ rx[2] ^ rx[3];	/* BCC */
		*rx_bits = 40;
		return NFC_OK;
	}
	if(tx_bits == 56 && tx[1] == 0x70) {		/* SELECT */
		int lvl = (tx[0] == 0x93) ? 0 : (tx[0] == 0x95) ? 1 : 2;
		if(k->uid_len == 7 && lvl == 0) rx[0] = 0x04;	/* cascade on */
		else rx[0] = k->sak;
		*rx_bits = 8;
		return NFC_OK;
	}
	return NFC_ERR_TIMEOUT;
}

static int card_a_xfer(void *ctx,
	const uint8_t *tx, size_t tx_bits,
	uint8_t *rx, size_t rx_room, size_t *rx_bits,
	uint32_t fwt, uint32_t flags)
{
	(void)fwt; (void)flags; (void)rx_room;
	return card_a_activate(ctx, tx, tx_bits, rx, rx_bits);
}

static void test_iso14443_3a(void)
{
	card_a_t k4 = { {0xA2,0x45,0x07,0x11}, 4, {0x00,0x04}, 0x08 };
	card_a_t k7 = { {0x04,0x11,0x22,0x33,0x44,0x55,0x66}, 7, {0x00,0x44}, 0x00 };
	nfc_xfer_t x;
	iso14443_3a_tag_t t;

	memset(&x, 0, sizeof x); x.transceive = card_a_xfer; x.ctx = &k4;
	CHECK(iso14443_3a_poll(&x, 0, &t) == NFC_OK, "3A poll 4-byte");
	CHECK(t.uid_len == 4 && memcmp(t.uid, k4.uid, 4) == 0, "4-byte UID");
	CHECK(t.sak == 0x08 && t.atqa[1] == 0x04, "SAK/ATQA 4-byte");
	CHECK(mf_type_from_sak(t.sak) == MF_TYPE_1K, "SAK 0x08 -> 1K");

	x.ctx = &k7;
	CHECK(iso14443_3a_poll(&x, 0, &t) == NFC_OK, "3A poll 7-byte cascade");
	CHECK(t.uid_len == 7 && memcmp(t.uid, k7.uid, 7) == 0, "7-byte UID");
	CHECK(t.sak == 0x00, "SAK 7-byte ultralight");
	CHECK(mf_type_from_sak(t.sak) < 0, "SAK 0x00 is not classic");
}

/* ---- MIFARE Ultralight / NTAG (poller + file round-trip) ---- */

typedef struct card_ul {
	card_a_t a;
	uint16_t pages;
	uint8_t mem[MFUL_MAX_PAGES][4];
	uint8_t version[8];
	int has_version;
} card_ul_t;

static int card_ul_xfer(void *ctx,
	const uint8_t *tx, size_t tx_bits,
	uint8_t *rx, size_t rx_room, size_t *rx_bits,
	uint32_t fwt, uint32_t flags)
{
	card_ul_t *k = ctx;
	(void)fwt; (void)flags; (void)rx_room;

	if(tx_bits == 8 && tx[0] == MFUL_CMD_GET_VERSION) {
		if(!k->has_version) return NFC_ERR_TIMEOUT;
		memcpy(rx, k->version, 8);
		*rx_bits = 64;
		return NFC_OK;
	}
	if(tx_bits == 16 && tx[0] == MFUL_CMD_READ) {
		uint8_t p = tx[1];
		int i;
		if(p >= k->pages) return NFC_ERR_TIMEOUT;
		for(i = 0; i < 4; i++)			/* 4 pages, wrapping */
			memcpy(rx + i * 4, k->mem[(p + i) % k->pages], 4);
		*rx_bits = NFC_BITS(16);
		return NFC_OK;
	}
	return card_a_activate(&k->a, tx, tx_bits, rx, rx_bits);
}

static void test_ultralight(void)
{
	static card_ul_t k;
	static nfc_device_t d, d2;
	nfc_xfer_t x;
	const char *err = NULL;
	const char *tmp = "build/ul_roundtrip.nfc";
	const mful_type_t *t;
	int p;

	memset(&k, 0, sizeof k);
	k.a = (card_a_t){ {0x04,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF}, 7, {0x00,0x44}, 0x00 };
	k.pages = 135;				/* NTAG215 */
	k.has_version = 1;
	/* NTAG215 GET_VERSION: NXP, NTAG (0x04), storage 0x11. */
	memcpy(k.version, "\x00\x04\x04\x02\x01\x00\x11\x03", 8);
	for(p = 0; p < k.pages; p++) {
		k.mem[p][0] = (uint8_t)p; k.mem[p][1] = 0x5A;
		k.mem[p][2] = (uint8_t)(p ^ 0xFF); k.mem[p][3] = 0xA5;
	}
	memset(&x, 0, sizeof x); x.transceive = card_ul_xfer; x.ctx = &k;

	t = mful_get_version(&x);
	CHECK(t && strcmp(t->name, "NTAG215") == 0, "GET_VERSION -> NTAG215");
	CHECK(t && t->total_pages == 135, "NTAG215 135 pages");

	CHECK(nfc_poll_identify(&x, &d) == NFC_OK, "UL identify");
	CHECK(d.type == NFC_DEV_MF_ULTRALIGHT, "identified as ultralight");
	CHECK(d.uid_len == 7, "UL 7-byte UID");
	CHECK(nfc_poll_read_ul(&x, &d) == NFC_OK, "UL read");
	CHECK(d.ul_pages == 135, "read 135 pages");
	CHECK(strcmp(d.ul_subtype, "NTAG215") == 0, "subtype NTAG215");
	CHECK(d.ul_page[7][0] == 7 && d.ul_page[7][1] == 0x5A, "page 7 content");

	/* save and reload: the pages survive the .nfc round trip */
	CHECK(nfc_file_save(&d, tmp) == 0, "save read UL");
	CHECK(nfc_file_load(&d2, tmp, &err) == 0, "reload UL");
	CHECK(d2.ul_pages == 135 &&
	      memcmp(d.ul_page, d2.ul_page, 135 * 4) == 0, "UL pages round-trip");
}

/* ---- MIFARE Classic read over Crypto-1 (poller) ---- */

/* A full auth+read mock: a 4-byte card, one known key on sector 0, each
 * block filled with a pattern.  The state machine follows the reader's
 * strict auth -> {nr,ar} -> encrypted-read per block. */
typedef struct card_rw {
	card_a_t a;
	uint32_t uid;
	mf_key_t keya0;
	mf_block_t block[64];
	int phase;			/* 0 idle, 1 awaiting {nr,ar}, 2 authed */
	uint8_t sector;
	uint32_t nt, rng;
	crypto1_t c;
} card_rw_t;

static int card_rw_xfer(void *ctx,
	const uint8_t *tx, size_t tx_bits,
	uint8_t *rx, size_t rx_room, size_t *rx_bits,
	uint32_t fwt, uint32_t flags)
{
	card_rw_t *k = ctx;
	(void)fwt; (void)flags; (void)rx_room;

	if(k->phase == 2) {		/* encrypted READ(0x30, block), 9-bit frames */
		uint8_t pl[18], f[24], blk;
		size_t pos = 0;
		unsigned crc;
		int i;
		k->phase = 0;
		if(mf_open(&k->c, tx, tx_bits, 0, pl, 4) != 4) return NFC_ERR_TIMEOUT;
		crc = mf_crc_a(pl, 2);
		if(pl[0] != 0x30 || pl[1] >= 64 || (pl[2] | pl[3] << 8) != (int)crc)
			return NFC_ERR_TIMEOUT;
		blk = pl[1];
		for(i = 0; i < 16; i++) pl[i] = k->block[blk].data[i];
		crc = mf_crc_a(pl, 16);
		pl[16] = (uint8_t)crc; pl[17] = (uint8_t)(crc >> 8);
		memset(f, 0, sizeof f);
		mf_seal(&k->c, pl, 18, 0, f, &pos);
		memcpy(rx, f, (pos + 7) / 8);
		*rx_bits = pos;
		return NFC_OK;
	}
	if(k->phase == 0) {
		if(tx_bits == 16 && (tx[0] == MF_KEY_A || tx[0] == MF_KEY_B)) {
			k->sector = (uint8_t)mf_block_to_sector(tx[1]);
			k->rng = k->rng * 1103515245u + 12345u;
			k->nt = k->rng ? k->rng : 0x1234abcdu;
			k->phase = 1;
			put32(rx, k->nt);
			*rx_bits = 32;
			return NFC_OK;
		}
		return card_a_activate(&k->a, tx, tx_bits, rx, rx_bits);
	}
	/* phase 1: the reader's {nr}{ar}, 9-bit frames. */
	k->phase = 0;
	if(tx_bits != 72) return NFC_ERR_PROTO;
	{
		uint8_t pl[8], in2[5], f[5];
		size_t b, o = 0, pos = 0;
		uint32_t ar, at;
		crypto1_init(&k->c, mf_key_to_num(&k->keya0));
		(void)crypto1_word(&k->c, k->uid ^ k->nt, 0);
		if(mf_open(&k->c, tx, 36, 1, pl, 4) != 4) return NFC_ERR_TIMEOUT;
		memset(in2, 0, sizeof in2);
		for(b = 36; b < 72; b++, o++)
			if(tx[b / 8] >> (b % 8) & 1) in2[o / 8] |= 1 << (o % 8);
		if(mf_open(&k->c, in2, 36, 0, pl + 4, 4) != 4) return NFC_ERR_TIMEOUT;
		ar = be32(pl + 4);
		if(k->sector != 0 || ar != crypto1_prng_successor(k->nt, 64))
			return NFC_ERR_TIMEOUT;		/* wrong/unknown key */
		at = crypto1_prng_successor(k->nt, 96);
		put32(pl, at);
		memset(f, 0, sizeof f);
		mf_seal(&k->c, pl, 4, 0, f, &pos);
		memcpy(rx, f, 5);
		*rx_bits = pos;
		k->phase = 2;				/* ready for the read */
		return NFC_OK;
	}
}

static void test_mfc_read(void)
{
	static card_rw_t k;
	static nfc_device_t d;
	nfc_xfer_t x;
	mf_found_t f;
	int blk, got;

	memset(&k, 0, sizeof k);
	k.a = (card_a_t){ {0xA2,0x45,0x07,0x11}, 4, {0x00,0x04}, 0x08 };
	k.uid = 0xA2450711u;
	mf_num_to_key(0xFFFFFFFFFFFFULL, &k.keya0);
	for(blk = 0; blk < 64; blk++) {
		int i;
		for(i = 0; i < 16; i++) k.block[blk].data[i] = (uint8_t)(blk * 16 + i);
	}
	memset(&x, 0, sizeof x); x.transceive = card_rw_xfer; x.ctx = &k;

	CHECK(nfc_poll_identify(&x, &d) == NFC_OK, "MFC identify");
	CHECK(d.type == NFC_DEV_MF_CLASSIC && d.mfc.type == MF_TYPE_1K,
	      "identified as MIFARE Classic 1K");

	/* only sector 0's key is known: blocks 0..3 should read. */
	mf_found_init(&f, MF_TYPE_1K);
	f.have_a[0] = 1; f.key_a[0] = k.keya0;
	got = nfc_poll_read_mfc(&x, &d, &f);
	CHECK(got == 4, "read 4 blocks of sector 0");
	CHECK((d.mfc.block_read & 0xF) == 0xF, "blocks 0..3 marked read");
	CHECK(d.mfc.block[2].data[0] == 2 * 16 && d.mfc.block[3].data[15] == 3*16+15,
	      "block contents decrypted correctly");
}

/* SHA-1, FIPS 180-4 / RFC 3174 test vectors (#412). */
static int sha1_is(const char *msg, size_t len, size_t reps, const char *hex)
{
	uint8_t d[20], *buf;
	char got[41];
	size_t i;
	buf = malloc(len * reps + 1);
	for(i = 0; i < reps; i++) memcpy(buf + i * len, msg, len);
	nfc_sha1(buf, len * reps, d);
	free(buf);
	for(i = 0; i < 20; i++) sprintf(got + 2 * i, "%02x", d[i]);
	return strcmp(got, hex) == 0;
}

static void test_sha1(void)
{
	CHECK(sha1_is("", 0, 1, "da39a3ee5e6b4b0d3255bfef95601890afd80709"), "sha1: empty");
	CHECK(sha1_is("abc", 3, 1, "a9993e364706816aba3e25717850c26c9cd0d89d"), "sha1: abc");
	CHECK(sha1_is("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, 1,
		      "84983e441c3bd26ebaae4aa1f95129e5e54670f1"), "sha1: 448 bits (two blocks)");
	CHECK(sha1_is("a", 1, 1000000, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"), "sha1: a million a");
	CHECK(sha1_is("0123456701234567012345670123456701234567012345670123456701234567", 64, 10,
		      "dea356a2cddd90c7a7ecedc5ebb563934f460452"), "sha1: RFC 3174 test 4");
}

int main(void)
{
	printf("libnfc host tests (Phase 14, #387)\n");
	test_crypto1();
	test_dict_stream();
	test_wrong_key();
	test_attack();
	test_nfc_file();
	test_iso14443_3a();
	test_ultralight();
	test_mfc_read();
	test_sha1();
	if(fails) { printf("%d FAILED\n", fails); return 1; }
	printf("all libnfc tests passed\n");
	return 0;
}
