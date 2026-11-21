/* SPDX-License-Identifier: GPL-3.0-or-later
 * SHA-256 (FIPS 180-4) and HMAC-SHA-256 (RFC 2104), for U2F (issue #420).
 * Small and portable: no tables in RAM, the 64 round constants in flash. */
#ifndef U2F_SHA256_H
#define U2F_SHA256_H

struct sha256 {
	unsigned long h[8];
	unsigned long nl, nh;		/* bytes hashed, low and high word */
	unsigned char b[64];
	unsigned bn;
};

void sha256_init(struct sha256 *c);
void sha256_update(struct sha256 *c, const void *p, unsigned n);
void sha256_final(struct sha256 *c, unsigned char out[32]);

struct hmac256 {
	struct sha256 in, out;
};

void hmac256_init(struct hmac256 *c, const unsigned char *key, unsigned n);
void hmac256_update(struct hmac256 *c, const void *p, unsigned n);
void hmac256_final(struct hmac256 *c, unsigned char out[32]);

#endif /* U2F_SHA256_H */
