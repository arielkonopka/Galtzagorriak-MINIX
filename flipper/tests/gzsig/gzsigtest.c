/* gzsigtest - host test of gzsig (src/lib/gzsig; spec RS.4, ACS.1, T33a
 * part 2): SHA-512 (FIPS 180-4 examples), Ed25519 (RFC 8032 section 7.1
 * tests 1-3 and SHA(abc), S >= L, bad points), then, if DIR is given (the
 * output of tools/updatetest.py), the signatures of sigs.txt and every
 * update file of cases.txt with the same verdict as gzupdate-check.py, and
 * one flipped bit at each byte of good.update (every STEP-th byte; 1 = all).
 *
 * Usage: gzsigtest [DIR [STEP]] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gzsig.h"

static int fails, checks;

static void ok(int c, const char *what)
{
	checks++;
	if (!c) { fails++; printf("FAIL %s\n", what); }
}

static int unhex(const char *h, gz_u8 *out, int max)
{
	int n = 0;
	unsigned v;

	if (!strcmp(h, "-")) return 0;
	while (h[0] && h[1] && n < max) {
		if (sscanf(h, "%2x", &v) != 1) return -1;
		out[n++] = (gz_u8) v;
		h += 2;
	}
	return *h ? -1 : n;
}

static void hexeq(const gz_u8 *b, const char *h, const char *what)
{
	gz_u8 w[64];
	int n = unhex(h, w, 64);

	ok(n > 0 && !memcmp(b, w, (size_t) n), what);
}

static void sha_tests(void)
{
	static const char *const v[][2] = {
	 { "", "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
	       "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e" },
	 { "abc", "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
	       "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f" },
	 { "abcdefghbcdefghicdefghjdefghijkefghijlfghijklmghijklmnhijklmno"
	   "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
	   "8363fc50ad3c48de4bf38ad1d3d8aaff06e99b0dae2fab774edd8a78a8c916b1"
	   "8745bc1d5facab0b1ab1bb71d0e9bbbf05c5a97e6ab89b5a9fca70ef7d6425df" },
	};
	struct gz_sha512 s;
	gz_u8 h[64], a[1000];
	unsigned i, j;

	for (i = 0; i < 3; i++) {
		gz_sha512(v[i][0], strlen(v[i][0]), h);
		hexeq(h, v[i][1], "sha512 vector");
		/* the same, one byte at a time */
		gz_sha512_init(&s);
		for (j = 0; j < strlen(v[i][0]); j++)
			gz_sha512_update(&s, v[i][0] + j, 1);
		gz_sha512_final(&s, h);
		hexeq(h, v[i][1], "sha512 bytewise");
	}
	memset(a, 'a', sizeof a);
	gz_sha512_init(&s);
	for (i = 0; i < 1000; i++) gz_sha512_update(&s, a, 1000);
	gz_sha512_final(&s, h);
	hexeq(h, "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
		 "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b",
	      "sha512 a million a");
}

