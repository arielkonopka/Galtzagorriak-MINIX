/*	pwdauth 2.0 - check a shadow password		Author: Kees J. Bot
 *								7 Feb 1994
 *
 * This program gets as input the key and salt arguments of the crypt(3)
 * function as two null terminated strings.  The crypt result is output as
 * one null terminated string.  Input and output must be <= 1024 characters.
 * The exit code will be 1 on any error.
 *
 * If the key has the form '##name' then the key will be encrypted and the
 * result checked to be equal to the encrypted password in the shadow password
 * file.  If equal than '##name' will be returned, otherwise exit code 2.
 *
 * Otherwise the key will be encrypted normally and the result returned.
 *
 * Hashes: "$5$[rounds=N$]salt$hash" is SHA-256-crypt, the format of glibc
 * and "openssl passwd -5" (Galtzagorriak MINIX, 2026); anything else is
 * the original DES crypt, still accepted for old password files.
 *
 * As a special case, anything matches a null encrypted password to allow
 * a no-password login.
 */
#define nil 0
#define crypt	CRYPT	/* The true crypt is included here. */
#include <sys/types.h>
#include <pwd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LEN	1024
char SHADOW[] = "/etc/shadow";

int main(int argc, char **argv)
{
	char key[LEN];
	char *salt;
	struct passwd *pw;
	int n;

	/* Read input data.  Check if there are exactly two null terminated
	 * strings.
	 */
	n= read(0, key, LEN);
	if (n < 0) return 1;
	salt = key + n;
	n = 0;
	while (salt > key) if (*--salt == 0) n++;
	if (n != 2) return 1;
	salt = key + strlen(key) + 1;

	if (salt[0] == '#' && salt[1] == '#') {
		/* Get the encrypted password from the shadow password file,
		 * encrypt key and compare.
		 */
		setpwfile(SHADOW);

		if ((pw= getpwnam(salt + 2)) == nil) return 2;

		/* A null encrypted password matches a null key, otherwise
		 * do the normal crypt(3) authentication check.
		 */
		if (*pw->pw_passwd == 0 && *key == 0) {
			/* fine */
		} else
		if (strcmp(crypt(key, pw->pw_passwd), pw->pw_passwd) != 0) {
			return 2;
		}
	} else {
		/* Normal encryption. */
		if (*salt == 0 && *key == 0) {
			/* fine */
		} else {
			salt= crypt(key, salt);
		}
	}

	/* Return the (possibly new) salt to the caller. */
	if (write(1, salt, strlen(salt) + 1) < 0) return 1;
	return 0;
}

/* The one and only crypt(3) function. */

/*	From Andy Tanenbaum's book "Computer Networks",
	rewritten in C
*/

struct block {
	unsigned char b_data[64];
};

struct ordering {
	unsigned char o_data[64];
};

static struct block key;

static struct ordering InitialTr = {
	58,50,42,34,26,18,10, 2,60,52,44,36,28,20,12, 4,
	62,54,46,38,30,22,14, 6,64,56,48,40,32,24,16, 8,
	57,49,41,33,25,17, 9, 1,59,51,43,35,27,19,11, 3,
	61,53,45,37,29,21,13, 5,63,55,47,39,31,23,15, 7,
};

static struct ordering FinalTr = {
	40, 8,48,16,56,24,64,32,39, 7,47,15,55,23,63,31,
	38, 6,46,14,54,22,62,30,37, 5,45,13,53,21,61,29,
	36, 4,44,12,52,20,60,28,35, 3,43,11,51,19,59,27,
	34, 2,42,10,50,18,58,26,33, 1,41, 9,49,17,57,25,
};

static struct ordering swap = {
	33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
	49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
	 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,
	17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
};

static struct ordering KeyTr1 = {
	57,49,41,33,25,17, 9, 1,58,50,42,34,26,18,
	10, 2,59,51,43,35,27,19,11, 3,60,52,44,36,
	63,55,47,39,31,23,15, 7,62,54,46,38,30,22,
	14, 6,61,53,45,37,29,21,13, 5,28,20,12, 4,
};

static struct ordering KeyTr2 = {
	14,17,11,24, 1, 5, 3,28,15, 6,21,10,
	23,19,12, 4,26, 8,16, 7,27,20,13, 2,
	41,52,31,37,47,55,30,40,51,45,33,48,
	44,49,39,56,34,53,46,42,50,36,29,32,
};

static struct ordering etr = {
	32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9,
	 8, 9,10,11,12,13,12,13,14,15,16,17,
	16,17,18,19,20,21,20,21,22,23,24,25,
	24,25,26,27,28,29,28,29,30,31,32, 1,
};

