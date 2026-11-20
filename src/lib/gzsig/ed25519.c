/* ed25519.c - Ed25519 signature check (RFC 8032 section 5.1.7, pure) for
 * gzsig (spec RS.4).  After TweetNaCl (Bernstein, van Gastel, Janssen,
 * Lange, Schwabe, Smetsers; public domain), verification only, with:
 *   - field elements of 16 x 16-bit limbs in int32 (64 B; products in
 *     int64), so the whole check needs about 1.5 KB of stack;
 *   - S < L and canonical point encodings required (RFC 8032 5.1.7 step 1);
 *   - no shifts of negative numbers (UBSan clean).
 * Not constant time: it handles only public data. */
#include "gzsig.h"

#ifdef __GNUC__
#define NOINLINE __attribute__((noinline))	/* keeps frames apart: stack */
#else
#define NOINLINE
#endif

typedef int fe[16];			/* 16-bit limbs, little endian */
typedef gz_i64 fe64[16];

static const fe fe0 = { 0 }, fe1 = { 1 };
static const fe D = { 0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d,
	0x0070, 0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203 };
static const fe D2 = { 0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a,
	0x00e0, 0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406 };
/* The base point B in extended coordinates (X, Y, Z = 1, T = XY). */
static const fe B[4] = {
	{ 0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c, 0xdc5c,
	  0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169 },
	{ 0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
	  0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666 },
	{ 1 },
	{ 0xdda3, 0xa5b7, 0x8ab3, 0x6dde, 0x52f5, 0x7751, 0x9f80, 0x20f0, 0xe37d,
	  0x64ab, 0x4e8e, 0x66ea, 0x7665, 0xd78b, 0x5f0f, 0x6787 },
};
static const fe SQRTM1 = { 0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f,
	0x1806, 0x2f43, 0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480,
	0x2b83 };
/* L = 2^252 + 27742317777372353535851937790883648493, little endian */
static const gz_u8 L[32] = { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
	0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0x10 };

static void set(fe r, const fe a)
{
	int i;

	for (i = 0; i < 16; i++) r[i] = a[i];
}

/* Carry: every limb to [0, 2^16), the top carry folded back as 38 * c
 * (2^256 = 38 mod p). */
static void carry(fe64 o)
{
	gz_i64 c;
	int i;

	for (i = 0; i < 16; i++) {
		o[i] += 65536;
		c = o[i] >> 16;		/* arithmetic shift: gcc and clang */
		if (i < 15) o[i + 1] += c - 1;
		else o[0] += 38 * (c - 1);
		o[i] -= c * 65536;
	}
}

