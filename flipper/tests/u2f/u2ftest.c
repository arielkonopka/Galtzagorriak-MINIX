/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host tests of libu2f (issue #420): SHA-256 and HMAC vectors, the U2F
 * messages (register, authenticate, version, every error) and the U2FHID
 * framing (init, ping over many reports, lock, sequence and busy errors),
 * a fuzz, and the same flows with the real P-256 (micro-ecc), whose
 * signatures are verified.  In the first part P-256 is a fake: pub = 04 SHA(priv) SHA(SHA(priv)),
 * sig = hash HMAC(priv, hash), so the test can tell what was signed with
 * which key. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "u2f.h"
#include "sha256.h"
#include "p256.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, \
	__LINE__, #c); fails++; } } while (0)

static unsigned long rnd = 1;
static int save_fail, events[8], saved_n;
static unsigned long saved;
static unsigned char out[64][U2F_PKT];
static int nout;

static void f_random(void *c, unsigned char *b, unsigned n)
{
	(void) c;
	while (n--) {
		rnd = rnd * 1103515245UL + 12345UL;
		*b++ = (unsigned char) (rnd >> 16);
	}
}

static void sha(const void *p, unsigned n, unsigned char o[32])
{
	struct sha256 s;

	sha256_init(&s);
	sha256_update(&s, p, n);
	sha256_final(&s, o);
}

static int f_pub(void *c, const unsigned char priv[32], unsigned char pub[65])
{
	(void) c;
	pub[0] = 4;
	sha(priv, 32, pub + 1);
	sha(pub + 1, 32, pub + 33);
	return 0;
}

static int f_sign(void *c, const unsigned char priv[32],
		  const unsigned char hash[32], unsigned char sig[64])
{
	struct hmac256 h;

	(void) c;
	memcpy(sig, hash, 32);
	hmac256_init(&h, priv, 32);
	hmac256_update(&h, hash, 32);
	hmac256_final(&h, sig + 32);
	return 0;
}

static int f_save(void *c, unsigned long v)
{
	(void) c;
	if (save_fail) return -1;
	saved = v;
	saved_n++;
	return 0;
}

static void f_event(void *c, int ev)
{
	(void) c;
	if (ev >= 0 && ev < 8) events[ev]++;
}

static void f_send(void *c, const unsigned char p[U2F_PKT])
{
	(void) c;
	if (nout < 64) memcpy(out[nout], p, U2F_PKT);
	nout++;
}

static const struct u2f_ops ops = { NULL, f_random, f_pub, f_sign, f_save,
				    f_event, f_send };
static const unsigned char cert[300] = { 0x30, 0x82, 0x01, 0x28 };

static void hexeq(const unsigned char *b, const char *hex, int line)
{
	char s[65];
	int i;

	for (i = 0; i < 32; i++) sprintf(s + 2 * i, "%02x", b[i]);
	if (strcmp(s, hex) != 0) {
		printf("FAIL line %d: %s\n          != %s\n", line, s, hex);
		fails++;
	}
}

static void vectors(void)
{
	unsigned char o[32], m[1000];
	struct hmac256 h;
	struct sha256 s;
	int i;

	sha("abc", 3, o);
	hexeq(o, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", __LINE__);
	sha("", 0, o);
	hexeq(o, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", __LINE__);
	sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, o);
	hexeq(o, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", __LINE__);
	memset(m, 'a', sizeof m);	/* a million 'a', in odd pieces */
	sha256_init(&s);
	for (i = 0; i < 1000; i++) sha256_update(&s, m, 999);
	sha256_update(&s, m, 1000);
	sha256_final(&s, o);
	hexeq(o, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", __LINE__);
	hmac256_init(&h, (const unsigned char *) "Jefe", 4);	/* RFC 4231 #2 */
	hmac256_update(&h, "what do ya want for nothing?", 28);
	hmac256_final(&h, o);
	hexeq(o, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", __LINE__);
	memset(m, 0xaa, 131);		/* RFC 4231 #6: a 131-byte key */
	hmac256_init(&h, m, 131);
	hmac256_update(&h, "Test Using Larger Than Block-Size Key - Hash Key First", 54);
	hmac256_final(&h, o);
	hexeq(o, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", __LINE__);
}

static void setup(struct u2f *u)
{
	memset(u, 0, sizeof *u);
	u->ops = &ops;
	memset(u->device_key, 0x42, 32);
	memset(u->cert_key, 0x17, 32);
	u->cert = cert;
	u->cert_len = sizeof cert;
	memset(events, 0, sizeof events);
	save_fail = 0;
}

static unsigned apdu(unsigned char *b, int ins, int p1, const unsigned char *d,
		     unsigned lc)
{
	b[0] = 0, b[1] = (unsigned char) ins, b[2] = (unsigned char) p1, b[3] = 0;
	b[4] = 0, b[5] = (unsigned char) (lc >> 8), b[6] = (unsigned char) lc;
	if (lc > 0) memcpy(b + 7, d, lc);
	b[7 + lc] = 0, b[8 + lc] = 0;	/* Le */
	return 9 + lc;
}

static unsigned swof(const unsigned char *b, unsigned n)
{
	return (unsigned) b[n - 2] << 8 | b[n - 1];
}

static void messages(void)
{
	struct u2f u;
	unsigned char b[U2F_MAXMSG], req[200], handle[64], pub[65];
	unsigned char hash[32], priv[32], z = 0, sig[64];
	struct hmac256 h;
	struct sha256 s;
	unsigned n, dl;

	setup(&u);
	n = apdu(b, 3, 0, NULL, 0);
	n = u2f_msg(&u, b, n, sizeof b);
	CHECK(n == 8 && memcmp(b, "U2F_V2\x90\x00", 8) == 0);
	b[0] = 0, b[1] = 3, b[2] = b[3] = 0;		/* short form */
	CHECK(u2f_msg(&u, b, 4, sizeof b) == 8);
	b[0] = 0x80, b[1] = 3;
	CHECK(u2f_msg(&u, b, 4, sizeof b) == 2 && swof(b, 2) == 0x6E00);
	b[0] = 0, b[1] = 0x40;
	CHECK(u2f_msg(&u, b, 4, sizeof b) == 2 && swof(b, 2) == 0x6D00);
	CHECK(u2f_msg(&u, b, 3, sizeof b) == 2 && swof(b, 2) == 0x6700);

	/* register: the challenge 0x11.., the app id 0x22.. */
	memset(req, 0x11, 32);
	memset(req + 32, 0x22, 32);
	n = apdu(b, 1, 0, req, 64);
	CHECK(u2f_msg(&u, b, n, sizeof b) == 2 && swof(b, 2) == 0x6985);
	CHECK(events[U2F_EV_REGISTER] == 1);
	n = apdu(b, 1, 0, req, 63);			/* short */
	CHECK(u2f_msg(&u, b, n, sizeof b) == 2 && swof(b, 2) == 0x6700);
	n = apdu(b, 1, 0, req, 64);
	CHECK(u2f_msg(&u, b, n - 3, sizeof b) == 2 && swof(b, 2) == 0x6700);
	u2f_present(&u);
	n = apdu(b, 1, 0, req, 64);
	n = u2f_msg(&u, b, n, sizeof b);
	CHECK(swof(b, n) == 0x9000);
	CHECK(b[0] == 5 && b[1] == 4 && b[66] == 64);
	CHECK(memcmp(b + 67 + 64, cert, sizeof cert) == 0);
	CHECK(u.present == 0);
	memcpy(pub, b + 1, 65);
	memcpy(handle, b + 67, 64);
	/* the key is stock's: HMAC(device key, app || nonce) */
	hmac256_init(&h, u.device_key, 32);
	hmac256_update(&h, req + 32, 32);
	hmac256_update(&h, handle + 32, 32);
	hmac256_final(&h, priv);
	f_pub(NULL, priv, pub);
	CHECK(memcmp(pub, b + 1, 65) == 0);
	/* the attestation signs 00 app chal handle pub with the cert key */
	sha256_init(&s);
	sha256_update(&s, &z, 1);
	sha256_update(&s, req + 32, 32);
	sha256_update(&s, req, 32);
	sha256_update(&s, handle, 64);
	sha256_update(&s, pub, 65);
	sha256_final(&s, hash);
	f_sign(NULL, u.cert_key, hash, sig);
	dl = n - 2 - (67 + 64 + sizeof cert);
	CHECK(b[67 + 64 + sizeof cert] == 0x30 && dl >= 70 && dl <= 72);
	{
		unsigned char der[72];

		CHECK(u2f_der_sig(der, sig) == dl);
		CHECK(memcmp(der, b + 67 + 64 + sizeof cert, dl) == 0);
	}

	/* authenticate with that handle */
	memset(req, 0x33, 32);				/* a new challenge */
	req[64] = 64;
	memcpy(req + 65, handle, 64);
	n = apdu(b, 2, 0x07, req, 129);			/* check-only */
	CHECK(u2f_msg(&u, b, n, sizeof b) == 2 && swof(b, 2) == 0x6985);
	n = apdu(b, 2, 0x03, req, 129);			/* not present */
	CHECK(u2f_msg(&u, b, n, sizeof b) == 2 && swof(b, 2) == 0x6985);
	CHECK(saved_n == 0);
	u2f_present(&u);
	n = apdu(b, 2, 0x03, req, 129);
	n = u2f_msg(&u, b, n, sizeof b);
	CHECK(swof(b, n) == 0x9000 && b[0] == 1);
	CHECK(b[1] == 0 && b[2] == 0 && b[3] == 0 && b[4] == 1);
	CHECK(saved == 1 && u.counter == 1 && events[U2F_EV_AUTH_OK] == 1);
	{
		unsigned char ctr[4] = { 0, 0, 0, 1 }, fl = 1, der[72];

		sha256_init(&s);
		sha256_update(&s, req + 32, 32);
		sha256_update(&s, &fl, 1);
		sha256_update(&s, ctr, 4);
		sha256_update(&s, req, 32);
		sha256_final(&s, hash);
		f_sign(NULL, priv, hash, sig);
		dl = u2f_der_sig(der, sig);
		CHECK(n == 5 + dl + 2 && memcmp(b + 5, der, dl) == 0);
	}
	/* dont-enforce: signs without presence, flag 0, counter 2 */
	n = apdu(b, 2, 0x08, req, 129);
	n = u2f_msg(&u, b, n, sizeof b);
	CHECK(swof(b, n) == 0x9000 && b[0] == 0 && b[4] == 2);
	/* the counter cannot be saved: nothing signed, not counted */
	save_fail = 1;
	n = apdu(b, 2, 0x08, req, 129);
	CHECK(u2f_msg(&u, b, n, sizeof b) == 2 && swof(b, 2) == 0x6F00);
	CHECK(u.counter == 2);
	save_fail = 0;
	/* another app id, a flipped handle, a bad length: 6A80 */
	req[40] ^= 1;
	n = apdu(b, 2, 0x07, req, 129);
	CHECK(u2f_msg(&u, b, n, sizeof b) == 2 && swof(b, 2) == 0x6A80);
	req[40] ^= 1;
	req[70] ^= 0x80;
	n = apdu(b, 2, 0x03, req, 129);
	CHECK(u2f_msg(&u, b, n, sizeof b) == 2 && swof(b, 2) == 0x6A80);
	req[70] ^= 0x80;
	req[64] = 63;
	n = apdu(b, 2, 0x03, req, 129);
	CHECK(u2f_msg(&u, b, n, sizeof b) == 2 && swof(b, 2) == 0x6A80);
	req[64] = 64;
	n = apdu(b, 2, 0x03, req, 128);
	CHECK(u2f_msg(&u, b, n, sizeof b) == 2 && swof(b, 2) == 0x6A80);
	n = apdu(b, 2, 0x05, req, 129);			/* unknown mode */
	CHECK(u2f_msg(&u, b, n, sizeof b) == 2 && swof(b, 2) == 0x6A80);
	/* no certificate, or a buffer too small for it: 6F00 */
	u.cert = NULL;
	u2f_present(&u);
	memset(req, 0x11, 64);
	n = apdu(b, 1, 0, req, 64);
	CHECK(u2f_msg(&u, b, n, sizeof b) == 2 && swof(b, 2) == 0x6F00);
	u.cert = cert;
	n = apdu(b, 1, 0, req, 64);
	CHECK(u2f_msg(&u, b, n, 400) == 2 && swof(b, 2) == 0x6F00);
	/* DER of zeros and of high bits */
	{
		unsigned char der[72], sg[64];

		memset(sg, 0, 64);
		CHECK(u2f_der_sig(der, sg) == 8 && der[3] == 1 && der[4] == 0);
		memset(sg, 0xff, 64);
		CHECK(u2f_der_sig(der, sg) == 72 && der[3] == 33 && der[4] == 0);
	}
}

/* ---- U2FHID ---- */

static void pkt_init(unsigned char *p, const unsigned char *cid, int cmd,
		     unsigned len, const unsigned char *d, unsigned dn)
{
	memset(p, 0, U2F_PKT);
	memcpy(p, cid, 4);
	p[4] = (unsigned char) cmd;
	p[5] = (unsigned char) (len >> 8), p[6] = (unsigned char) len;
	if (dn > 0) memcpy(p + 7, d, dn);
}

static void hid(void)
{
	static struct u2fhid h;
	struct u2f u;
	unsigned char p[U2F_PKT], cid[4], data[300], got[300];
	const unsigned char bc[4] = { 0xff, 0xff, 0xff, 0xff };
	const unsigned char other[4] = { 1, 2, 3, 4 };
	unsigned i, k, have, len;

	setup(&u);
	u2fhid_init(&h, &u);
	/* INIT on the broadcast channel: the nonce back and a new cid */
	pkt_init(p, bc, 0x86, 8, (const unsigned char *) "nonce123", 8);
	nout = 0;
	u2fhid_packet(&h, p, 64, 0);
	CHECK(nout == 1 && memcmp(out[0], bc, 4) == 0 && out[0][4] == 0x86);
	CHECK(out[0][6] == 17 && memcmp(out[0] + 7, "nonce123", 8) == 0);
	CHECK(out[0][19] == 2 && out[0][23] == 1);
	memcpy(cid, out[0] + 15, 4);
	/* MSG on the broadcast channel: refused */
	pkt_init(p, bc, 0x83, 4, (const unsigned char *) "\0\3\0\0", 4);
	nout = 0;
	u2fhid_packet(&h, p, 64, 0);
	CHECK(nout == 1 && out[0][4] == 0xBF && out[0][7] == 1);
	/* VERSION over MSG */
	pkt_init(p, cid, 0x83, 7, (const unsigned char *) "\0\3\0\0\0\0\0", 7);
	nout = 0;
	u2fhid_packet(&h, p, 64, 0);
	CHECK(nout == 1 && out[0][4] == 0x83 && out[0][6] == 8);
	CHECK(memcmp(out[0] + 7, "U2F_V2\x90\x00", 8) == 0);
	/* PING of 300 bytes: 1 + 5 reports each way, the data back */
	for (i = 0; i < sizeof data; i++) data[i] = (unsigned char) (i * 7);
	pkt_init(p, cid, 0x81, 300, data, 57);
	nout = 0;
	u2fhid_packet(&h, p, 64, 0);
	CHECK(nout == 0);
	for (have = 57, k = 0; have < 300; k++, have += 59) {
		memset(p, 0, 64);
		memcpy(p, cid, 4);
		p[4] = (unsigned char) k;
		memcpy(p + 5, data + have, 300 - have < 59 ? 300 - have : 59);
		if (k == 2) {			/* another channel in between */
			unsigned char q[64];

			pkt_init(q, other, 0x81, 1, data, 1);
			u2fhid_packet(&h, q, 64, 0);
			CHECK(nout == 1 && memcmp(out[0], other, 4) == 0
			      && out[0][4] == 0xBF && out[0][7] == 6);
			nout = 0;
		}
		u2fhid_packet(&h, p, 64, 0);
	}
	CHECK(nout == 6);
	len = (unsigned) out[0][5] << 8 | out[0][6];
	CHECK(len == 300 && out[0][4] == 0x81);
	memcpy(got, out[0] + 7, 57);
	for (i = 1, have = 57; i < 6 && i < (unsigned) nout; i++, have += 59) {
		CHECK(out[i][4] == i - 1 && memcmp(out[i], cid, 4) == 0);
		memcpy(got + have, out[i] + 5, 300 - have < 59 ? 300 - have : 59);
	}
	CHECK(memcmp(got, data, 300) == 0);
	/* a wrong sequence number */
	pkt_init(p, cid, 0x81, 100, data, 57);
	u2fhid_packet(&h, p, 64, 0);
	memset(p, 0, 64);
	memcpy(p, cid, 4);
	p[4] = 1;
	nout = 0;
	u2fhid_packet(&h, p, 64, 0);
	CHECK(nout == 1 && out[0][4] == 0xBF && out[0][7] == 4);
	/* too long, unknown command, LOCK and its timeout */
	pkt_init(p, cid, 0x83, U2F_MAXMSG + 1, data, 57);
	nout = 0;
	u2fhid_packet(&h, p, 64, 0);
	CHECK(nout == 1 && out[0][7] == 3);
	pkt_init(p, cid, 0x99, 0, data, 0);
	nout = 0;
	u2fhid_packet(&h, p, 64, 0);
	CHECK(nout == 1 && out[0][7] == 1);
	pkt_init(p, cid, 0x84, 1, (const unsigned char *) "\2", 1);
	nout = 0;
	u2fhid_packet(&h, p, 64, 1000);
	CHECK(nout == 1 && out[0][4] == 0x84);
	pkt_init(p, other, 0x83, 7, (const unsigned char *) "\0\3\0\0\0\0\0", 7);
	nout = 0;
	u2fhid_packet(&h, p, 64, 2500);
	CHECK(nout == 1 && out[0][4] == 0xBF && out[0][7] == 6);
	nout = 0;
	u2fhid_packet(&h, p, 64, 3000);		/* 2 s are over */
	CHECK(nout == 1 && out[0][4] == 0x83);
	/* WINK */
	pkt_init(p, cid, 0x88, 0, data, 0);
	nout = 0;
	u2fhid_packet(&h, p, 64, 3000);
	CHECK(nout == 1 && out[0][4] == 0x88 && events[U2F_EV_WINK] == 1);
	/* short reports and cid 0 are dropped */
	nout = 0;
	u2fhid_packet(&h, p, 4, 0);
	memset(p, 0, 64);
	p[4] = 0x81;
	u2fhid_packet(&h, p, 64, 0);
	CHECK(nout == 0);
	u2f_present(&u);
	u2fhid_reset(&h);
	CHECK(u.present == 0 && h.busy == 0 && h.lock == 0);
}

static void fuzz(void)
{
	static struct u2fhid h;
	struct u2f u;
	unsigned char p[U2F_PKT], b[U2F_MAXMSG];
	unsigned i, j, n;

	setup(&u);
	u2fhid_init(&h, &u);
	srand(420);
	for (i = 0; i < 300000; i++) {
		n = (unsigned) rand() % (U2F_PKT + 1);
		for (j = 0; j < n; j++) p[j] = (unsigned char) rand();
		if (rand() % 2) p[0] = p[1] = p[2] = p[3] = 0xA5;
		if (rand() % 4 == 0) p[5] = 0;
		if (rand() % 3 == 0) u2f_present(&u);
		nout = 0;
		u2fhid_packet(&h, p, n, (long) i);
		CHECK(nout < 20);
		/* raw messages straight in */
		n = (unsigned) rand() % 200;
		for (j = 0; j < n; j++) b[j] = (unsigned char) rand();
		if (n > 1) b[0] = 0, b[1] = (unsigned char) (rand() % 4);
		if (n > 6) b[4] = 0, b[5] = 0;
		j = u2f_msg(&u, b, n, sizeof b);
		CHECK(j >= 2 && j <= sizeof b);
		if (fails) return;
	}
}

/* ---- the real P-256 ---- */

static int r_fill(unsigned char *b, unsigned n)
{
	f_random(NULL, b, n);
	return 0;
}

/* DER (r, s) back to 64 bytes: 0, or -1. */
static int under(const unsigned char *d, unsigned n, unsigned char sig[64])
{
	unsigned i = 2, k, l, part;

	memset(sig, 0, 64);
	if (n < 8 || d[0] != 0x30 || d[1] != n - 2) return -1;
	for (part = 0; part < 2; part++) {
		if (i + 2 > n || d[i] != 2) return -1;
		l = d[i + 1];
		i += 2;
		if (i + l > n || l < 1 || l > 33) return -1;
		k = l;
		if (k == 33) { if (d[i] != 0) return -1; i++, k--; }
		memcpy(sig + part * 32 + 32 - k, d + i, k);
		i += k;
	}
	return i == n ? 0 : -1;
}

static const struct u2f_ops rops = { NULL, f_random, p256_pub, p256_sign,
				     f_save, f_event, f_send };

static void real(void)
{
	struct u2f u;
	unsigned char b[U2F_MAXMSG], req[200], one[32], pub[65], cpub[65];
	unsigned char hash[32], sig[64], z = 0, handle[64];
	struct sha256 s;
	unsigned n, cl = sizeof cert;
	/* RFC 6979 A.2.5: the P-256 key x and its public key U */
	static const unsigned char x[32] = {
		0xC9, 0xAF, 0xA9, 0xD8, 0x45, 0xBA, 0x75, 0x16, 0x6B, 0x5C, 0x21,
		0x57, 0x67, 0xB1, 0xD6, 0x93, 0x4E, 0x50, 0xC3, 0xDB, 0x36, 0xE8,
		0x9B, 0x12, 0x7B, 0x8A, 0x62, 0x2B, 0x12, 0x0F, 0x67, 0x21 };

	CHECK(p256_pub(NULL, x, pub) == 0 && pub[0] == 4);
	hexeq(pub + 1, "60fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6", __LINE__);
	hexeq(pub + 33, "7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299", __LINE__);
	memset(one, 0, 32);			/* 0 is no key */
	CHECK(p256_pub(NULL, one, pub) < 0);
	memset(one, 0xff, 32);			/* above the order: no key */
	CHECK(p256_pub(NULL, one, pub) < 0);
	CHECK(p256_sign(NULL, one, one, sig) < 0);	/* no RNG yet */
	p256_rng(r_fill);

	setup(&u);
	u.ops = &rops;
	CHECK(p256_pub(NULL, u.cert_key, cpub) == 0);
	memset(req, 0x11, 32);
	memset(req + 32, 0x22, 32);
	u2f_present(&u);
	n = u2f_msg(&u, b, apdu(b, 1, 0, req, 64), sizeof b);
	CHECK(swof(b, n) == 0x9000);
	memcpy(pub, b + 1, 65);
	memcpy(handle, b + 67, 64);
	sha256_init(&s);
	sha256_update(&s, &z, 1);
	sha256_update(&s, req + 32, 32);
	sha256_update(&s, req, 32);
	sha256_update(&s, handle, 64);
	sha256_update(&s, pub, 65);
	sha256_final(&s, hash);
	CHECK(under(b + 131 + cl, n - 2 - 131 - cl, sig) == 0);
	CHECK(p256_verify(cpub, hash, sig) == 1);
	hash[0] ^= 1;
	CHECK(p256_verify(cpub, hash, sig) == 0);

	memset(req, 0x44, 32);
	req[64] = 64;
	memcpy(req + 65, handle, 64);
	u2f_present(&u);
	n = u2f_msg(&u, b, apdu(b, 2, 0x03, req, 129), sizeof b);
	CHECK(swof(b, n) == 0x9000 && b[0] == 1);
	sha256_init(&s);
	sha256_update(&s, req + 32, 32);
	sha256_update(&s, b, 5);		/* flags and counter */
	sha256_update(&s, req, 32);
	sha256_final(&s, hash);
	CHECK(under(b + 5, n - 7, sig) == 0);
	CHECK(p256_verify(pub, hash, sig) == 1);
	CHECK(p256_verify(cpub, hash, sig) == 0);	/* not the cert's */
}

int main(void)
{
	vectors();
	messages();
	hid();
	fuzz();
	real();
	if (fails) { printf("u2f: %d failed\n", fails); return 1; }
	printf("u2f: all tests passed\n");
	return 0;
}