static struct ordering ptr = {
	16, 7,20,21,29,12,28,17, 1,15,23,26, 5,18,31,10,
	 2, 8,24,14,32,27, 3, 9,19,13,30, 6,22,11, 4,25,
};

static unsigned char s_boxes[8][64] = {
{	14, 4,13, 1, 2,15,11, 8, 3,10, 6,12, 5, 9, 0, 7,
	 0,15, 7, 4,14, 2,13, 1,10, 6,12,11, 9, 5, 3, 8,
	 4, 1,14, 8,13, 6, 2,11,15,12, 9, 7, 3,10, 5, 0,
	15,12, 8, 2, 4, 9, 1, 7, 5,11, 3,14,10, 0, 6,13,
},

{	15, 1, 8,14, 6,11, 3, 4, 9, 7, 2,13,12, 0, 5,10,
	 3,13, 4, 7,15, 2, 8,14,12, 0, 1,10, 6, 9,11, 5,
	 0,14, 7,11,10, 4,13, 1, 5, 8,12, 6, 9, 3, 2,15,
	13, 8,10, 1, 3,15, 4, 2,11, 6, 7,12, 0, 5,14, 9,
},

{	10, 0, 9,14, 6, 3,15, 5, 1,13,12, 7,11, 4, 2, 8,
	13, 7, 0, 9, 3, 4, 6,10, 2, 8, 5,14,12,11,15, 1,
	13, 6, 4, 9, 8,15, 3, 0,11, 1, 2,12, 5,10,14, 7,
	 1,10,13, 0, 6, 9, 8, 7, 4,15,14, 3,11, 5, 2,12,
},

{	 7,13,14, 3, 0, 6, 9,10, 1, 2, 8, 5,11,12, 4,15,
	13, 8,11, 5, 6,15, 0, 3, 4, 7, 2,12, 1,10,14, 9,
	10, 6, 9, 0,12,11, 7,13,15, 1, 3,14, 5, 2, 8, 4,
	 3,15, 0, 6,10, 1,13, 8, 9, 4, 5,11,12, 7, 2,14,
},

{	 2,12, 4, 1, 7,10,11, 6, 8, 5, 3,15,13, 0,14, 9,
	14,11, 2,12, 4, 7,13, 1, 5, 0,15,10, 3, 9, 8, 6,
	 4, 2, 1,11,10,13, 7, 8,15, 9,12, 5, 6, 3, 0,14,
	11, 8,12, 7, 1,14, 2,13, 6,15, 0, 9,10, 4, 5, 3,
},

{	12, 1,10,15, 9, 2, 6, 8, 0,13, 3, 4,14, 7, 5,11,
	10,15, 4, 2, 7,12, 9, 5, 6, 1,13,14, 0,11, 3, 8,
	 9,14,15, 5, 2, 8,12, 3, 7, 0, 4,10, 1,13,11, 6,
	 4, 3, 2,12, 9, 5,15,10,11,14, 1, 7, 6, 0, 8,13,
},

{	 4,11, 2,14,15, 0, 8,13, 3,12, 9, 7, 5,10, 6, 1,
	13, 0,11, 7, 4, 9, 1,10,14, 3, 5,12, 2,15, 8, 6,
	 1, 4,11,13,12, 3, 7,14,10,15, 6, 8, 0, 5, 9, 2,
	 6,11,13, 8, 1, 4,10, 7, 9, 5, 0,15,14, 2, 3,12,
},

{	13, 2, 8, 4, 6,15,11, 1,10, 9, 3,14, 5, 0,12, 7,
	 1,15,13, 8,10, 3, 7, 4,12, 5, 6,11, 0,14, 9, 2,
	 7,11, 4, 1, 9,12,14, 2, 0, 6,10,13,15, 3, 5, 8,
	 2, 1,14, 7, 4,10, 8,13,15,12, 9, 0, 3, 5, 6,11,
},
};

static int rots[] = {
	1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1,
};

static void transpose(struct block *data, struct ordering *t, int n)
{
	struct block x;

	x = *data;

	while (n-- > 0) {
		data->b_data[n] = x.b_data[t->o_data[n] - 1];
	}
}

static void rotate(struct block *key)
{
	unsigned char *p = key->b_data;
	unsigned char *ep = &(key->b_data[55]);
	int data0 = key->b_data[0], data28 = key->b_data[28];

	while (p++ < ep) *(p-1) = *p;
	key->b_data[27] = data0;
	key->b_data[55] = data28;
}

static struct ordering *EP = &etr;

