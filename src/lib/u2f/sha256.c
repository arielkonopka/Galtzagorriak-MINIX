/* SPDX-License-Identifier: GPL-3.0-or-later
 * SHA-256 and HMAC-SHA-256, see sha256.h. */
#include <string.h>
#include "sha256.h"

#define M32(x)		((x) & 0xFFFFFFFFUL)
#define ROR(x, n)	M32(((x) >> (n)) | ((x) << (32 - (n))))

static const unsigned long K[64] = {
	0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL,
	0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL, 0xd807aa98UL, 0x12835b01UL,
	0x243185beUL, 0x550c7dc3UL, 0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL,
	0xc19bf174UL, 0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
	0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL, 0x983e5152UL,
	0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL, 0xc6e00bf3UL, 0xd5a79147UL,
	0x06ca6351UL, 0x14292967UL, 0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL,
	0x53380d13UL, 0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
	0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL,
	0xd6990624UL, 0xf40e3585UL, 0x106aa070UL, 0x19a4c116UL, 0x1e376c08UL,
	0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL,
	0x682e6ff3UL, 0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
	0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

static void block(struct sha256 *c, const unsigned char *p)
{
	unsigned long w[16], a, b, d, e, f, g, h, cc, t1, t2, s0, s1;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = (unsigned long) p[4*i] << 24 | (unsigned long) p[4*i+1] << 16
		     | (unsigned long) p[4*i+2] << 8 | p[4*i+3];
	a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
	e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
	for (i = 0; i < 64; i++) {
		if (i >= 16) {
			unsigned long x = w[(i + 1) & 15], y = w[(i + 14) & 15];

			s0 = ROR(x, 7) ^ ROR(x, 18) ^ (x >> 3);
			s1 = ROR(y, 17) ^ ROR(y, 19) ^ (y >> 10);
			w[i & 15] = M32(w[i & 15] + s0 + w[(i + 9) & 15] + s1);
		}
		t1 = M32(h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25))
			 + ((e & f) ^ (~e & g)) + K[i] + w[i & 15]);
		t2 = M32((ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22))
			 + ((a & b) ^ (a & cc) ^ (b & cc)));
		h = g; g = f; f = e; e = M32(d + t1);
		d = cc; cc = b; b = a; a = M32(t1 + t2);
	}
	c->h[0] = M32(c->h[0] + a); c->h[1] = M32(c->h[1] + b);
	c->h[2] = M32(c->h[2] + cc); c->h[3] = M32(c->h[3] + d);
	c->h[4] = M32(c->h[4] + e); c->h[5] = M32(c->h[5] + f);
	c->h[6] = M32(c->h[6] + g); c->h[7] = M32(c->h[7] + h);
}

void sha256_init(struct sha256 *c)
{
	static const unsigned long iv[8] = {
		0x6a09e667UL, 0xbb67ae85UL, 0x3c6ef372UL, 0xa54ff53aUL,
		0x510e527fUL, 0x9b05688cUL, 0x1f83d9abUL, 0x5be0cd19UL
	};

	memcpy(c->h, iv, sizeof iv);
	c->nl = c->nh = 0;
	c->bn = 0;
}

void sha256_update(struct sha256 *c, const void *v, unsigned n)
{
	const unsigned char *p = v;
	unsigned k;

	c->nl = M32(c->nl + n);
	if (c->nl < n) c->nh++;
	while (n > 0) {
		k = 64 - c->bn < n ? 64 - c->bn : n;
		memcpy(c->b + c->bn, p, k);
		c->bn += k, p += k, n -= k;
		if (c->bn == 64) {
			block(c, c->b);
			c->bn = 0;
		}
	}
}

void sha256_final(struct sha256 *c, unsigned char out[32])
{
	unsigned long hi = M32(c->nh << 3 | c->nl >> 29), lo = M32(c->nl << 3);
	unsigned char z = 0x80, len[8];
	int i;

	sha256_update(c, &z, 1);
	z = 0;
	while (c->bn != 56) sha256_update(c, &z, 1);
	for (i = 0; i < 4; i++) {
		len[i] = (unsigned char) (hi >> (24 - 8 * i));
		len[4 + i] = (unsigned char) (lo >> (24 - 8 * i));
	}
	sha256_update(c, len, 8);
	for (i = 0; i < 32; i++)
		out[i] = (unsigned char) (c->h[i / 4] >> (24 - 8 * (i % 4)));
	memset(c, 0, sizeof *c);
}

void hmac256_init(struct hmac256 *c, const unsigned char *key, unsigned n)
{
	unsigned char k[64], kh[32];
	int i;

	memset(k, 0, sizeof k);
	if (n > 64) {
		sha256_init(&c->in);
		sha256_update(&c->in, key, n);
		sha256_final(&c->in, kh);
		memcpy(k, kh, 32);
	} else
		memcpy(k, key, n);
	for (i = 0; i < 64; i++) k[i] ^= 0x36;
	sha256_init(&c->in);
	sha256_update(&c->in, k, 64);
	for (i = 0; i < 64; i++) k[i] ^= 0x36 ^ 0x5c;
	sha256_init(&c->out);
	sha256_update(&c->out, k, 64);
	memset(k, 0, sizeof k);
	memset(kh, 0, sizeof kh);
}

void hmac256_update(struct hmac256 *c, const void *p, unsigned n)
{
	sha256_update(&c->in, p, n);
}

void hmac256_final(struct hmac256 *c, unsigned char out[32])
{
	unsigned char ih[32];

	sha256_final(&c->in, ih);
	sha256_update(&c->out, ih, 32);
	sha256_final(&c->out, out);
	memset(ih, 0, sizeof ih);
}
