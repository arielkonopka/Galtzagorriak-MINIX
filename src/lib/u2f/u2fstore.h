/* SPDX-License-Identifier: GPL-3.0-or-later
 * u2fstore - the U2F app's files (issue #420), in a folder (on MINIX
 * /home/flipper/u2f, stock's /ext/u2f):
 *
 *  assets/cert.der	the attestation certificate (DER, at most U2F_MAXCERT
 *			B), the user's own: never shipped
 *  assets/cert_key.u2f	its private key, stock's format with "Type: 2" (not
 *			encrypted):  Filetype: Flipper U2F Certificate Key
 *			File / Version: 1 / Type: 2 / Data: 32 hex bytes.
 *			Types 0 and 1 are encrypted with a key inside the
 *			stock Flipper's radio core: refused (U2FS_ENCRYPTED)
 *  minix.key		the device key, 32 random bytes, made at the first
 *			start (Filetype: Galtzagorriak U2F Device Key File)
 *  minix.cnt		the counter (Filetype: Galtzagorriak U2F Counter File)
 *
 * Stock's own key.u2f and cnt.u2f are encrypted the same way, so they are
 * left alone: a card moved back to stock keeps its stock keys.  Every file
 * is written as name.tmp, mode 0600, then renamed over the old one (a
 * power cut leaves the old file or the new one).  The counter never goes
 * back: a counter file that exists but cannot be read is an error, never
 * a new start at 0.  Nothing is written to internal flash.
 */
#ifndef U2F_STORE_H
#define U2F_STORE_H

#define U2FS_OK		0
#define U2FS_NOCERT	(-1)		/* cert.der missing or bad */
#define U2FS_NOKEY	(-2)		/* cert_key.u2f missing or bad */
#define U2FS_ENCRYPTED	(-3)		/* cert_key.u2f of Type 0 or 1 */
#define U2FS_DEVKEY	(-4)		/* minix.key unreadable or unwritable */
#define U2FS_COUNTER	(-5)		/* minix.cnt unreadable */
#define U2FS_WRITE	(-6)		/* a file could not be written */

struct u2f;

/* Everything into u (cert into the caller's buffer of U2F_MAXCERT B);
 * a missing device key is made with rnd and saved, a missing counter is
 * 0.  U2FS_OK or the first error. */
int u2fs_load(const char *dir, struct u2f *u, unsigned char *cert,
	      void (*rnd)(unsigned char *b, unsigned n));
int u2fs_counter_save(const char *dir, unsigned long counter);
const char *u2fs_error(int e);

/* The parser, for the tests: the value of "key:" in text, hex bytes into
 * out (exactly n), or a number; 0 or -1. */
int u2fs_hex(const char *text, const char *key, unsigned char *out,
	     unsigned n);
int u2fs_num(const char *text, const char *key, unsigned long *v);

#endif /* U2F_STORE_H */
