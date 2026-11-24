/* mf_classic.c - MIFARE Classic data model and the reader-side Crypto-1
 * authentication (spec Phase 14, issue #387).  See mf_classic.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The three-pass authentication, as the stock reader does it:
 *   1. reader -> tag  AUTH(0x60/0x61, block) + CRC          (plaintext)
 *   2. tag   -> reader nt                                   (plaintext)
 *   3. reader -> tag  {nr} {ar}   ar = suc64(nt)            (ciphered)
 *   4. tag   -> reader {at}       at = suc96(nt)            (ciphered)
 * Both sides clock the same Crypto-1; a wrong key gives a wrong {at}.
 */
#include "mf_classic.h"
#include "iso14443_3a.h"
#include <string.h>

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void put32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

uint64_t mf_key_to_num(const mf_key_t *k)
{
	uint64_t n = 0;
	int i;
	for(i = 0; i < MF_KEY_SIZE; i++) n = n << 8 | k->data[i];
	return n;
}
void mf_num_to_key(uint64_t n, mf_key_t *k)
{
	int i;
	for(i = MF_KEY_SIZE - 1; i >= 0; i--) { k->data[i] = n & 0xff; n >>= 8; }
}

uint16_t mf_block_count(mf_type_t t)
{
	switch(t) {
	case MF_TYPE_MINI: return MF_MINI_BLOCKS;
	case MF_TYPE_4K: return MF_4K_BLOCKS;
	default: return MF_1K_BLOCKS;
	}
}

/* Sectors 0..31 are 4 blocks; 32..39 (4K only) are 16 blocks. */
uint8_t mf_block_to_sector(uint16_t block)
{
	if(block < 128) return block / 4;
	return 32 + (block - 128) / 16;
}
uint16_t mf_sector_trailer(uint8_t sector)
{
	if(sector < 32) return sector * 4 + 3;
	return 128 + (sector - 32) * 16 + 15;
}

static unsigned odd_par(unsigned b)
{
	b ^= b >> 4; b ^= b >> 2; b ^= b >> 1;
	return ~b & 1;
}

uint16_t mf_crc_a(const uint8_t *p, size_t n)
{
	unsigned c = 0x6363, b;

	while(n-- > 0) {
		b = (*p++ ^ c) & 0xFF;
		b = (b ^ b << 4) & 0xFF;
		c = ((c >> 8) ^ b << 8 ^ b << 3 ^ b >> 4) & 0xFFFF;
	}
	return (uint16_t)c;
}

void mf_seal(crypto1_t *c, const uint8_t *plain, size_t n, int feed,
	     uint8_t *out, size_t *pos)
{
	size_t i, k;

	for(i = 0; i < n; i++) {
		unsigned ks = crypto1_byte(c, feed ? plain[i] : 0, 0);
		unsigned w = (plain[i] ^ ks) |
			(odd_par(plain[i]) ^ crypto1_filter(c->odd)) << 8;

		for(k = 0; k < 9; k++, (*pos)++)
			if(w >> k & 1) out[*pos / 8] |= (uint8_t)(1u << (*pos % 8));
	}
}

int mf_open(crypto1_t *c, const uint8_t *in, size_t bits, int feed,
	    uint8_t *plain, size_t max)
{
	size_t n = bits / 9, i, k, pos = 0;

	if(bits % 9 || n > max) return -1;
	for(i = 0; i < n; i++) {
		unsigned w = 0, p;

		for(k = 0; k < 9; k++, pos++)
			w |= (unsigned)(in[pos / 8] >> (pos % 8) & 1) << k;
		p = (w & 0xFF) ^ crypto1_byte(c, feed ? (w & 0xFF) : 0, feed);
		if(((w >> 8) ^ crypto1_filter(c->odd)) != odd_par(p)) return -1;
		plain[i] = (uint8_t)p;
	}
	return (int)n;
}

int mf_nibble(crypto1_t *c, const uint8_t *in, size_t bits)
{
	unsigned i, v = 0;

	if(bits != 4) return -1;
	for(i = 0; i < 4; i++)
		v |= (unsigned)((in[0] >> i & 1) ^ crypto1_bit(c, 0, 0)) << i;
	return (int)v;
}

void mf_halt(const nfc_xfer_t *x, crypto1_t *c)
{
	uint8_t pl[4], f[6], rx[2];
	size_t pos = 0, rxb;
	unsigned crc;

	pl[0] = 0x50;
	pl[1] = 0x00;
	crc = mf_crc_a(pl, 2);
	pl[2] = (uint8_t)crc;
	pl[3] = (uint8_t)(crc >> 8);
	memset(f, 0, sizeof f);
	mf_seal(c, pl, 4, 0, f, &pos);
	(void)nfc_transceive(x, f, pos, rx, sizeof rx * 8, &rxb, 0,
			     NFC_XF_PARITY | NFC_XF_NO_RX);
}

