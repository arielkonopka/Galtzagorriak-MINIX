/* crypto1.h - the MIFARE Classic stream cipher (Crypto-1).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14 (D.3: ciphers are ported in full,
 * only secret data files are left out), issue #387.
 *
 * Crypto-1 is a 48-bit LFSR stream cipher.  The algorithm is public (NXP's
 * design, reconstructed by Garcia et al. 2008 and the Crapto1 project,
 * GPL-2.0-or-later; the same code lives GPL-3.0 in the Flipper firmware's
 * lib/nfc/helpers/crypto1.c).  Reimplemented here from the public filter
 * and feedback polynomials; interface kept like the Flipper's so the
 * MIFARE Classic poller and `mfkey` read the same way.
 */
#ifndef _LIBNFC_CRYPTO1_H
#define _LIBNFC_CRYPTO1_H

#include <stdint.h>

typedef struct crypto1 {
	uint32_t odd;		/* bits 0,2,4,... of the 48-bit state */
	uint32_t even;		/* bits 1,3,5,... */
} crypto1_t;

/* The nonlinear filter f(state): one keystream bit. */
uint32_t crypto1_filter(uint32_t x);

/* Load a 48-bit key (big-endian in the 6 key bytes). */
void crypto1_init(crypto1_t *c, uint64_t key);
void crypto1_reset(crypto1_t *c);

/* One keystream/cipher bit.  `in` is fed into the LFSR; `is_encrypted`
 * selects whether the fed-in bit is first deciphered (reader->tag flow). */
uint8_t crypto1_bit(crypto1_t *c, uint8_t in, int is_encrypted);
uint8_t crypto1_byte(crypto1_t *c, uint8_t in, int is_encrypted);
uint32_t crypto1_word(crypto1_t *c, uint32_t in, int is_encrypted);

/* The tag nonce PRNG: advance the 16-bit nonce x by n clocks (used by the
 * authentication exchange and by the nested attack to predict nonces). */
uint32_t crypto1_prng_successor(uint32_t x, uint32_t n);

#endif /* _LIBNFC_CRYPTO1_H */
