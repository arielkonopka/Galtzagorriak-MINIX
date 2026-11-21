/* SPDX-License-Identifier: GPL-3.0-or-later
 * libu2f - U2F raw messages and U2FHID framing, see u2f.h.
 * After Unleashed applications/main/u2f/u2f.c and u2f_hid.c (GPLv3).
 */
#include <string.h>
#include "u2f.h"
#include "sha256.h"

#define INS_REGISTER	0x01
#define INS_AUTH	0x02
#define INS_VERSION	0x03

#define AUTH_CHECK	0x07		/* check-only */
#define AUTH_ENFORCE	0x03		/* enforce-user-presence-and-sign */
#define AUTH_DONT	0x08		/* dont-enforce-user-presence-and-sign */

#define SW_OK		0x9000
#define SW_PRESENCE	0x6985		/* conditions not satisfied */
#define SW_DATA		0x6A80		/* wrong data (key handle) */
#define SW_LENGTH	0x6700
#define SW_INS		0x6D00
#define SW_CLA		0x6E00
#define SW_OTHER	0x6F00

#define HID_INIT	0x80
#define HID_PING	0x81
#define HID_MSG		0x83
#define HID_LOCK	0x84
#define HID_CHINIT	0x86
#define HID_WINK	0x88
#define HID_ERROR	0xBF

static const unsigned char bcast[4] = { 0xFF, 0xFF, 0xFF, 0xFF };

static unsigned sw(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char) (v >> 8);
	p[1] = (unsigned char) v;
	return 2;
}

static void event(struct u2f *u, int ev)
{
	if (u->ops->event != NULL) u->ops->event(u->ops->ctx, ev);
}

void u2f_present(struct u2f *u)
{
	u->present = 1;
}

static unsigned der_int(unsigned char *der, const unsigned char *v,
			unsigned n)
{
	unsigned len = 2;

	der[0] = 0x02;
	while (n > 1 && v[0] == 0) v++, n--;	/* no leading zeros */
	if (v[0] & 0x80) der[len++] = 0;	/* not negative */
	memcpy(der + len, v, n);
	len += n;
	der[1] = (unsigned char) (len - 2);
	return len;
}

unsigned u2f_der_sig(unsigned char *der, const unsigned char sig[64])
{
	unsigned len = 2;

	der[0] = 0x30;
	len += der_int(der + len, sig, 32);
	len += der_int(der + len, sig + 32, 32);
	der[1] = (unsigned char) (len - 2);
	return len;
}

/* priv = HMAC(device key, app id || nonce), the handle's MAC =
 * HMAC(device key, priv || app id): stock's derivation. */
static void derive(const struct u2f *u, const unsigned char *app,
		   const unsigned char *nonce, unsigned char priv[32],
		   unsigned char mac[32])
{
	struct hmac256 h;

	hmac256_init(&h, u->device_key, 32);
	hmac256_update(&h, app, 32);
	hmac256_update(&h, nonce, 32);
	hmac256_final(&h, priv);
	hmac256_init(&h, u->device_key, 32);
	hmac256_update(&h, priv, 32);
	hmac256_update(&h, app, 32);
	hmac256_final(&h, mac);
}

/* REGISTER: data = challenge(32) app id(32).  Answer: 05, the public key
 * (65), 64, the key handle (MAC 32, nonce 32), the certificate, the
 * attestation signature (DER), 9000. */
static unsigned reg(struct u2f *u, unsigned char *buf, unsigned size)
{
	unsigned char chal[32], app[32], nonce[32], priv[32], mac[32];
	unsigned char pub[65], hash[32], sig[64], z = 0;
	struct sha256 s;
	unsigned n;

	if (u->cert == NULL || u->cert_len > U2F_MAXCERT
	    || size < 1 + 65 + 1 + 64 + u->cert_len + 72 + 2) {
		event(u, U2F_EV_ERROR);
		return sw(buf, SW_OTHER);
	}
	event(u, U2F_EV_REGISTER);
	if (!u->present) return sw(buf, SW_PRESENCE);
	memcpy(chal, buf + 7, 32);
	memcpy(app, buf + 7 + 32, 32);
	u->ops->random(u->ops->ctx, nonce, 32);
	derive(u, app, nonce, priv, mac);
	if (u->ops->p256_pub(u->ops->ctx, priv, pub) < 0) {
		memset(priv, 0, sizeof priv);
		return sw(buf, SW_OTHER);
	}
	memset(priv, 0, sizeof priv);
	sha256_init(&s);
	sha256_update(&s, &z, 1);
	sha256_update(&s, app, 32);
	sha256_update(&s, chal, 32);
	sha256_update(&s, mac, 32);
	sha256_update(&s, nonce, 32);
	sha256_update(&s, pub, 65);
	sha256_final(&s, hash);
	if (u->ops->p256_sign(u->ops->ctx, u->cert_key, hash, sig) < 0)
		return sw(buf, SW_OTHER);
	u->present = 0;
	n = 0;
	buf[n++] = 0x05;
	memcpy(buf + n, pub, 65), n += 65;
	buf[n++] = 64;
	memcpy(buf + n, mac, 32), n += 32;
	memcpy(buf + n, nonce, 32), n += 32;
	memcpy(buf + n, u->cert, u->cert_len), n += u->cert_len;
	n += u2f_der_sig(buf + n, sig);
	n += sw(buf + n, SW_OK);
	return n;
}

