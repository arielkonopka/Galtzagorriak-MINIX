/* SPDX-License-Identifier: GPL-3.0-or-later
 * libu2f - the stock U2F app's logic on Galtzagorriak MINIX (issue #420):
 * the U2F raw messages (REGISTER, AUTHENTICATE, VERSION) and the U2FHID
 * framing (64-byte reports: INIT, PING, MSG, LOCK, WINK, ERROR), as plain
 * C with no I/O.  The caller gives the randomness, the P-256 operations,
 * the counter's storage and the 64-byte report output as hooks, so the
 * same code runs in the host tests and, later, on the Flipper.
 *
 * Source: Unleashed applications/main/u2f (u2f.c, u2f_hid.c), GPLv3.  The
 * message formats, the key derivation and the wire bytes are stock's, so
 * a key registered with stock's logic and the same device key
 * authenticates here.  Changes from stock (bounds and spec fixes):
 *  - every request length is checked (stock reads the fixed offsets of a
 *    full request whatever arrived): a short REGISTER or AUTHENTICATE is
 *    0x6700, an AUTHENTICATE whose key handle is not 64 B is 0x6A80;
 *  - a CLA other than 0 is 0x6E00, an unknown INS 0x6D00 (as stock);
 *  - a wrong key handle is 0x6A80 also when the user is not present
 *    (stock answered 0x6985 first), as the U2F spec says;
 *  - the counter is saved before the signature leaves; when it cannot be
 *    saved the answer is 0x6F00 and nothing is signed (a counter value is
 *    never used twice);
 *  - the DER encoder keeps one byte of an all-zero integer;
 *  - the message buffer is U2F_MAXMSG (stock: 7.4 KB); longer requests
 *    are U2FHID ERR_INVALID_LEN, a certificate over U2F_MAXCERT is refused
 *    at load time;
 *  - U2FHID: a continuation with the wrong sequence is ERR_INVALID_SEQ, a
 *    packet of another channel while one is being assembled ERR_CHANNEL_
 *    BUSY; the channel lock times out by the caller's clock (no timer).
 *
 * Key storage rules (the app, next part; summary of docs/manual/u2f.md):
 * the attestation certificate and its key are the user's own files
 * (/ext/u2f/assets/cert.der, cert_key.u2f): never shipped.  The device
 * key (key.u2f, 32 random bytes from /dev/random at the first start) and
 * the counter (cnt.u2f) are files of the user u2f, mode 0600, on the SD
 * card (never internal flash); the counter is written with a new file and
 * rename(), and an unreadable counter stops the app instead of starting
 * again at 0.
 */
#ifndef U2F_H
#define U2F_H

#define U2F_PKT		64		/* U2FHID report */
#define U2F_MAXCERT	768		/* attestation certificate, DER */
#define U2F_MAXMSG	(1 + 65 + 1 + 64 + U2F_MAXCERT + 72 + 2)

/* Events for the app's screen (u2f_ops.event) */
#define U2F_EV_REGISTER	1		/* a site wants to register: press OK */
#define U2F_EV_AUTH	2		/* a site wants a login: press OK */
#define U2F_EV_AUTH_OK	3		/* signed */
#define U2F_EV_WINK	4		/* the host asks for a sign of life */
#define U2F_EV_ERROR	5

struct u2f_ops {
	void *ctx;
	void (*random)(void *ctx, unsigned char *b, unsigned n);
	/* P-256: the public key (04 X Y) of priv; 0, or -1 when priv is not
	 * a valid key */
	int (*p256_pub)(void *ctx, const unsigned char priv[32],
			unsigned char pub[65]);
	/* P-256 ECDSA of the 32-byte hash: r and s, 32 B each; 0 or -1 */
	int (*p256_sign)(void *ctx, const unsigned char priv[32],
			 const unsigned char hash[32], unsigned char sig[64]);
	/* store the counter for good before it is used; 0 or -1 */
	int (*counter_save)(void *ctx, unsigned long counter);
	void (*event)(void *ctx, int ev);		/* may be NULL */
	/* one 64-byte U2FHID report to the host */
	void (*send)(void *ctx, const unsigned char pkt[U2F_PKT]);
};

struct u2f {
	const struct u2f_ops *ops;
	unsigned char device_key[32];	/* key.u2f */
	unsigned char cert_key[32];	/* the attestation key */
	const unsigned char *cert;	/* DER, at most U2F_MAXCERT */
	unsigned cert_len;
	unsigned long counter;		/* the last value used */
	unsigned char present;		/* the user pressed OK */
};

/* U2F raw message in buf (len bytes, buf has size bytes): the answer is
 * written over it; its length (always at least 2, the status word). */
unsigned u2f_msg(struct u2f *u, unsigned char *buf, unsigned len,
		 unsigned size);
void u2f_present(struct u2f *u);	/* OK pressed: the next one may sign */
/* DER of an ECDSA signature (r, s) into der (72 B): its length. */
unsigned u2f_der_sig(unsigned char *der, const unsigned char sig[64]);

/* U2FHID: one per USB interface (~1.2 KB with the message buffer). */
struct u2fhid {
	struct u2f *u;
	unsigned char cid[4];		/* the channel being assembled */
	unsigned char lock_cid[4];
	unsigned char cmd, seq, busy, lock;
	unsigned short len, have;	/* message length, bytes so far */
	long lock_until;		/* ms, the caller's clock */
	unsigned char msg[U2F_MAXMSG];
};

/* U2FHID errors */
#define U2FHID_ERR_CMD	0x01
#define U2FHID_ERR_PAR	0x02
#define U2FHID_ERR_LEN	0x03
#define U2FHID_ERR_SEQ	0x04
#define U2FHID_ERR_BUSY	0x06

void u2fhid_init(struct u2fhid *h, struct u2f *u);
/* A report from the host (n bytes, at most 64), now_ms: the caller's
 * millisecond clock.  Answers go out through u->ops->send. */
void u2fhid_packet(struct u2fhid *h, const unsigned char *pkt, unsigned n,
		   long now_ms);
/* The host went away (USB unplugged): no channel, no lock, no presence. */
void u2fhid_reset(struct u2fhid *h);

#endif /* U2F_H */