static void f(int i, struct block *key, struct block *a, struct block *x)
{
	struct block e, ikey, y;
	int k;
	unsigned char *p, *q, *r;

	e = *a;
	transpose(&e, EP, 48);
	for (k = rots[i]; k; k--) rotate(key);
	ikey = *key;
	transpose(&ikey, &KeyTr2, 48);
	p = &(y.b_data[48]);
	q = &(e.b_data[48]);
	r = &(ikey.b_data[48]);
	while (p > y.b_data) {
		*--p = *--q ^ *--r;
	}
	q = x->b_data;
	for (k = 0; k < 8; k++) {
		int xb, r;

		r = *p++ << 5;
		r += *p++ << 3;
		r += *p++ << 2;
		r += *p++ << 1;
		r += *p++;
		r += *p++ << 4;

		xb = s_boxes[k][r];

		*q++ = (xb >> 3) & 1;
		*q++ = (xb>>2) & 1;
		*q++ = (xb>>1) & 1;
		*q++ = (xb & 1);
	}
	transpose(x, &ptr, 32);
}

static void setkey(char *k)
{

	key = *((struct block *) k);
	transpose(&key, &KeyTr1, 56);
}

static void encrypt(char *blck, int edflag)
{
	struct block *p = (struct block *) blck;
	int i;

	transpose(p, &InitialTr, 64);
	for (i = 15; i>= 0; i--) {
		int j = edflag ? i : 15 - i;
		int k;
		struct block b, x;

		b = *p;
		for (k = 31; k >= 0; k--) {
			p->b_data[k] = b.b_data[k + 32];
		}
		f(j, &key, p, &x);
		for (k = 31; k >= 0; k--) {
			p->b_data[k+32] = b.b_data[k] ^ x.b_data[k];
		}
	}
	transpose(p, &swap, 64);
	transpose(p, &FinalTr, 64);
}

static char *des_crypt(const char *pw, const char *salt)
{
	char pwb[66];
	char *cp;
	static char result[16];
	char *p = pwb;
	struct ordering new_etr;
	int i;

	while (*pw && p < &pwb[64]) {
		int j = 7;

		while (j--) {
			*p++ = (*pw >> j) & 01;
		}
		pw++;
		*p++ = 0;
	}
	while (p < &pwb[64]) *p++ = 0;

	setkey(p = pwb);

	while (p < &pwb[66]) *p++ = 0;

	new_etr = etr;
	EP = &new_etr;
	if (salt[0] == 0 || salt[1] == 0) salt = "**";
	for (i = 0; i < 2; i++) {
		char c = *salt++;
		int j;

		result[i] = c;
		if ( c > 'Z') c -= 6 + 7 + '.';	/* c was a lower case letter */
		else if ( c > '9') c -= 7 + '.';/* c was upper case letter */
		else c -= '.';			/* c was digit, '.' or '/'. */
						/* now, 0 <= c <= 63 */
		for (j = 0; j < 6; j++) {
			if ((c >> j) & 01) {
				int t = 6*i + j;
				int temp = new_etr.o_data[t];
				new_etr.o_data[t] = new_etr.o_data[t+24];
				new_etr.o_data[t+24] = temp;
			}
		}
	}

	if (result[1] == 0) result[1] = result[0];

	for (i = 0; i < 25; i++) encrypt(pwb,0);
	EP = &etr;

	p = pwb;
	cp = result+2;
	while (p < &pwb[66]) {
		int c = 0;
		int j = 6;

		while (j--) {
			c <<= 1;
			c |= *p++;
		}
		c += '.';		/* becomes >= '.' */
		if (c > '9') c += 7;	/* not in [./0-9], becomes upper */
		if (c > 'Z') c += 6;	/* not in [A-Z], becomes lower */
		*cp++ = c;
	}
	*cp = 0;
	return result;
}


/* SHA-256 (FIPS 180-4) and SHA-256-crypt (U. Drepper, "Unix crypt using
 * SHA-256 and SHA-512", the $5$ scheme of glibc).
 */
typedef unsigned long u32;	/* at least 32 bits */

struct sha256 {
	u32 h[8];
	unsigned char buf[64];
	unsigned long len;		/* bytes hashed so far */
};

