/* rgsha.c - SHA-256 of the region tables (spec Q.3, RQ.5; task T32a).
 *
 * `region set` hashes the card's CC.reg (and its Base: file) and looks the
 * hash up in the list the build made of the shipped files
 * (flipper/tools/mkreghash.py, 34 B per table, const in flash).  A hash not
 * in it is a custom table: `region set` needs -f and logs the hash.
 * SHA-256 (FIPS 180-4) as in pwdauth.c.  No statics: 0 B RAM resident.
 */
#include <stdio.h>
#include <string.h>
#include "modem.h"


static const unsigned long K256[64] = {
	0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
	0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
	0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
	0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
	0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
	0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
	0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
	0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
	0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
	0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
	0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define M32(x)		((x) & 0xffffffffUL)
#define ROR(x, n)	M32(((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(struct mdm_sha256 *c, const unsigned char *p)
{
	unsigned long w[64], a, b, d, e, f, g, h, cc, t1, t2;
	int i;

	for (i = 0; i < 16; i++, p += 4)
		w[i] = ((unsigned long) p[0] << 24) | ((unsigned long) p[1] << 16)
					| ((unsigned long) p[2] << 8) | p[3];
	for (; i < 64; i++) {
		t1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
		t2 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
		w[i] = M32(t1 + w[i-7] + t2 + w[i-16]);
	}
	a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
	e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
	for (i = 0; i < 64; i++) {
		t1 = M32(h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25))
			+ ((e & f) ^ (~e & g)) + K256[i] + w[i]);
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

void mdm_sha_init(struct mdm_sha256 *c)
{
	static const unsigned long h0[8] = {
		0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
		0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
	};
	memcpy(c->h, h0, sizeof(c->h));
	c->len = 0;
}

void mdm_sha_add(struct mdm_sha256 *c, const void *data, unsigned long n)
{
	const unsigned char *p = data;

	while (n > 0) {
		size_t k = c->len % 64, m = 64 - k;
		if (m > n) m = n;
		memcpy(c->buf + k, p, m);
		c->len += m; p += m; n -= m;
		if (c->len % 64 == 0) sha_block(c, c->buf);
	}
}

void mdm_sha_done(struct mdm_sha256 *c, unsigned char out[32])
{
	unsigned long bits = c->len * 8;
	size_t k = c->len % 64;
	int i;

	c->buf[k++] = 0x80;
	if (k > 56) {
		memset(c->buf + k, 0, 64 - k);
		sha_block(c, c->buf);
		k = 0;
	}
	memset(c->buf + k, 0, 56 - k);
	for (i = 0; i < 8; i++)		/* length < 2^32 bits here */
		c->buf[63 - i] = i < 4 ? (bits >> (8 * i)) & 0xff : 0;
	sha_block(c, c->buf);
	for (i = 0; i < 32; i++)
		out[i] = (c->h[i / 4] >> (24 - 8 * (i % 4))) & 0xff;
}

int mdm_rg_sha_file(const char *dir, const char *cc, unsigned char out[32])
{
	struct mdm_sha256 c;
	unsigned char buf[128];
	char path[256];
	FILE *fp;
	size_t n;

	snprintf(path, sizeof(path), "%s/%s.reg", dir, cc);
	if ((fp = fopen(path, "rb")) == NULL) return -1;
	mdm_sha_init(&c);
	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) mdm_sha_add(&c, buf, n);
	n = (size_t) ferror(fp);
	fclose(fp);
	if (n) return -1;
	mdm_sha_done(&c, out);
	return 0;
}

int mdm_rg_sha_find(const struct mdm_rghash *list, int n, const char *cc,
	const unsigned char sha[32], int *same_code)
{
	int i;

	*same_code = -1;
	for (i = 0; i < n; i++) {
		if (list[i].cc[0] != cc[0] || list[i].cc[1] != cc[1]) continue;
		*same_code = i;
		if (memcmp(list[i].sha, sha, 32) == 0) return 1;
	}
	return 0;
}