static void add(fe o, const fe a, const fe b)
{
	int i;

	for (i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void sub(fe o, const fe a, const fe b)
{
	int i;

	for (i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void mul(fe o, const fe a, const fe b)
{
	gz_i64 t[31];
	int i, j;

	for (i = 0; i < 31; i++) t[i] = 0;
	for (i = 0; i < 16; i++)
		for (j = 0; j < 16; j++) t[i + j] += (gz_i64) a[i] * b[j];
	for (i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
	carry(t);
	carry(t);
	for (i = 0; i < 16; i++) o[i] = (int) t[i];
}

static void sq(fe o, const fe a)
{
	mul(o, a, a);
}

/* The canonical 32 bytes of a (fully reduced mod p). */
static void pack(gz_u8 *o, const fe a)
{
	fe64 t, m;
	gz_i64 b;
	int i, j;

	for (i = 0; i < 16; i++) t[i] = a[i];
	carry(t);
	carry(t);
	carry(t);
	for (j = 0; j < 2; j++) {
		m[0] = t[0] - 0xffed;
		for (i = 1; i < 15; i++) {
			m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
			m[i - 1] &= 0xffff;
		}
		m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
		b = (m[15] >> 16) & 1;
		m[14] &= 0xffff;
		if (!b) for (i = 0; i < 16; i++) t[i] = m[i];
	}
	for (i = 0; i < 16; i++) {
		o[2 * i] = (gz_u8) (t[i] & 0xff);
		o[2 * i + 1] = (gz_u8) (t[i] >> 8);
	}
}

static int neq(const fe a, const fe b)
{
	gz_u8 c[32], d[32];
	int i, r = 0;

	pack(c, a);
	pack(d, b);
	for (i = 0; i < 32; i++) r |= c[i] ^ d[i];
	return r != 0;
}

static int parity(const fe a)
{
	gz_u8 d[32];

	pack(d, a);
	return d[0] & 1;
}

static void unpack(fe o, const gz_u8 *n)
{
	int i;

	for (i = 0; i < 16; i++) o[i] = n[2 * i] | n[2 * i + 1] << 8;
	o[15] &= 0x7fff;
}

static void inv(fe o, const fe in)
{
	fe c;
	int a;

	set(c, in);
	for (a = 253; a >= 0; a--) {
		sq(c, c);
		if (a != 2 && a != 4) mul(c, c, in);
	}
	set(o, c);
}

static void pow2523(fe o, const fe in)
{
	fe c;
	int a;

	set(c, in);
	for (a = 250; a >= 0; a--) {
		sq(c, c);
		if (a != 1) mul(c, c, in);
	}
	set(o, c);
}

/* Points in extended coordinates (X, Y, Z, T).  p += q. */
static void padd(fe p[4], fe q[4])
{
	fe a, b, c, d, t;

	sub(a, p[1], p[0]);
	sub(t, q[1], q[0]);
	mul(a, a, t);
	add(b, p[0], p[1]);
	add(t, q[0], q[1]);
	mul(b, b, t);
	mul(c, p[3], q[3]);
	mul(c, c, D2);
	mul(d, p[2], q[2]);
	add(d, d, d);
	sub(t, b, a);			/* e */
	add(b, b, a);			/* h */
	sub(a, d, c);			/* f */
	add(d, d, c);			/* g */
	mul(p[0], t, a);
	mul(p[1], b, d);
	mul(p[2], d, a);
	mul(p[3], t, b);
}

/* p = s * q (s: 32 bytes little endian; q is only read, p != q).  Public
 * data only, so a plain double-and-add. */
static NOINLINE void smul(fe p[4], fe q[4], const gz_u8 *s)
{
	int i;

	set(p[0], fe0);
	set(p[1], fe1);
	set(p[2], fe1);
	set(p[3], fe0);
	for (i = 255; i >= 0; i--) {
		padd(p, p);
		if ((s[i / 8] >> (i & 7)) & 1) padd(p, q);
	}
}

static NOINLINE void ppack(gz_u8 *r, fe p[4])
{
	fe zi, tx, ty;

	inv(zi, p[2]);
	mul(tx, p[0], zi);
	mul(ty, p[1], zi);
	pack(r, ty);
	r[31] ^= (gz_u8) (parity(tx) << 7);
}

/* r = -A for the encoding s; -1 if s is not a canonical point. */
static NOINLINE int unpackneg(fe r[4], const gz_u8 s[32])
{
	fe t, chk, num, den, den2;
	gz_u8 y[32];
	int i;

	set(r[2], fe1);
	unpack(r[1], s);
	pack(y, r[1]);			/* y < p (canonical) */
	for (i = 0; i < 31; i++) if (y[i] != s[i]) return -1;
	if (y[31] != (s[31] & 0x7f)) return -1;
	sq(num, r[1]);
	mul(den, num, D);
	sub(num, num, r[2]);		/* y^2 - 1 */
	add(den, r[2], den);		/* d y^2 + 1 */
	sq(den2, den);
	sq(t, den2);
	mul(t, t, den2);		/* den^6 */
	mul(t, t, num);
	mul(t, t, den);			/* num den^7 */
	pow2523(t, t);
	mul(t, t, num);
	mul(t, t, den);
	mul(t, t, den);
	mul(r[0], t, den);		/* candidate x */
	sq(chk, r[0]);
	mul(chk, chk, den);
	if (neq(chk, num)) mul(r[0], r[0], SQRTM1);
	sq(chk, r[0]);
	mul(chk, chk, den);
	if (neq(chk, num)) return -1;
	if (!neq(r[0], fe0) && (s[31] >> 7)) return -1;	/* x = 0, sign 1 */
	if (parity(r[0]) == (s[31] >> 7)) sub(r[0], fe0, r[0]);
	mul(r[3], r[0], r[1]);
	return 0;
}

/* r (64 bytes, little endian) reduced mod L into r[0..32). */
static NOINLINE void reduce(gz_u8 *r)
{
	gz_i64 x[64], c;
	int i, j;

	for (i = 0; i < 64; i++) x[i] = r[i];
	for (i = 63; i >= 32; i--) {
		c = 0;
		for (j = i - 32; j < i - 12; j++) {
			x[j] += c - 16 * x[i] * L[j - (i - 32)];
			c = (x[j] + 128) >> 8;
			x[j] -= c * 256;
		}
		x[j] += c;
		x[i] = 0;
	}
	c = 0;
	for (j = 0; j < 32; j++) {
		x[j] += c - (x[31] >> 4) * L[j];
		c = x[j] >> 8;
		x[j] &= 255;
	}
	for (j = 0; j < 32; j++) x[j] -= c * L[j];
	for (i = 0; i < 32; i++) {
		x[i + 1] += x[i] >> 8;
		r[i] = (gz_u8) (x[i] & 255);
	}
}

/* s < L, both 32 bytes little endian. */
static int below_l(const gz_u8 *s)
{
	int i;

	for (i = 31; i >= 0; i--) {
		if (s[i] < L[i]) return 1;
		if (s[i] > L[i]) return 0;
	}
	return 0;
}

/* h = SHA-512(R || A || M), its own frame (stack). */
static NOINLINE void hram(gz_u8 h[64], const gz_u8 *sig, const gz_u8 *pk,
			  const gz_u8 *m, unsigned long n)
{
	struct gz_sha512 s;

	gz_sha512_init(&s);
	gz_sha512_update(&s, sig, 32);
	gz_sha512_update(&s, pk, 32);
	gz_sha512_update(&s, m, n);
	gz_sha512_final(&s, h);
}

int gz_ed25519_verify(const gz_u8 sig[64], const gz_u8 pk[32],
		      const gz_u8 *m, unsigned long n)
{
	fe p[4], q[4];
	gz_u8 h[64], t[32];
	int i, r = 0;

	if (!below_l(sig + 32)) return -1;
	hram(h, sig, pk, m, n);
	reduce(h);
	/* [S]B - [h]A, compared with R; two points of stack */
	if (unpackneg(q, pk)) return -1;
	smul(p, q, h);			/* p = -hA */
	smul(q, (fe *) B, sig + 32);	/* q = SB (B is only read) */
	padd(p, q);
	ppack(t, p);
	for (i = 0; i < 32; i++) r |= t[i] ^ sig[i];
	return r ? -1 : 0;
}