static const u32 K256[64] = {
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

static void sha_block(struct sha256 *c, const unsigned char *p)
{
	u32 w[64], a, b, d, e, f, g, h, cc, t1, t2;
	int i;

	for (i = 0; i < 16; i++, p += 4)
		w[i] = ((u32) p[0] << 24) | ((u32) p[1] << 16)
					| ((u32) p[2] << 8) | p[3];
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

static void sha_init(struct sha256 *c)
{
	static const u32 h0[8] = {
		0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
		0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
	};
	memcpy(c->h, h0, sizeof(c->h));
	c->len = 0;
}

static void sha_add(struct sha256 *c, const void *data, size_t n)
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

static void sha_done(struct sha256 *c, unsigned char out[32])
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

static const char itoa64[] =
	"./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static char *b64(char *cp, unsigned b2, unsigned b1, unsigned b0, int n)
{
	u32 w = ((u32) b2 << 16) | ((u32) b1 << 8) | b0;

	while (n-- > 0) {
		*cp++ = itoa64[w & 0x3f];
		w >>= 6;
	}
	return cp;
}

#define ROUNDS_DEFAULT	5000
#define ROUNDS_MIN	1000
#define ROUNDS_MAX	999999999UL

static char *sha256_crypt(const char *key, const char *salt)
{
	static char result[3 + 17 + 17 + 43 + 1];
	static const unsigned char order[10][3] = {
		{0,10,20},{21,1,11},{12,22,2},{3,13,23},{24,4,14},
		{15,25,5},{6,16,26},{27,7,17},{18,28,8},{9,19,29}
	};
	struct sha256 c, alt;
	unsigned char a[32], dp[32], ds[32];
	unsigned long rounds = ROUNDS_DEFAULT, r, i;
	size_t klen = strlen(key), slen;
	int custom = 0;
	char *cp, *end;

	salt += 3;				/* skip "$5$" */
	if (strncmp(salt, "rounds=", 7) == 0) {
		r = strtoul(salt + 7, &end, 10);
		if (*end == '$') {
			salt = end + 1;
			rounds = r < ROUNDS_MIN ? ROUNDS_MIN
				: r > ROUNDS_MAX ? ROUNDS_MAX : r;
			custom = 1;
		}
	}
	for (slen = 0; slen < 16 && salt[slen] && salt[slen] != '$'; slen++) {}

	/* B = H(key salt key) */
	sha_init(&alt);
	sha_add(&alt, key, klen); sha_add(&alt, salt, slen);
	sha_add(&alt, key, klen);
	sha_done(&alt, a);
	/* A = H(key salt B... bits) */
	sha_init(&c);
	sha_add(&c, key, klen); sha_add(&c, salt, slen);
	for (i = klen; i > 32; i -= 32) sha_add(&c, a, 32);
	sha_add(&c, a, i);
	for (i = klen; i > 0; i >>= 1)
		if (i & 1) sha_add(&c, a, 32); else sha_add(&c, key, klen);
	sha_done(&c, a);
	/* DP = H(key x klen), DS = H(salt x (16 + A[0])) */
	sha_init(&alt);
	for (i = 0; i < klen; i++) sha_add(&alt, key, klen);
	sha_done(&alt, dp);
	sha_init(&alt);
	for (i = 0; i < 16 + (unsigned) a[0]; i++) sha_add(&alt, salt, slen);
	sha_done(&alt, ds);
	/* P and S are DP and DS repeated to klen and slen bytes; they are fed
	 * from dp/ds directly instead of being stored.
	 */
#define ADD_P(c)	do { size_t n_ = klen; \
		while (n_ > 32) { sha_add(c, dp, 32); n_ -= 32; } \
		sha_add(c, dp, n_); } while (0)
	for (r = 0; r < rounds; r++) {
		sha_init(&c);
		if (r & 1) ADD_P(&c); else sha_add(&c, a, 32);
		if (r % 3) sha_add(&c, ds, slen);	/* slen <= 16 < 32 */
		if (r % 7) ADD_P(&c);
		if (r & 1) sha_add(&c, a, 32); else ADD_P(&c);
		sha_done(&c, a);
	}

	cp = result;
	strcpy(cp, "$5$"); cp += 3;
	if (custom) {
		sprintf(cp, "rounds=%lu$", rounds);
		cp += strlen(cp);
	}
	memcpy(cp, salt, slen); cp += slen;
	*cp++ = '$';
	for (i = 0; i < 10; i++)
		cp = b64(cp, a[order[i][0]], a[order[i][1]], a[order[i][2]], 4);
	cp = b64(cp, 0, a[31], a[30], 3);
	*cp = 0;
	memset(a, 0, sizeof(a)); memset(dp, 0, sizeof(dp));
	memset(&c, 0, sizeof(c)); memset(&alt, 0, sizeof(alt));
	return result;
}

char *crypt(const char *key, const char *salt)
{
	if (strncmp(salt, "$5$", 3) == 0) return sha256_crypt(key, salt);
	return des_crypt(key, salt);
}
