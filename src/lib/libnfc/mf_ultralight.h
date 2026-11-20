/* mf_ultralight.h - MIFARE Ultralight / NTAG page reader.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #387.
 *
 * The reader side of MIFARE Ultralight and NTAG (stock
 * lib/nfc/protocols/mf_ultralight), over one nfc_xfer_t.  These are 4-byte-
 * page memory cards: READ (0x30, page) returns 4 pages (16 bytes) wrapping
 * at the end; FAST_READ and GET_VERSION identify the exact chip.  No buffer
 * stays resident: pages are read into the caller's array.
 */
#ifndef _LIBNFC_MF_ULTRALIGHT_H
#define _LIBNFC_MF_ULTRALIGHT_H

#include <stdint.h>
#include <stddef.h>
#include "nfc_xfer.h"

#define MFUL_PAGE_SIZE		4
#define MFUL_MAX_PAGES		256	/* NTAG I2C plus 2k is the largest */

#define MFUL_CMD_READ		0x30
#define MFUL_CMD_GET_VERSION	0x60

/* Known subtypes: the page count and a name for the file. */
typedef struct mful_type {
	const char *name;	/* "NTAG215", "MF0ICU1", ... */
	uint16_t total_pages;	/* user + config pages */
} mful_type_t;

/* GET_VERSION (0x60) -> 8-byte version, mapped to a subtype.  Returns a
 * pointer to a static descriptor, or NULL if the card does not answer
 * GET_VERSION (an original Ultralight): the caller then probes by reading. */
const mful_type_t *mful_get_version(const nfc_xfer_t *x);

/* Map an 8-byte GET_VERSION answer to a subtype (exposed for tests). */
const mful_type_t *mful_type_from_version(const uint8_t ver[8]);

/* Read one 4-byte page.  `out` gets 4 bytes.  Returns NFC_OK or an error. */
int mful_read_page(const nfc_xfer_t *x, uint8_t page, uint8_t out[MFUL_PAGE_SIZE]);

/* Read `pages` pages into page[i][0..3].  Stops and returns the count read
 * on the first page that does not answer; *pages_out gets how many were
 * read.  READ returns 4 pages at a time, so this issues one command per 4
 * pages.  Returns NFC_OK if at least one page was read. */
int mful_read_card(const nfc_xfer_t *x, uint16_t pages,
		   uint8_t page[][MFUL_PAGE_SIZE], uint16_t *pages_out);

#endif /* _LIBNFC_MF_ULTRALIGHT_H */
