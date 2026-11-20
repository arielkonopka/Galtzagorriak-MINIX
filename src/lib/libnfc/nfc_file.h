/* nfc_file.h - the stock `.nfc` file format, reader and writer.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14 (AC14.1), issue #387.
 *
 * Stock saves cards as a Flipper NFC device file: a FlipperFormat text
 * file ("Key: values", "#" comments).  This reads it a line at a time
 * (never the whole file resident) and writes the current version, so a
 * dump stock saved loads here and the reverse (AC14.1).  Versions 1-4 of
 * the header are tolerated on read; the writer emits version 4.  MIFARE
 * Classic and Ultralight/NTAG are modelled; other device types still load
 * their common header (type, UID, ATQA, SAK).
 */
#ifndef _LIBNFC_NFC_FILE_H
#define _LIBNFC_NFC_FILE_H

#include <stdio.h>
#include "mf_classic.h"

#define NFC_FILE_VERSION	4	/* the version the writer emits */

typedef enum {
	NFC_DEV_UNKNOWN = 0,
	NFC_DEV_MF_CLASSIC,
	NFC_DEV_MF_ULTRALIGHT,	/* Ultralight / NTAG */
	NFC_DEV_ISO14443_3A,	/* a bare 14443-3A tag (UID only) */
} nfc_dev_type_t;

#define NFC_UL_MAX_PAGES	256	/* NTAG I2C plus; NTAG215 is 135 */

typedef struct nfc_device {
	int version;			/* file version read (0 if written) */
	nfc_dev_type_t type;
	uint8_t uid[10];
	uint8_t uid_len;
	uint8_t atqa[2];
	uint8_t sak;

	mf_classic_data_t mfc;		/* NFC_DEV_MF_CLASSIC */

	uint8_t ul_page[NFC_UL_MAX_PAGES][4];	/* NFC_DEV_MF_ULTRALIGHT */
	uint16_t ul_pages;
	char ul_subtype[24];		/* "NTAG215", "MF0ICU1", ... */
} nfc_device_t;

/* Load from a path or an open stream.  Return 0 on success, -1 (errno set
 * or 0 for a format error) on failure; *err (if not NULL) gets a short
 * reason.  The stream form lets a test read a fixture without a temp file. */
int nfc_file_load(nfc_device_t *d, const char *path, const char **err);
int nfc_file_read(nfc_device_t *d, FILE *fp, const char **err);

/* Save to a path or stream in version-4 format.  Return 0 / -1. */
int nfc_file_save(const nfc_device_t *d, const char *path);
int nfc_file_write(const nfc_device_t *d, FILE *fp);

#endif /* _LIBNFC_NFC_FILE_H */
