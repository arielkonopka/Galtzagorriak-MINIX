/* t2t_pwd.h - passwords for NTAG / MIFARE Ultralight copies (the NFC
 * password copier, issue #110 queue; user request 2026-11-24).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX.
 *
 * The rule of this port: a password is in the dictionary BEFORE it is
 * written to a card, so a card that was written and then lost (power cut,
 * card pulled) can always be opened again.  t2t_pwd_choose() therefore
 * returns a password only after it is stored (fclose'd) in the user's
 * dictionary, and the last choice is kept in a small file, not in RAM:
 * nothing is held between runs.  Pure stdio, no malloc; the card side
 * (PWD_AUTH, writing PWD/PACK/AUTH0) is the device lane's.
 *
 * Modes (Settings, user request): own password (a fixed one the user
 * typed), the password of the previous copier run, a chosen dictionary
 * entry, or (the default) a new random one; "legacy" keeps the stock
 * behaviour: the copy gets no new password (T2T_LEGACY, nothing stored).
 */
#ifndef _LIBNFC_T2T_PWD_H
#define _LIBNFC_T2T_PWD_H

#include <stdio.h>

#define T2T_DICT_USER	"/home/flipper/nfc/assets/t2t_pwd_user.nfc"
#define T2T_LAST	"/home/flipper/nfc/assets/t2t_pwd_last.nfc"

enum t2t_mode {
	T2T_RANDOM,	/* a new random password (default) */
	T2T_OWN,	/* cfg.own */
	T2T_PREVIOUS,	/* the one in the last file */
	T2T_DICT,	/* line cfg.index of the user dictionary (0 based) */
	T2T_LEGACY	/* none: stock behaviour */
};

struct t2t_cfg {
	unsigned char mode;
	unsigned char own[4];
	unsigned long index;
	const char *dict;	/* NULL: T2T_DICT_USER */
	const char *last;	/* NULL: T2T_LAST */
	/* random bytes: fills n bytes, returns 0 (the caller's RNG) */
	int (*rng)(unsigned char *buf, int n);
	const char *note;	/* appended as a comment to a new line */
};

/* PACK is the 2-byte answer to PWD_AUTH; this port makes it from the
 * password and the UID's first byte, so two copies differ (any 2 bytes
 * are valid). */
#define T2T_OK		0
#define T2T_NONE	1	/* T2T_LEGACY: no password */
#define T2T_ERR		-1	/* the file could not be written or read */
#define T2T_BADCFG	-2	/* no RNG, bad index, no previous, weak own */

/* Fills pwd[4] and pack[2].  Weak passwords (all bytes equal, the stock
 * default FFFFFFFF, 00000000) are refused for T2T_OWN and never made. */
int t2t_pwd_choose(const struct t2t_cfg *cfg, const unsigned char uid[7],
		   unsigned char pwd[4], unsigned char pack[2]);

/* Is the password in the dictionary?  1 yes, 0 no, -1 no file/error. */
int t2t_pwd_known(const char *dict, const unsigned char pwd[4]);

/* One line, "XXXXXXXX  # note", unless it is there already. */
int t2t_pwd_add(const char *dict, const unsigned char pwd[4],
		const char *note);

int t2t_pwd_weak(const unsigned char pwd[4]);

#endif
