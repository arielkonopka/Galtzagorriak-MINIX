/* SPDX-License-Identifier: GPL-3.0-or-later
 * p256 - P-256 for libu2f, see p256.h.  micro-ecc is compiled here, with
 * its options set in one place: only secp256r1, portable C (no assembler:
 * MINIX user programs keep their PIC base in r9).  The vendored files are
 * unchanged. */
#define uECC_PLATFORM		uECC_arch_other
#define uECC_SUPPORTS_secp160r1	0
#define uECC_SUPPORTS_secp192r1	0
#define uECC_SUPPORTS_secp224r1	0
#define uECC_SUPPORTS_secp256r1	1
#define uECC_SUPPORTS_secp256k1	0
#define uECC_SUPPORT_COMPRESSED_POINT 0
#define uECC_OPTIMIZATION_LEVEL	2
#include "uecc/uECC.c"
#include <stddef.h>
#include "p256.h"

static int (*rng_fill)(unsigned char *b, unsigned n);

static int rng(uint8_t *dest, unsigned size)
{
	return rng_fill != NULL && rng_fill(dest, size) == 0;
}

void p256_rng(int (*fill)(unsigned char *b, unsigned n))
{
	rng_fill = fill;
	uECC_set_rng(rng);
}

int p256_pub(void *ctx, const unsigned char priv[32], unsigned char pub[65])
{
	(void) ctx;
	pub[0] = 0x04;
	return uECC_compute_public_key(priv, pub + 1, uECC_secp256r1()) ? 0 : -1;
}

int p256_sign(void *ctx, const unsigned char priv[32],
	      const unsigned char hash[32], unsigned char sig[64])
{
	(void) ctx;
	if (rng_fill == NULL) return -1;
	return uECC_sign(priv, hash, 32, sig, uECC_secp256r1()) ? 0 : -1;
}

int p256_verify(const unsigned char pub[65], const unsigned char hash[32],
		const unsigned char sig[64])
{
	if (pub[0] != 0x04) return 0;
	return uECC_verify(pub + 1, hash, 32, sig, uECC_secp256r1());
}