int mf_authenticate(
	const nfc_xfer_t *x, uint32_t uid,
	uint8_t block, const mf_key_t *key, enum mf_key_type type,
	crypto1_t *out)
{
	uint8_t tx[8], rx[8], pl[8], f[10], f9[6];
	size_t rxb, pos;
	int r;
	uint32_t nt, nr, ar, at, at_card;
	crypto1_t c;

	/* 1. AUTH command: plain frame, the CRC added by the front end; the
	 * nonce comes without a CRC.  A card that dropped a wrong key or was
	 * halted does not answer: wake it up and select it again (as the
	 * stock poller does) and ask once more.  A front end without a card
	 * to wake (a test model) simply gets no answer, which is no error. */
	tx[0] = (uint8_t)type;		/* 0x60 key A, 0x61 key B */
	tx[1] = block;
	r = nfc_transceive(x, tx, NFC_BITS(2), rx, sizeof rx * 8, &rxb,
			   0, NFC_XF_CRC_TX);
	if(r != NFC_OK) {
		iso14443_3a_tag_t tag;

		(void)iso14443_3a_poll(x, 1, &tag);
		r = nfc_transceive(x, tx, NFC_BITS(2), rx, sizeof rx * 8, &rxb,
				   0, NFC_XF_CRC_TX);
	}
	if(r != NFC_OK) return r;
	if(rxb < 32) return NFC_ERR_PROTO;
	nt = be32(rx);			/* 2. tag nonce, plaintext */

	/* 3. load key, absorb uid^nt, send {nr}{ar} as 9-bit frames: the
	 * parity bits are ciphered too. */
	crypto1_init(&c, mf_key_to_num(key));
	(void)crypto1_word(&c, uid ^ nt, 0);
	nr = 0;				/* a fixed reader nonce is fine here */
	put32(pl, nr);
	ar = crypto1_prng_successor(nt, 64);
	put32(pl + 4, ar);
	memset(f, 0, sizeof f);
	pos = 0;
	mf_seal(&c, pl, 4, 1, f, &pos);
	mf_seal(&c, pl + 4, 4, 0, f, &pos);
	r = nfc_transceive(x, f, pos, f9, sizeof f9 * 8, &rxb, 0, NFC_XF_PARITY);
	if(r != NFC_OK) return r;	/* a wrong key: tag drops the frame */
	if(rxb != 36 || mf_open(&c, f9, rxb, 0, pl, sizeof pl) != 4)
		return NFC_ERR_PROTO;

	/* 4. check {at}. */
	at = crypto1_prng_successor(nt, 96);
	at_card = be32(pl);
	if(at_card != at) return NFC_ERR_PROTO;
	if(out) *out = c;
	return NFC_OK;
}

/* The SAK values stock maps to MIFARE Classic (ISO 14443-3A, bit 5 set,
 * UID complete).  0x09 Mini, 0x08 1K, 0x18 4K. */
int mf_type_from_sak(uint8_t sak)
{
	switch(sak & 0x7f) {	/* ignore the cascade bit */
	case 0x09: return MF_TYPE_MINI;
	case 0x08: return MF_TYPE_1K;
	case 0x18: return MF_TYPE_4K;
	}
	return -1;
}

int mf_read_block(const nfc_xfer_t *x, crypto1_t *c, uint8_t block, mf_block_t *out)
{
	uint8_t pl[18], f[6], f9[24];
	size_t rxb, pos = 0;
	unsigned crc;
	int r;

	/* READ(0x30, block) and its CRC over the plain bytes, all ciphered
	 * with their parity: 36 bits. */
	pl[0] = 0x30;
	pl[1] = block;
	crc = mf_crc_a(pl, 2);
	pl[2] = (uint8_t)crc;
	pl[3] = (uint8_t)(crc >> 8);
	memset(f, 0, sizeof f);
	mf_seal(c, pl, 4, 0, f, &pos);
	r = nfc_transceive(x, f, pos, f9, sizeof f9 * 8, &rxb, 0, NFC_XF_PARITY);
	if(r != NFC_OK) return r;
	if(rxb == 4) return NFC_ERR_PROTO;	/* NAK: the access bits refuse */
	if(rxb != 18 * 9 || mf_open(c, f9, rxb, 0, pl, sizeof pl) != 18)
		return NFC_ERR_PROTO;
	crc = mf_crc_a(pl, 16);
	if((pl[16] | pl[17] << 8) != (int)crc) return NFC_ERR_CRC;
	memcpy(out->data, pl, MF_BLOCK_SIZE);
	return NFC_OK;
}
