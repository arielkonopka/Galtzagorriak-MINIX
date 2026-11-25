/* pgpenc - encrypt a file to an OpenPGP RSA public key (RFC 4880: a v3
 * public-key encrypted session key packet and a symmetrically encrypted
 * integrity protected data packet, AES-256, MDC).  The recipient opens it
 * on a PC with `gpg -d`, with the secret key (for the user's YubiKey: the
 * key on the card and its PIN).  Nothing secret is on the Flipper: only the
 * public key.  Pure C, no malloc, no I/O but through the callbacks; runs
 * on the PC (tests/pgpenc: gpg decrypts what it writes) and in the
 * `pgpenc` command.
 *
 * RAM: struct pgp_ctx is about 1.5 KB (two 512 B chunk buffers, AES and
 * SHA-1 state) plus the key (n, up to 4096 bits: 512 B) on the caller's
 * stack; the whole file never sits in memory.
 *
 * Keys: RSA 1024..4096 bits, e up to 32 bits.  ECC keys (Curve25519, NIST)
 * are not supported yet.
 */
#ifndef PGPENC_H
#define PGPENC_H

#include <stddef.h>
#include <stdint.h>

#define PGP_NMAX	512		/* bytes of n (4096 bits) */

struct pgp_pub {
	uint8_t n[PGP_NMAX];
	uint16_t nlen;			/* bytes, no leading zeros */
	uint32_t e;
	uint8_t keyid[8];		/* the last 8 bytes of the fingerprint */
	uint8_t fpr[20];		/* v4 fingerprint (SHA-1) */
};

/* The encryption key out of an OpenPGP public key block: binary packets
 * (what `gpg --export` writes) or ASCII armor.  The last RSA public
 * subkey is taken (the encryption subkey of a usual key); with none, the
 * primary key.  Armor is decoded in place: buf is changed.  0, or -1: not a key / not RSA / too big. */
int pgp_key_parse(uint8_t *buf, size_t len, struct pgp_pub *k);

/* Armor: the bytes of a binary key (or message) out of armor text; out may
 * be the same buffer as in (it never overtakes the input): the length, or -1.  (pgp_key_parse() calls
 * this itself when the block starts with "-----BEGIN".) */
int pgp_dearmor(const uint8_t *in, size_t len, uint8_t *out, size_t max);

struct pgp_io {
	void *arg;
	/* the source: up to n bytes into buf; the count, 0 at the end, -1 on error */
	int (*read)(void *arg, uint8_t *buf, size_t n);
	/* the sink: all n bytes, 0 or -1 */
	int (*write)(void *arg, const uint8_t *buf, size_t n);
	/* n random bytes (session key, padding, the CFB prefix); 0 or -1 */
	int (*rng)(void *arg, uint8_t *buf, size_t n);
};

/* Encrypt the source to the key, as a binary OpenPGP message in the sink.
 * name: the file name stored in the literal packet (NULL: none), mtime its
 * time (seconds since 1970, 0: none).  0, or -1 (the callbacks failed). */
int pgp_encrypt(const struct pgp_pub *k, const struct pgp_io *io,
		const char *name, uint32_t mtime);

/* The same as ASCII armor ("-----BEGIN PGP MESSAGE-----"), the 64-character
 * lines; the sink gets text. */
int pgp_encrypt_armor(const struct pgp_pub *k, const struct pgp_io *io,
		      const char *name, uint32_t mtime);

#endif
