/* mf_classic.h - MIFARE Classic: blocks, sectors, keys, the poller.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #387.
 *
 * The MIFARE Classic data model (stock lib/nfc/protocols/mf_classic) and
 * the reader side of its Crypto-1 authentication, over one nfc_xfer_t.  No
 * buffer stays resident: a struct mf_classic_data is allocated by the
 * caller for the size of card it reads and freed after (the `nfc` app owns
 * it, R14 memory rule).
 */
#ifndef _LIBNFC_MF_CLASSIC_H
#define _LIBNFC_MF_CLASSIC_H

#include <stdint.h>
#include <stddef.h>
#include "nfc_xfer.h"
#include "crypto1.h"

#define MF_BLOCK_SIZE		16
#define MF_KEY_SIZE		6
#define MF_1K_BLOCKS		64	/* 16 sectors x 4 blocks */
#define MF_4K_BLOCKS		256
#define MF_MINI_BLOCKS		20

/* A 48-bit key, big-endian 6 bytes. */
typedef struct mf_key {
	uint8_t data[MF_KEY_SIZE];
} mf_key_t;

enum mf_key_type { MF_KEY_A = 0x60, MF_KEY_B = 0x61 };

typedef enum {
	MF_TYPE_MINI,
	MF_TYPE_1K,
	MF_TYPE_4K,
} mf_type_t;

/* One block of card data. */
typedef struct mf_block {
	uint8_t data[MF_BLOCK_SIZE];
} mf_block_t;

/* The card: blocks plus a bitmap of which are known, and the keys found. */
typedef struct mf_classic_data {
	mf_type_t type;
	uint8_t uid[10];
	uint8_t uid_len;
	uint8_t atqa[2];
	uint8_t sak;
	uint16_t block_count;
	mf_block_t block[MF_4K_BLOCKS];	/* only block_count used */
	uint64_t block_read;		/* up to 64 blocks (1K): bit = known */
} mf_classic_data_t;

uint64_t mf_key_to_num(const mf_key_t *k);
void mf_num_to_key(uint64_t n, mf_key_t *k);
uint16_t mf_block_count(mf_type_t t);
/* Which sector a block is in, and that sector's trailer (key) block. */
uint8_t mf_block_to_sector(uint16_t block);
uint16_t mf_sector_trailer(uint8_t sector);

/* The reader-side authentication to one block with one key.  Runs the full
 * three-pass Crypto-1 exchange over `x`; on success `out` holds the cipher
 * state (ready to read/write that sector) and the call returns NFC_OK.
 * NFC_ERR_PROTO means the key is wrong.  `uid` is the 4-byte cascade-1 UID
 * the nonce is mixed with. */
int mf_authenticate(
	const nfc_xfer_t *x, uint32_t uid,
	uint8_t block, const mf_key_t *key, enum mf_key_type type,
	crypto1_t *out);

/* Read a 16-byte block after a successful mf_authenticate (cipher in `c`). */
int mf_read_block(const nfc_xfer_t *x, crypto1_t *c, uint8_t block, mf_block_t *out);

/* The type a SAK byte names, or -1 if it is not a MIFARE Classic SAK. */
int mf_type_from_sak(uint8_t sak);

#endif /* _LIBNFC_MF_CLASSIC_H */
