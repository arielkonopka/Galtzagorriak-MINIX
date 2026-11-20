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

int mf_authenticate(
	const nfc_xfer_t *x, uint32_t uid,
	uint8_t block, const mf_key_t *key, enum mf_key_type type,
	crypto1_t *out)
{
	uint8_t tx[8], rx[8];
	size_t rxb;
	int r;
	uint32_t nt, nr, ar, at, at_card;
	crypto1_t c;

	/* 1. AUTH command (CRC added/checked by the front end). */
	tx[0] = (uint8_t)type;		/* 0x60 key A, 0x61 key B */
	tx[1] = block;
	r = nfc_transceive(x, tx, NFC_BITS(2), rx, sizeof rx * 8, &rxb,
			   0, NFC_XF_CRC_TX | NFC_XF_CRC_RX);
	if(r != NFC_OK) return r;
	if(rxb < 32) return NFC_ERR_PROTO;
	nt = be32(rx);			/* 2. tag nonce, plaintext */

	/* 3. load key, absorb uid^nt, send {nr}{ar}. */
	crypto1_init(&c, mf_key_to_num(key));
	(void)crypto1_word(&c, uid ^ nt, 0);
	nr = 0;				/* a fixed reader nonce is fine here */
	put32(tx, nr ^ crypto1_word(&c, nr, 0));
	ar = crypto1_prng_successor(nt, 64);
	put32(tx + 4, ar ^ crypto1_word(&c, 0, 0));
	r = nfc_transceive(x, tx, NFC_BITS(8), rx, sizeof rx * 8, &rxb,
			   0, 0);
	if(r != NFC_OK) return r;	/* a wrong key: tag drops the frame */
	if(rxb < 32) return NFC_ERR_PROTO;

	/* 4. check {at}. */
	at = crypto1_prng_successor(nt, 96);
	at_card = be32(rx) ^ crypto1_word(&c, 0, 0);
	if(at_card != at) return NFC_ERR_PROTO;
	if(out) *out = c;
	return NFC_OK;
}

int mf_read_block(const nfc_xfer_t *x, crypto1_t *c, uint8_t block, mf_block_t *out)
{
	uint8_t tx[4], rx[18];
	size_t rxb;
	int r, i;

	/* Encrypted READ(0x30, block) + CRC. */
	tx[0] = 0x30 ^ crypto1_byte(c, 0x30, 0);
	tx[1] = block ^ crypto1_byte(c, block, 0);
	/* the two CRC bytes are added by the front end over the plaintext,
	 * then the front end ciphers them too (transparent mode); the host
	 * mock takes the command from the first two deciphered bytes. */
	r = nfc_transceive(x, tx, NFC_BITS(2), rx, sizeof rx * 8, &rxb,
			   0, NFC_XF_CRC_TX | NFC_XF_CRC_RX);
	if(r != NFC_OK) return r;
	if(rxb < NFC_BITS(MF_BLOCK_SIZE)) return NFC_ERR_PROTO;
	for(i = 0; i < MF_BLOCK_SIZE; i++)
		out->data[i] = rx[i] ^ crypto1_byte(c, 0, 0);
	return NFC_OK;
}
