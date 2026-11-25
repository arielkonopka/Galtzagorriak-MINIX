/* pgpenc - see pgpenc.h.  RFC 4880: PKESK v3 (RSA, PKCS#1 v1.5) and SEIPD
 * v1 (AES-256 CFB, no resync, MDC).  Everything streams in 512 byte chunks
 * (OpenPGP partial body lengths), so a file of any size needs the same
 * RAM.  The big number part is a plain Montgomery exponentiation with the
 * public exponent, nothing secret goes through it except the session key,
 * which is random per message. */
#include <string.h>
#include "pgpenc.h"

#define CHUNK	512
#define LIMBS	(PGP_NMAX / 4)

/*====================== SHA-1 =========================================*/
struct sha1 { uint32_t h[5]; uint64_t n; uint8_t b[64]; unsigned k; };

static uint32_t rol(uint32_t x, int s) { return x << s | x >> (32 - s); }

static void sha1_block(struct sha1 *s, const uint8_t *p)
{
	uint32_t w[80], a, b, c, d, e, f, t;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = (uint32_t) p[4*i] << 24 | (uint32_t) p[4*i+1] << 16
			| (uint32_t) p[4*i+2] << 8 | p[4*i+3];
	for (; i < 80; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
	a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
	for (i = 0; i < 80; i++) {
		if (i < 20) { f = (b & c) | (~b & d); t = 0x5A827999; }
		else if (i < 40) { f = b ^ c ^ d; t = 0x6ED9EBA1; }
		else if (i < 60) { f = (b & c) | (b & d) | (c & d); t = 0x8F1BBCDC; }
		else { f = b ^ c ^ d; t = 0xCA62C1D6; }
		t += rol(a, 5) + f + e + w[i];
		e = d; d = c; c = rol(b, 30); b = a; a = t;
	}
	s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_init(struct sha1 *s)
{
	static const uint32_t iv[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE,
					0x10325476, 0xC3D2E1F0 };
	memcpy(s->h, iv, sizeof iv);
	s->n = 0;
	s->k = 0;
}

static void sha1_add(struct sha1 *s, const uint8_t *p, size_t n)
{
	s->n += n;
	while (n > 0) {
		unsigned t = 64 - s->k < n ? 64 - s->k : (unsigned) n;

		memcpy(s->b + s->k, p, t);
		s->k += t; p += t; n -= t;
		if (s->k == 64) { sha1_block(s, s->b); s->k = 0; }
	}
}

static void sha1_end(struct sha1 *s, uint8_t d[20])
{
	uint64_t bits = s->n * 8;
	uint8_t pad = 0x80, z = 0, l[8];
	int i;

	sha1_add(s, &pad, 1);
	while (s->k != 56) sha1_add(s, &z, 1);
	for (i = 0; i < 8; i++) l[i] = (uint8_t) (bits >> (56 - 8 * i));
	sha1_add(s, l, 8);
	for (i = 0; i < 20; i++) d[i] = (uint8_t) (s->h[i / 4] >> (24 - 8 * (i % 4)));
}

/*====================== AES-256 (encrypt) ==============================*/
static const uint8_t sbox[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b,
	0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
	0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26,
	0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
	0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
	0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed,
	0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f,
	0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
	0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
	0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14,
	0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
	0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
	0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f,
	0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
	0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
	0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f,
	0xb0, 0x54, 0xbb, 0x16
};

struct aes { uint8_t rk[240]; };

static uint8_t xt(uint8_t a) { return (uint8_t) (a << 1 ^ ((a & 0x80) ? 0x1B : 0)); }

static void aes_key(struct aes *a, const uint8_t k[32])
{
	uint8_t rc = 1, t[4], x;
	int i, j;

	memcpy(a->rk, k, 32);
	for (i = 8; i < 60; i++) {
		memcpy(t, a->rk + 4 * (i - 1), 4);
		if (i % 8 == 0) {
			x = t[0];
			t[0] = (uint8_t) (sbox[t[1]] ^ rc);
			t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[x];
			rc = xt(rc);
		} else if (i % 8 == 4)
			for (j = 0; j < 4; j++) t[j] = sbox[t[j]];
		for (j = 0; j < 4; j++)
			a->rk[4 * i + j] = (uint8_t) (a->rk[4 * (i - 8) + j] ^ t[j]);
	}
}

static void aes_enc(const struct aes *a, const uint8_t in[16], uint8_t out[16])
{
	uint8_t s[16], t[16], u;
	int r, i, c;

	for (i = 0; i < 16; i++) s[i] = (uint8_t) (in[i] ^ a->rk[i]);
	for (r = 1; r <= 14; r++) {
		for (i = 0; i < 16; i++) t[i] = sbox[s[i]];
		/* ShiftRows: column c, row j takes from column c + j */
		for (c = 0; c < 4; c++)
			for (i = 0; i < 4; i++)
				s[4 * c + i] = t[4 * ((c + i) & 3) + i];
		if (r < 14)
			for (c = 0; c < 4; c++) {
				uint8_t *p = s + 4 * c, p0 = p[0];

				u = (uint8_t) (p[0] ^ p[1] ^ p[2] ^ p[3]);
				p[0] ^= (uint8_t) (u ^ xt((uint8_t) (p[0] ^ p[1])));
				p[1] ^= (uint8_t) (u ^ xt((uint8_t) (p[1] ^ p[2])));
				p[2] ^= (uint8_t) (u ^ xt((uint8_t) (p[2] ^ p[3])));
				p[3] ^= (uint8_t) (u ^ xt((uint8_t) (p[3] ^ p0)));
			}
		for (i = 0; i < 16; i++) s[i] ^= a->rk[16 * r + i];
	}
	memcpy(out, s, 16);
}

/*====================== RSA public operation ===========================*/
/* Montgomery arithmetic over L 32-bit limbs, little endian. */
static void mont_mul(uint32_t *r, const uint32_t *a, const uint32_t *b,
		     const uint32_t *n, uint32_t n0, int L)
{
	uint32_t t[LIMBS + 2];
	uint64_t c, x;
	uint32_t m;
	int i, j;

	memset(t, 0, sizeof(uint32_t) * (L + 2));
	for (i = 0; i < L; i++) {
		c = 0;
		for (j = 0; j < L; j++) {
			x = (uint64_t) a[j] * b[i] + t[j] + c;
			t[j] = (uint32_t) x;
			c = x >> 32;
		}
		x = (uint64_t) t[L] + c;
		t[L] = (uint32_t) x;
		t[L + 1] = (uint32_t) (x >> 32);
		m = t[0] * n0;
		x = (uint64_t) m * n[0] + t[0];
		c = x >> 32;
		for (j = 1; j < L; j++) {
			x = (uint64_t) m * n[j] + t[j] + c;
			t[j - 1] = (uint32_t) x;
			c = x >> 32;
		}
		x = (uint64_t) t[L] + c;
		t[L - 1] = (uint32_t) x;
		t[L] = t[L + 1] + (uint32_t) (x >> 32);
	}
	/* t < 2n: subtract n once if t >= n */
	{
		int ge = t[L] != 0;

		if (!ge) {
			ge = 1;
			for (j = L - 1; j >= 0; j--)
				if (t[j] != n[j]) { ge = t[j] > n[j]; break; }
		}
		if (ge) {
			int64_t br = 0;

			for (j = 0; j < L; j++) {
				int64_t d = (int64_t) t[j] - n[j] + br;

				r[j] = (uint32_t) d;
				br = d >> 32;
			}
		} else memcpy(r, t, sizeof(uint32_t) * L);
	}
}

/* out = in ^ e mod n; in, out: nlen bytes big endian.  -1: n even/zero. */
static int rsa_pub(const struct pgp_pub *k, const uint8_t *in, uint8_t *out)
{
	uint32_t n[LIMBS], r2[LIMBS], x[LIMBS], acc[LIMBS], one[LIMBS];
	uint32_t n0, inv;
	int L = (k->nlen + 3) / 4, i, bit, started = 0;

	memset(n, 0, sizeof n); memset(x, 0, sizeof x);
	for (i = 0; i < k->nlen; i++) {
		int p = k->nlen - 1 - i;

		n[p / 4] |= (uint32_t) k->n[i] << (8 * (p % 4));
		x[p / 4] |= (uint32_t) in[i] << (8 * (p % 4));
	}
	if ((n[0] & 1) == 0 || k->e < 3) return -1;
	inv = n[0];				/* Newton: n0 = -1/n mod 2^32 */
	for (i = 0; i < 5; i++) inv *= 2 - n[0] * inv;
	n0 = (uint32_t) (0 - inv);
	/* r2 = 2^(64 L) mod n, by doubling 1 */
	memset(r2, 0, sizeof r2);
	r2[0] = 1;
	for (i = 0; i < 64 * L; i++) {
		uint32_t cy = 0;
		int ge = 1, j;

		for (j = 0; j < L; j++) {
			uint32_t nc = r2[j] >> 31;

			r2[j] = r2[j] << 1 | cy;
			cy = nc;
		}
		if (!cy)
			for (j = L - 1; j >= 0; j--)
				if (r2[j] != n[j]) { ge = r2[j] > n[j]; break; }
		if (cy || ge) {
			int64_t br = 0;

			for (j = 0; j < L; j++) {
				int64_t d = (int64_t) r2[j] - n[j] + br;

				r2[j] = (uint32_t) d;
				br = d >> 32;
			}
		}
	}
	memset(one, 0, sizeof one);
	one[0] = 1;
	mont_mul(x, x, r2, n, n0, L);		/* x in Montgomery form */
	memcpy(acc, x, sizeof(uint32_t) * L);
	for (bit = 31; bit >= 0 && !(k->e >> bit & 1); bit--) ;
	for (bit--; bit >= 0; bit--) {		/* the leading 1 is acc = x */
		mont_mul(acc, acc, acc, n, n0, L);
		if (k->e >> bit & 1) mont_mul(acc, acc, x, n, n0, L);
	}
	(void) started;
	mont_mul(acc, acc, one, n, n0, L);	/* out of Montgomery form */
	for (i = 0; i < k->nlen; i++) {
		int p = k->nlen - 1 - i;

		out[i] = (uint8_t) (acc[p / 4] >> (8 * (p % 4)));
	}
	return 0;
}

/*====================== key parsing ====================================*/
static int pkt_next(const uint8_t *b, size_t len, size_t *pos, int *tag,
		    const uint8_t **body, size_t *blen)
{
	size_t p = *pos, l;
	uint8_t h;

	if (p >= len) return 0;
	h = b[p++];
	if (!(h & 0x80)) return -1;
	if (h & 0x40) {				/* new format */
		*tag = h & 0x3F;
		if (p >= len) return -1;
		if (b[p] < 192) l = b[p++];
		else if (b[p] < 224) {
			if (p + 1 >= len) return -1;
			l = ((size_t) (b[p] - 192) << 8) + b[p + 1] + 192;
			p += 2;
		} else if (b[p] == 255) {
			if (p + 4 >= len) return -1;
			l = (size_t) b[p+1] << 24 | (size_t) b[p+2] << 16
				| (size_t) b[p+3] << 8 | b[p+4];
			p += 5;
		} else return -1;		/* partial lengths: not a key */
	} else {				/* old format */
		int lt = h & 3;

		*tag = (h >> 2) & 15;
		if (lt == 3) return -1;
		if (p + (1u << lt) > len) return -1;
		l = 0;
		while (lt-- >= 0 && (size_t) (1u << (h & 3)) > 0) {
			l = l << 8 | b[p++];
			if (lt < 0) break;
		}
	}
	if (l > len - p) return -1;
	*body = b + p;
	*blen = l;
	*pos = p + l;
	return 1;
}

static int mpi(const uint8_t *b, size_t len, size_t *pos, const uint8_t **v,
	       size_t *vl)
{
	size_t bits, n;

	if (*pos + 2 > len) return -1;
	bits = (size_t) b[*pos] << 8 | b[*pos + 1];
	n = (bits + 7) / 8;
	if (*pos + 2 + n > len) return -1;
	*v = b + *pos + 2;
	*vl = n;
	*pos += 2 + n;
	return 0;
}

static int key_packet(const uint8_t *body, size_t bl, struct pgp_pub *k)
{
	size_t pos = 6, nl, el;
	const uint8_t *nv, *ev;
	struct sha1 s;
	uint8_t hdr[3], d[20];
	uint32_t e = 0;
	size_t i;

	if (bl < 8 || body[0] != 4) return -1;
	if (body[5] != 1 && body[5] != 2) return -1;	/* RSA, can encrypt */
	if (mpi(body, bl, &pos, &nv, &nl) < 0 || mpi(body, bl, &pos, &ev, &el) < 0)
		return -1;
	while (nl > 0 && nv[0] == 0) { nv++; nl--; }
	if (nl < 64 || nl > PGP_NMAX || el == 0 || el > 4) return -1;
	for (i = 0; i < el; i++) e = e << 8 | ev[i];
	memcpy(k->n, nv, nl);
	k->nlen = (uint16_t) nl;
	k->e = e;
	hdr[0] = 0x99; hdr[1] = (uint8_t) (bl >> 8); hdr[2] = (uint8_t) bl;
	sha1_init(&s);
	sha1_add(&s, hdr, 3);
	sha1_add(&s, body, bl);
	sha1_end(&s, d);
	memcpy(k->fpr, d, 20);
	memcpy(k->keyid, d + 12, 8);
	return 0;
}

/*====================== armor ==========================================*/
static const char b64[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64v(int c)
{
	return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26
		: c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
}

int pgp_dearmor(const uint8_t *in, size_t len, uint8_t *out, size_t max)
{
	size_t i = 0, o = 0;
	uint32_t acc = 0;
	int nb = 0, v;

	/* skip the BEGIN line and the headers up to the empty line */
	while (i < len && in[i] != '\n') i++;
	i++;
	for (;;) {
		size_t s = i;

		while (i < len && in[i] != '\n') i++;
		if (i >= len) return -1;
		if (i == s || (i == s + 1 && in[s] == '\r')) { i++; break; }
		i++;
	}
	for (; i < len; i++) {
		if (in[i] == '=' || in[i] == '-') break;	/* CRC, END */
		if ((v = b64v(in[i])) < 0) continue;	/* line ends, spaces */
		acc = acc << 6 | (uint32_t) v;
		if (++nb == 4) {
			if (o + 3 > max) return -1;
			out[o++] = (uint8_t) (acc >> 16);
			out[o++] = (uint8_t) (acc >> 8);
			out[o++] = (uint8_t) acc;
			nb = 0;
			acc = 0;
		}
	}
	if (nb == 2) { if (o + 1 > max) return -1; out[o++] = (uint8_t) (acc >> 4); }
	else if (nb == 3) {
		if (o + 2 > max) return -1;
		out[o++] = (uint8_t) (acc >> 10);
		out[o++] = (uint8_t) (acc >> 2);
	} else if (nb == 1) return -1;
	return (int) o;
}

int pgp_key_parse(uint8_t *buf, size_t len, struct pgp_pub *k)
{
	size_t pos = 0, bl;
	int tag, r, have = 0;
	const uint8_t *body;
	struct pgp_pub cand;

	if (len > 10 && memcmp(buf, "-----BEGIN", 10) == 0) {
		int n = pgp_dearmor(buf, len, buf, len);	/* in place */

		if (n < 0) return -1;
		len = (size_t) n;
	}
	memset(k, 0, sizeof *k);
	while ((r = pkt_next(buf, len, &pos, &tag, &body, &bl)) > 0) {
		if (tag == 14 || tag == 6) {
			if (tag == 6 && have) continue;	/* subkeys win */
			if (key_packet(body, bl, &cand) == 0) {
				*k = cand;
				have = 1;
			}
		}
	}
	return r < 0 || !have ? -1 : 0;
}

/*====================== the message ====================================*/
struct pgp_ctx {
	const struct pgp_io *io;
	struct aes aes;
	struct sha1 sha;
	uint8_t fr[16], fre[16];
	unsigned pos;
	uint8_t ob[CHUNK];		/* the encrypted packet's current chunk */
	unsigned on;
	uint8_t lb[CHUNK];		/* the literal packet's current chunk */
	unsigned ln;
};

static int emit(struct pgp_ctx *c, const uint8_t *p, size_t n)
{
	return c->io->write(c->io->arg, p, n);
}

/* the length of a final chunk, one or two octets (n < 8384) */
static int final_len(uint8_t *h, unsigned n)
{
	if (n < 192) { h[0] = (uint8_t) n; return 1; }
	h[0] = (uint8_t) (((n - 192) >> 8) + 192);
	h[1] = (uint8_t) ((n - 192) & 255);
	return 2;
}

static int enc_byte(struct pgp_ctx *c, uint8_t p)
{
	uint8_t e;

	if (c->pos == 0) aes_enc(&c->aes, c->fr, c->fre);
	e = (uint8_t) (p ^ c->fre[c->pos]);
	c->fr[c->pos] = e;
	c->pos = (c->pos + 1) & 15;
	c->ob[c->on++] = e;
	if (c->on == CHUNK) {			/* a full partial chunk */
		uint8_t h = 0xE0 | 9;		/* 2^9 */

		if (emit(c, &h, 1) < 0 || emit(c, c->ob, CHUNK) < 0) return -1;
		c->on = 0;
	}
	return 0;
}

/* plaintext of the message: hashed, then encrypted */
static int plain(struct pgp_ctx *c, const uint8_t *p, size_t n)
{
	size_t i;

	sha1_add(&c->sha, p, n);
	for (i = 0; i < n; i++)
		if (enc_byte(c, p[i]) < 0) return -1;
	return 0;
}

static int lit_add(struct pgp_ctx *c, const uint8_t *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		c->lb[c->ln++] = p[i];
		if (c->ln == CHUNK) {
			uint8_t h = 0xE0 | 9;

			if (plain(c, &h, 1) < 0 || plain(c, c->lb, CHUNK) < 0)
				return -1;
			c->ln = 0;
		}
	}
	return 0;
}

static int pkesk(const struct pgp_pub *k, const struct pgp_io *io,
		 const uint8_t sk[32])
{
	uint8_t em[PGP_NMAX], ct[PGP_NMAX], h[8], r;
	unsigned sum = 0, ps, i, mlen, bits, blen, hl;
	uint8_t *p;

	for (i = 0; i < 32; i++) sum += sk[i];
	ps = k->nlen - 3 - 35;
	em[0] = 0; em[1] = 2;
	if (io->rng(io->arg, em + 2, ps) < 0) return -1;
	for (i = 0; i < ps; i++)
		while (em[2 + i] == 0)
			if (io->rng(io->arg, em + 2 + i, 1) < 0) return -1;
	p = em + 2 + ps;
	*p++ = 0;
	*p++ = 9;				/* AES-256 */
	memcpy(p, sk, 32);
	p += 32;
	*p++ = (uint8_t) (sum >> 8);
	*p++ = (uint8_t) sum;
	if (rsa_pub(k, em, ct) < 0) return -1;
	memset(em, 0, sizeof em);
	for (i = 0; i < k->nlen && ct[i] == 0; i++) ;
	mlen = k->nlen - i;			/* the MPI: no leading zeros */
	if (mlen == 0) return -1;
	r = ct[i];
	for (bits = 8; bits > 0 && !(r & 0x80); bits--) r = (uint8_t) (r << 1);
	bits += 8 * (mlen - 1);
	blen = 1 + 8 + 1 + 2 + mlen;
	h[0] = 0xC1;
	hl = 1 + (unsigned) final_len(h + 1, blen);
	if (io->write(io->arg, h, hl) < 0) return -1;
	h[0] = 3;
	if (io->write(io->arg, h, 1) < 0
	    || io->write(io->arg, k->keyid, 8) < 0) return -1;
	h[0] = 1;				/* RSA */
	h[1] = (uint8_t) (bits >> 8);
	h[2] = (uint8_t) bits;
	if (io->write(io->arg, h, 3) < 0
	    || io->write(io->arg, ct + i, mlen) < 0) return -1;
	return 0;
}

int pgp_encrypt(const struct pgp_pub *k, const struct pgp_io *io,
		const char *name, uint32_t mtime)
{
	struct pgp_ctx cs;			/* ~1.5 KB, on the stack */
	struct pgp_ctx *c = &cs;
	uint8_t sk[32], pre[18], hdr[8], md[22], fin[2], buf[CHUNK];
	size_t nl = name != NULL ? strlen(name) : 0;
	int n, hl;

	if (nl > 255) nl = 255;
	memset(c, 0, sizeof *c);
	c->io = io;
	if (io->rng(io->arg, sk, 32) < 0 || io->rng(io->arg, pre, 16) < 0)
		return -1;
	if (pkesk(k, io, sk) < 0) return -1;
	aes_key(&c->aes, sk);
	memset(sk, 0, sizeof sk);
	sha1_init(&c->sha);
	hdr[0] = 0xD2;				/* SEIPD, partial lengths */
	if (emit(c, hdr, 1) < 0) return -1;
	c->ob[c->on++] = 1;			/* version 1: in the chunk, unencrypted */
	pre[16] = pre[14]; pre[17] = pre[15];
	if (plain(c, pre, 18) < 0) return -1;
	hdr[0] = 0xCB;				/* the literal packet, partial */
	if (plain(c, hdr, 1) < 0) return -1;
	hdr[0] = 'b';
	hdr[1] = (uint8_t) nl;
	if (lit_add(c, hdr, 2) < 0 || lit_add(c, (const uint8_t *) name, nl) < 0)
		return -1;
	hdr[0] = (uint8_t) (mtime >> 24); hdr[1] = (uint8_t) (mtime >> 16);
	hdr[2] = (uint8_t) (mtime >> 8); hdr[3] = (uint8_t) mtime;
	if (lit_add(c, hdr, 4) < 0) return -1;
	while ((n = io->read(io->arg, buf, sizeof buf)) > 0)
		if (lit_add(c, buf, (size_t) n) < 0) return -1;
	if (n < 0) return -1;
	hl = final_len(fin, c->ln);		/* the literal's last chunk */
	if (plain(c, fin, (size_t) hl) < 0 || plain(c, c->lb, c->ln) < 0) return -1;
	md[0] = 0xD3; md[1] = 0x14;		/* the MDC packet */
	sha1_add(&c->sha, md, 2);
	sha1_end(&c->sha, md + 2);
	{
		int i;

		for (i = 0; i < 22; i++)
			if (enc_byte(c, md[i]) < 0) return -1;
	}
	hl = final_len(fin, c->on);		/* the last chunk of the packet */
	if (emit(c, fin, (size_t) hl) < 0 || emit(c, c->ob, c->on) < 0) return -1;
	memset(c, 0, sizeof *c);
	return 0;
}

/*====================== ASCII armor output ============================*/
struct armor {
	const struct pgp_io *io;
	uint8_t t[3];
	unsigned tn, col;
	uint32_t crc;
	int err;
};

static int a_put(struct armor *a, const char *s, size_t n)
{
	return a->io->write(a->io->arg, (const uint8_t *) s, n);
}

static int a_group(struct armor *a, const uint8_t *t, unsigned n)
{
	char q[5];

	q[0] = b64[t[0] >> 2];
	q[1] = b64[(t[0] & 3) << 4 | (n > 1 ? t[1] >> 4 : 0)];
	q[2] = n > 1 ? b64[(t[1] & 15) << 2 | (n > 2 ? t[2] >> 6 : 0)] : '=';
	q[3] = n > 2 ? b64[t[2] & 63] : '=';
	if (a_put(a, q, 4) < 0) return -1;
	if ((a->col += 4) == 64) { a->col = 0; return a_put(a, "\n", 1); }
	return 0;
}

static int a_write(void *arg, const uint8_t *p, size_t n)
{
	struct armor *a = arg;
	size_t i;
	int j;

	for (i = 0; i < n; i++) {
		a->crc ^= (uint32_t) p[i] << 16;
		for (j = 0; j < 8; j++) {
			a->crc <<= 1;
			if (a->crc & 0x1000000) a->crc ^= 0x1864CFB;
		}
		a->t[a->tn++] = p[i];
		if (a->tn == 3) {
			if (a_group(a, a->t, 3) < 0) return -1;
			a->tn = 0;
		}
	}
	return 0;
}

static int a_read(void *arg, uint8_t *b, size_t n)
{
	struct armor *a = arg;

	return a->io->read(a->io->arg, b, n);
}

static int a_rng(void *arg, uint8_t *b, size_t n)
{
	struct armor *a = arg;

	return a->io->rng(a->io->arg, b, n);
}

int pgp_encrypt_armor(const struct pgp_pub *k, const struct pgp_io *io,
		      const char *name, uint32_t mtime)
{
	struct armor a;
	struct pgp_io w;
	uint8_t c[3];
	static const char head[] = "-----BEGIN PGP MESSAGE-----\n\n";
	static const char tail[] = "-----END PGP MESSAGE-----\n";

	memset(&a, 0, sizeof a);
	a.io = io;
	a.crc = 0xB704CE;
	w.arg = &a;
	w.read = a_read;
	w.write = a_write;
	w.rng = a_rng;
	if (a_put(&a, head, sizeof head - 1) < 0) return -1;
	if (pgp_encrypt(k, &w, name, mtime) < 0) return -1;
	if (a.tn > 0 && a_group(&a, a.t, a.tn) < 0) return -1;
	if (a.col > 0 && a_put(&a, "\n", 1) < 0) return -1;
	c[0] = (uint8_t) (a.crc >> 16); c[1] = (uint8_t) (a.crc >> 8);
	c[2] = (uint8_t) a.crc;
	if (a_put(&a, "=", 1) < 0) return -1;
	a.col = 0;
	if (a_group(&a, c, 3) < 0) return -1;
	if (a.col > 0 && a_put(&a, "\n", 1) < 0) return -1;
	return a_put(&a, tail, sizeof tail - 1);
}
