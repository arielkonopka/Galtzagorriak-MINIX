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
	/* phase 1: the reader's {nr}{ar}. */
	k->phase = 0;
	if(tx_bits != 64) return NFC_ERR_PROTO;
	{
		uint32_t nr_enc = be32(tx), ar_enc = be32(tx + 4);
		const mf_key_t *key = (k->ktype == MF_KEY_A)
			? &k->keya[k->sector] : &k->keyb[k->sector];
		uint32_t ks_ar, ar, at;
		crypto1_init(&k->c, mf_key_to_num(key));
		(void)crypto1_word(&k->c, k->uid ^ k->nt, 0);
		(void)crypto1_word(&k->c, nr_enc, 1);	/* feeds plaintext nr */
		ks_ar = crypto1_word(&k->c, 0, 0);
		ar = ar_enc ^ ks_ar;
		if(ar != crypto1_prng_successor(k->nt, 64))
			return NFC_ERR_TIMEOUT;		/* wrong key: silent */
		at = crypto1_prng_successor(k->nt, 96);
		put32(rx, at ^ crypto1_word(&k->c, 0, 0));
		*rx_bits = 32;
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

int main(void)
{
	printf("libnfc host tests (Phase 14, #387)\n");
	test_crypto1();
	test_dict_stream();
	test_wrong_key();
	test_attack();
	test_nfc_file();
	if(fails) { printf("%d FAILED\n", fails); return 1; }
	printf("all libnfc tests passed\n");
	return 0;
}