/* AUTHENTICATE: data = challenge(32) app id(32) L(1) handle(L).  Answer:
 * the flags (1: present), the counter (big endian), the signature (DER),
 * 9000. */
static unsigned auth(struct u2f *u, unsigned char *buf, unsigned lc)
{
	unsigned char chal[32], app[32], priv[32], mac[32], hash[32], sig[64];
	unsigned char flags, ctr[4], mode = buf[2];
	unsigned long c;
	struct sha256 s;
	unsigned n;

	if (lc < 65 || buf[7 + 64] != 64 || lc != 65 + 64)
		return sw(buf, SW_DATA);
	event(u, U2F_EV_AUTH);
	memcpy(chal, buf + 7, 32);
	memcpy(app, buf + 7 + 32, 32);
	derive(u, app, buf + 7 + 65 + 32, priv, mac);
	if (memcmp(mac, buf + 7 + 65, 32) != 0) {	/* not ours */
		memset(priv, 0, sizeof priv);
		return sw(buf, SW_DATA);
	}
	if (mode == AUTH_CHECK) {
		memset(priv, 0, sizeof priv);
		return sw(buf, SW_PRESENCE);	/* "yes, it is mine" */
	}
	if (mode != AUTH_ENFORCE && mode != AUTH_DONT) {
		memset(priv, 0, sizeof priv);
		return sw(buf, SW_DATA);
	}
	flags = u->present ? 1 : 0;
	if (!flags && mode == AUTH_ENFORCE) {
		memset(priv, 0, sizeof priv);
		return sw(buf, SW_PRESENCE);
	}
	c = (u->counter + 1) & 0xFFFFFFFFUL;
	if (c == 0 || u->ops->counter_save(u->ops->ctx, c) < 0) {
		memset(priv, 0, sizeof priv);
		event(u, U2F_EV_ERROR);
		return sw(buf, SW_OTHER);
	}
	u->counter = c;
	ctr[0] = (unsigned char) (c >> 24);
	ctr[1] = (unsigned char) (c >> 16);
	ctr[2] = (unsigned char) (c >> 8);
	ctr[3] = (unsigned char) c;
	sha256_init(&s);
	sha256_update(&s, app, 32);
	sha256_update(&s, &flags, 1);
	sha256_update(&s, ctr, 4);
	sha256_update(&s, chal, 32);
	sha256_final(&s, hash);
	n = u->ops->p256_sign(u->ops->ctx, priv, hash, sig) < 0;
	memset(priv, 0, sizeof priv);
	if (n) return sw(buf, SW_OTHER);
	u->present = 0;
	n = 0;
	buf[n++] = flags;
	memcpy(buf + n, ctr, 4), n += 4;
	n += u2f_der_sig(buf + n, sig);
	n += sw(buf + n, SW_OK);
	event(u, U2F_EV_AUTH_OK);
	return n;
}

unsigned u2f_msg(struct u2f *u, unsigned char *buf, unsigned len,
		 unsigned size)
{
	unsigned lc = 0;

	if (len < 4) return sw(buf, SW_LENGTH);
	if (buf[0] != 0) return sw(buf, SW_CLA);
	if (len >= 7) {			/* extended length: 00 Lc1 Lc2 */
		lc = (unsigned) buf[5] << 8 | buf[6];
		if (buf[4] != 0 || 7 + lc > len) return sw(buf, SW_LENGTH);
	}
	switch (buf[1]) {
	case INS_REGISTER:
		if (lc != 64) return sw(buf, SW_LENGTH);
		return reg(u, buf, size);
	case INS_AUTH:
		if (lc < 65) return sw(buf, SW_LENGTH);
		return auth(u, buf, lc);
	case INS_VERSION:
		if (lc != 0) return sw(buf, SW_LENGTH);
		memcpy(buf, "U2F_V2", 6);
		return 6 + sw(buf + 6, SW_OK);
	}
	return sw(buf, SW_INS);
}

/* ---- U2FHID ---- */

void u2fhid_init(struct u2fhid *h, struct u2f *u)
{
	memset(h, 0, sizeof *h);
	h->u = u;
}

void u2fhid_reset(struct u2fhid *h)
{
	struct u2f *u = h->u;

	u2fhid_init(h, u);
	u->present = 0;
}

