/* nfc_poller.h - the NFC-A poller: activate a card, identify it and read
 * it into an nfc_device, ready to save as a `.nfc` file.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #387.
 *
 * This is the top of the reader side: it runs ISO 14443-3A activation
 * (iso14443_3a), decides from the SAK which protocol the card speaks and
 * reads its memory with that protocol (MIFARE Classic with the dictionary
 * attack, Ultralight/NTAG by page), filling the nfc_device model the file
 * writer saves.  Nothing stays resident: the nfc_device and the found-keys
 * table are the caller's, allocated per read and freed after (R14 memory
 * rule).  The field is the caller's too (NFCIOCFIELD before, 0 after).
 */
#ifndef _LIBNFC_NFC_POLLER_H
#define _LIBNFC_NFC_POLLER_H

#include "nfc_file.h"		/* nfc_device_t */
#include "mf_attack.h"		/* mf_found_t */

/* Activate one card and fill d->type/uid/atqa/sak (no memory read yet).
 * Returns NFC_OK, or an nfc_err if no card answers. */
int nfc_poll_identify(const nfc_xfer_t *x, nfc_device_t *d);

/* Read an identified MIFARE Classic into d->mfc using the keys in `f`
 * (run mf_attack_dict first to fill `f`).  Every block whose sector key is
 * known is read; d->mfc.block_read marks which.  Returns the number of
 * blocks read. */
int nfc_poll_read_mfc(const nfc_xfer_t *x, nfc_device_t *d, const mf_found_t *f);

/* Read an identified Ultralight/NTAG into d->ul_page/ul_pages.  Uses
 * GET_VERSION to size the card, then reads every page.  Returns NFC_OK if
 * at least one page was read. */
int nfc_poll_read_ul(const nfc_xfer_t *x, nfc_device_t *d);

#endif /* _LIBNFC_NFC_POLLER_H */
