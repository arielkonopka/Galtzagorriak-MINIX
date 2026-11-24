/* aid_scan.h - the read-only service scanner: which applications (AIDs)
 * does an ISO 14443-4 card answer to?
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #451 (NFC part 7).
 *
 * The scanner sends one ISO 7816-4 SELECT by DF name (00 A4 04 00 Lc AID
 * 00) per AID of a dictionary file and notes the AIDs the card accepts.
 * Nothing else goes to the card: no READ, no authentication, no PIN, no
 * GET PROCESSING OPTIONS.  SELECT changes nothing on a card; it only
 * picks the application the next command would go to.
 *
 * The dictionary is a text file read a line at a time (never whole, R14):
 *
 *	# a comment
 *	[Payments] Bank cards: the app a shop terminal talks to
 *	A0000000031010 Visa credit/debit
 *	A0000000041010 Mastercard
 *
 * "[Group] description" opens a group; the description is the "what is
 * this?" line the app shows.  An AID line is 5 to 16 bytes in hex (no
 * spaces inside), then blanks, then the service's name.  Bad lines are
 * skipped and counted.  The example file is flipper/etc/nfc/aid_scan.txt,
 * installed as /home/flipper/nfc/assets/aid_scan.nfc.
 *
 * Memory: the scan state (struct aid_scan, ~1.6 KB with the ISO-DEP block
 * buffers) is the caller's, malloc'd for one scan and freed after.
 */
#ifndef _LIBNFC_AID_SCAN_H
#define _LIBNFC_AID_SCAN_H

#include <stdint.h>
#include <stdio.h>
#include "iso_dep.h"

#define AID_SCAN_DICT	"/home/flipper/nfc/assets/aid_scan.nfc"
#define AID_MAX		16		/* ISO 7816-4: 5..16 bytes */
#define AID_MIN		5
#define AID_GROUP_MAX	20		/* "Payments", "Transport" ... */
#define AID_DESC_MAX	64
#define AID_NAME_MAX	28
#define AID_HITS_MAX	16		/* matches kept (the rest counted) */

/* One dictionary entry, as the streamer hands it out. */
typedef struct aid_entry {
	uint8_t aid[AID_MAX];
	uint8_t aid_len;
	char name[AID_NAME_MAX];
	char group[AID_GROUP_MAX];	/* the current [Group] */
} aid_entry_t;

/* The streamer: one open file and the group being read. */
typedef struct aid_dict {
	FILE *fp;
	unsigned long line;
	unsigned bad;			/* malformed lines skipped */
	char group[AID_GROUP_MAX];
} aid_dict_t;

int aid_dict_open(aid_dict_t *d, const char *path);	/* 0 or -1 */
void aid_dict_close(aid_dict_t *d);
/* The next AID line: 1 and *e filled, 0 at the end of the file. */
int aid_dict_next(aid_dict_t *d, aid_entry_t *e);
/* The description of [group] in the file at path, into out (n bytes);
 * 0, or -1 if the file or the group is not there. */
int aid_dict_group_desc(const char *path, const char *group,
			char *out, size_t n);

/* One match: the service the card answered to. */
typedef struct aid_hit {
	aid_entry_t e;
	uint16_t sw;			/* 9000, 61xx, 6283 (blocked) ... */
} aid_hit_t;

typedef struct aid_scan {
	uint8_t atqa[2], sak, uid[10], uid_len;
	uint8_t ats[ISO_DEP_ATS_MAX], ats_len;
	unsigned tried, nhits, bad_lines, lost;
	aid_hit_t hit[AID_HITS_MAX];
	iso_dep_t dep;
	uint8_t resp[64];		/* the FCI is not kept: SW only */
} aid_scan_t;

/* The status words that mean "this application is here". */
int aid_sw_present(uint16_t sw);

/* Activate one card (3A + RATS) and SELECT every AID of the dictionary
 * at path.  Fills *s.  The field is the caller's (on before, off after).
 * progress, if not NULL, is called after each AID (the app's busy line;
 * returning non-zero stops the scan).
 * Returns NFC_OK; NFC_ERR_NOTSUP if the card is not ISO 14443-4 (SAK bit
 * 0x20 clear; s still holds UID/ATQA/SAK); -100 if the dictionary cannot
 * be opened; or the activation's nfc_err. */
#define AID_SCAN_NODICT	(-100)
int aid_scan(const nfc_xfer_t *x, const char *path, aid_scan_t *s,
	     int (*progress)(void *ctx, unsigned tried), void *ctx);

/* An AID in hex into out (2 * len + 1 bytes). */
void aid_hex(char *out, const uint8_t *p, unsigned len);

#endif /* _LIBNFC_AID_SCAN_H */