/* The answer (cmd, h->msg, len) on channel cid, in reports. */
static void reply(struct u2fhid *h, const unsigned char *cid, int cmd,
		  const unsigned char *p, unsigned len)
{
	unsigned char pkt[U2F_PKT];
	unsigned k, seq = 0;

	memset(pkt, 0, sizeof pkt);
	memcpy(pkt, cid, 4);
	pkt[4] = (unsigned char) cmd;
	pkt[5] = (unsigned char) (len >> 8);
	pkt[6] = (unsigned char) len;
	k = len < U2F_PKT - 7 ? len : U2F_PKT - 7;
	memcpy(pkt + 7, p, k);
	h->u->ops->send(h->u->ops->ctx, pkt);
	p += k, len -= k;
	while (len > 0 && seq < 128) {
		memset(pkt + 4, 0, U2F_PKT - 4);
		pkt[4] = (unsigned char) seq++;
		k = len < U2F_PKT - 5 ? len : U2F_PKT - 5;
		memcpy(pkt + 5, p, k);
		h->u->ops->send(h->u->ops->ctx, pkt);
		p += k, len -= k;
	}
}

static void error(struct u2fhid *h, const unsigned char *cid, int err)
{
	unsigned char e = (unsigned char) err;

	reply(h, cid, HID_ERROR, &e, 1);
}

/* A whole request in h: cmd, msg, len on channel cid. */
static void request(struct u2fhid *h, long now)
{
	unsigned char *m = h->msg, r[17];
	unsigned n;

	h->busy = 0;
	if (h->lock && memcmp(h->cid, h->lock_cid, 4) != 0
	    && (h->cmd == HID_MSG || h->cmd == HID_CHINIT)) {
		error(h, h->cid, U2FHID_ERR_BUSY);
		return;
	}
	switch (h->cmd) {
	case HID_PING:
		reply(h, h->cid, HID_PING, m, h->len);
		return;
	case HID_MSG:
		if (memcmp(h->cid, bcast, 4) == 0) break;
		n = u2f_msg(h->u, m, h->len, sizeof h->msg);
		reply(h, h->cid, HID_MSG, m, n);
		return;
	case HID_LOCK:
		if (h->len != 1 || m[0] > 10) {
			error(h, h->cid, U2FHID_ERR_PAR);
			return;
		}
		h->lock = m[0] != 0;
		memcpy(h->lock_cid, h->cid, 4);
		h->lock_until = now + 1000L * m[0];
		reply(h, h->cid, HID_LOCK, m, 0);
		return;
	case HID_CHINIT:
		if (h->len != 8) {
			error(h, h->cid, U2FHID_ERR_LEN);
			return;
		}
		memcpy(r, m, 8);		/* the host's nonce */
		do h->u->ops->random(h->u->ops->ctx, r + 8, 4);
		while (memcmp(r + 8, bcast, 4) == 0
		       || (r[8] | r[9] | r[10] | r[11]) == 0);
		r[12] = 2;			/* U2FHID protocol version */
		r[13] = 1, r[14] = 0, r[15] = 1;	/* device version */
		r[16] = 1;			/* capabilities: wink */
		reply(h, h->cid, HID_CHINIT, r, 17);
		return;
	case HID_WINK:
		if (h->len != 0) break;
		event(h->u, U2F_EV_WINK);
		reply(h, h->cid, HID_WINK, m, 0);
		return;
	}
	error(h, h->cid, U2FHID_ERR_CMD);
}

void u2fhid_packet(struct u2fhid *h, const unsigned char *pkt, unsigned n,
		   long now)
{
	unsigned k;

	if (h->lock && now - h->lock_until >= 0) h->lock = 0;
	if (n < 5 || n > U2F_PKT) return;
	if ((pkt[0] | pkt[1] | pkt[2] | pkt[3]) == 0) return;	/* cid 0 */
	if (pkt[4] & HID_INIT) {		/* an initial packet */
		if (n < 7) return;
		if (h->busy && memcmp(pkt, h->cid, 4) != 0) {
			error(h, pkt, U2FHID_ERR_BUSY);
			return;
		}
		if (pkt[4] != HID_CHINIT && memcmp(pkt, bcast, 4) == 0) {
			/* the broadcast channel is for INIT only */
			error(h, pkt, U2FHID_ERR_CMD);
			return;
		}
		memcpy(h->cid, pkt, 4);
		h->cmd = pkt[4];
		h->len = (unsigned short) (pkt[5] << 8 | pkt[6]);
		h->busy = 0;
		if (h->len > sizeof h->msg) {
			error(h, pkt, U2FHID_ERR_LEN);
			return;
		}
		k = n - 7 < h->len ? n - 7 : h->len;
		memcpy(h->msg, pkt + 7, k);
		h->have = (unsigned short) k;
		h->seq = 0;
	} else {				/* a continuation */
		if (!h->busy) return;		/* none expected: ignored */
		if (memcmp(pkt, h->cid, 4) != 0) {
			error(h, pkt, U2FHID_ERR_BUSY);
			return;
		}
		if (pkt[4] != h->seq) {
			h->busy = 0;
			error(h, pkt, U2FHID_ERR_SEQ);
			return;
		}
		h->seq++;
		k = n - 5 < (unsigned) (h->len - h->have) ? n - 5
			: (unsigned) (h->len - h->have);
		memcpy(h->msg + h->have, pkt + 5, k);
		h->have = (unsigned short) (h->have + k);
	}
	if (h->have < h->len) {
		h->busy = 1;
		return;
	}
	request(h, now);
}