static const char *const rfc[][3] = {	/* RFC 8032 7.1: pk, message, sig */
 { "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "-",
   "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
   "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
 { "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
   "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
   "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
 { "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
   "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
   "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
 { "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
   "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
   "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
   "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b589"
   "09351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704" },
};

static void ed_tests(void)
{
	gz_u8 pk[32], m[64], sig[64], x[64];
	int i, n, j;
	clock_t t0;

	for (i = 0; i < 4; i++) {
		unhex(rfc[i][0], pk, 32);
		n = unhex(rfc[i][1], m, 64);
		unhex(rfc[i][2], sig, 64);
		ok(gz_ed25519_verify(sig, pk, m, (unsigned long) n) == 0,
		   "RFC 8032 vector accepted");
		/* a flipped bit in R, S, message, key */
		for (j = 0; j < 64; j += 9) {
			memcpy(x, sig, 64);
			x[j] ^= (gz_u8) (1 << (j % 8));
			ok(gz_ed25519_verify(x, pk, m, (unsigned long) n) != 0,
			   "RFC 8032 flipped signature refused");
		}
		if (n) {
			m[0] ^= 1;
			ok(gz_ed25519_verify(sig, pk, m, (unsigned long) n) != 0,
			   "RFC 8032 flipped message refused");
			m[0] ^= 1;
		}
		pk[3] ^= 0x10;
		ok(gz_ed25519_verify(sig, pk, m, (unsigned long) n) != 0,
		   "RFC 8032 flipped key refused");
	}
	/* S = 2^256 - 1 (>= L), non-canonical y = p, zero key */
	unhex(rfc[0][0], pk, 32);
	unhex(rfc[0][2], sig, 64);
	memset(sig + 32, 0xff, 32);
	ok(gz_ed25519_verify(sig, pk, m, 0) != 0, "S >= L refused");
	unhex(rfc[0][2], sig, 64);
	memset(x, 0xff, 32);
	x[0] = 0xed; x[31] = 0x7f;	/* y = p */
	ok(gz_ed25519_verify(sig, x, m, 0) != 0, "y = p refused");
	memset(x, 0, 32);
	x[31] = 0x80;			/* y = 0, sign 1: no such point */
	ok(gz_ed25519_verify(sig, x, m, 0) != 0, "bad point refused");
	t0 = clock();
	for (i = 0; i < 20; i++) gz_ed25519_verify(sig, pk, m, 0);
	printf("gzsigtest: %.2f ms per signature check here\n",
	       (double) (clock() - t0) * 1000 / CLOCKS_PER_SEC / 20);
}

static void sig_file(const char *dir)
{
	char path[1024], line[2048], tag[8], a[80], b[700], c[140];
	gz_u8 pk[32], m[320], sig[64];
	int n, good, got;
	FILE *f;

	snprintf(path, sizeof path, "%s/sigs.txt", dir);
	if (!(f = fopen(path, "r"))) { ok(0, "sigs.txt missing"); return; }
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '#') continue;
		if (sscanf(line, "%7s %79s %699s %139s", tag, a, b, c) != 4
		    || unhex(a, pk, 32) != 32 || (n = unhex(b, m, 320)) < 0
		    || unhex(c, sig, 64) != 64) {
			ok(0, "sigs.txt line");
			continue;
		}
		good = !strcmp(tag, "good");
		got = gz_ed25519_verify(sig, pk, m, (unsigned long) n) == 0;
		if (got != good) printf("  %s", line);
		ok(got == good, "sigs.txt verdict as cryptography's");
	}
	fclose(f);
}

struct mem { const gz_u8 *p; unsigned long n; };

static long mread(void *ctx, gz_u32 off, void *buf, unsigned long n)
{
	struct mem *m = ctx;

	if (off >= m->n) return 0;
	if (n > m->n - off) n = m->n - off;
	memcpy(buf, m->p + off, n);
	return (long) n;
}

static gz_u8 *load(const char *path, unsigned long *n)
{
	FILE *f = fopen(path, "rb");
	gz_u8 *p;
	long sz;

	if (!f) return 0;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	rewind(f);
	p = malloc((size_t) sz + 1);
	if (p && fread(p, 1, (size_t) sz, f) != (size_t) sz) { free(p); p = 0; }
	fclose(f);
	*n = (unsigned long) sz;
	return p;
}

static gz_u8 keys[2][32];
static gz_u8 buf[4096];

static const char *check(const gz_u8 *p, unsigned long n, long epoch,
			 const char *rel, struct gzu_info *in, unsigned long bufsz)
{
	struct gzu_running r;
	struct mem m;
	unsigned x, y, z;

	m.p = p; m.n = n;
	r.board = "flipper-f7";
	r.epoch = epoch;
	r.downgrade = 0;
	r.have_rel = rel && sscanf(rel, "%u.%u.%u", &x, &y, &z) == 3;
	if (r.have_rel) {
		r.rel[0] = (gz_u8) x; r.rel[1] = (gz_u8) y; r.rel[2] = (gz_u8) z;
	}
	return gzu_check(mread, &m, (gz_u32) n, (const gz_u8 (*)[32]) keys, 1,
			 &r, in, buf, bufsz);
}

static void update_files(const char *dir, int step)
{
	char path[1024], line[512], name[256], rel[32], want[16];
	static struct gzu_info in;
	unsigned long n, i, bad = 0, tried = 0;
	const char *why;
	long epoch;
	gz_u8 *p;
	FILE *f;

	snprintf(path, sizeof path, "%s/trusted.pub", dir);
	if (!(f = fopen(path, "r")) || !fgets(line, sizeof line, f)) {
		ok(0, "trusted.pub missing");
		return;
	}
	fclose(f);
	line[64] = 0;
	ok(unhex(line, keys[0], 32) == 32, "trusted.pub");
	snprintf(path, sizeof path, "%s/cases.txt", dir);
	if (!(f = fopen(path, "r"))) { ok(0, "cases.txt missing"); return; }
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '#') continue;
		if (sscanf(line, "%255s %ld %31s %15s", name, &epoch, rel,
			   want) != 4) {
			ok(0, "cases.txt line");
			continue;
		}
		snprintf(path, sizeof path, "%s/%s", dir, name);
		if (!(p = load(path, &n))) { ok(0, name); continue; }
		why = check(p, n, epoch, rel, &in, sizeof buf);
		if ((why == 0) != !strcmp(want, "good"))
			printf("  %s: want %s, got %s\n", name, want,
			       why ? why : "good");
		ok((why == 0) == !strcmp(want, "good"), "cases.txt verdict");
		/* a 512 B buffer (atea's) gives the same answer */
		ok(check(p, n, epoch, rel, &in, 512) == why
		   || (why && !strcmp(why, in.why)), "512 B buffer");
		free(p);
	}
	fclose(f);
	snprintf(path, sizeof path, "%s/good.update", dir);
	if (!(p = load(path, &n))) { ok(0, "good.update"); return; }
	ok(check(p, n, 6, "0.8.2", &in, sizeof buf) == 0, "good.update good");
	ok(in.nsect >= 3 && in.epoch == 7 && !strcmp(in.board, "flipper-f7"),
	   "good.update fields");
	for (i = 0; i < n; i += (unsigned long) step) {
		p[i] ^= (gz_u8) (1 << (i % 8));
		tried++;
		if (check(p, n, 6, "0.8.2", &in, sizeof buf) == 0) {
			if (!bad) printf("  flip at %#lx accepted\n", i);
			bad++;
		}
		p[i] ^= (gz_u8) (1 << (i % 8));
	}
	checks += (int) tried - 1;
	ok(bad == 0, "one-bit flips all refused");
	free(p);
}

int main(int argc, char **argv)
{
	sha_tests();
	ed_tests();
	if (argc > 1) {
		sig_file(argv[1]);
		update_files(argv[1], argc > 2 ? atoi(argv[2]) : 1);
	}
	if (fails) {
		printf("gzsigtest: %d of %d checks FAILED\n", fails, checks);
		return 1;
	}
	printf("gzsigtest: %d checks passed\n", checks);
	return 0;
}
