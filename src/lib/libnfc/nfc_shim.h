/* nfc_shim.h - the vendored Unleashed NFC pollers over nfc_xfer (#404).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #404 (follow-up of #387/#402).
 *
 * Unleashed's protocol stacks (src/lib/nfc, vendored byte for byte) run on
 * an asynchronous `Nfc` worker thread that calls the poller chain until it
 * says stop.  This shim gives them the same `Nfc` API (<nfc/nfc.h>) over
 * the one transceiver of libnfc, nfc_xfer_t, and runs that loop in the
 * caller: nfc_start() returns when the chain answers NfcCommandStop (or
 * after max_steps PollerReady events, so a missing card cannot hang the
 * caller).  No threads, no RTOS objects; the pollers are allocated per
 * poll by the vendored nfc_poller.c and freed after.
 *
 * Frames go to nfc_xfer as stock's HAL sends them: the pollers add and
 * check CRCs themselves, so the flags are 0 (hardware parity) except
 *   - REQA/WUPA: NFC_XF_SHORT, 7 bits;
 *   - custom parity (MIFARE Classic Crypto-1): NFC_XF_PARITY, the stream
 *     is 9 bits per byte (data LSB first, then its parity bit), the layout
 *     of bit_buffer_write_bytes_with_parity() and of the ST25R3916's
 *     transparent mode.
 */
#ifndef _LIBNFC_NFC_SHIM_H
#define _LIBNFC_NFC_SHIM_H

#include "nfc_xfer.h"

struct Nfc;

/* An Nfc over x (kept by pointer; x must outlive it). */
struct Nfc* nfc_alloc_xfer(const nfc_xfer_t* x);
/* Cap on PollerReady events per nfc_start (default 64). */
void nfc_shim_set_max_steps(struct Nfc* nfc, unsigned max_steps);
/* Transceive calls made since nfc_alloc_xfer (for tests). */
unsigned nfc_shim_frames(const struct Nfc* nfc);

#endif
