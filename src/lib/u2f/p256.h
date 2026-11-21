/* SPDX-License-Identifier: GPL-3.0-or-later
 * p256 - P-256 (secp256r1) for libu2f, on micro-ecc (uecc/, BSD-2). */
#ifndef U2F_P256_H
#define U2F_P256_H

/* The random source for signatures (ECDSA's k): fill b with n bytes, 0 on
 * success.  Without one, signing fails. */
void p256_rng(int (*fill)(unsigned char *b, unsigned n));
/* u2f_ops hooks (ctx unused): see u2f.h */
int p256_pub(void *ctx, const unsigned char priv[32], unsigned char pub[65]);
int p256_sign(void *ctx, const unsigned char priv[32],
	      const unsigned char hash[32], unsigned char sig[64]);
/* 1 when sig (r, s) is a valid signature of hash for pub (04 X Y): the
 * tests check the U2F answers with it. */
int p256_verify(const unsigned char pub[65], const unsigned char hash[32],
		const unsigned char sig[64]);

#endif /* U2F_P256_H */
