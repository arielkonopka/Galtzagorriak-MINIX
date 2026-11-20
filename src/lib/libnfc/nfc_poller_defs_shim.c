/* nfc_poller_defs_shim.c - the poller table of the protocols libnfc builds
 * from the vendored stacks (#404).  Stock's nfc_poller_defs.c lists every
 * protocol; this one lists only those compiled here, the rest stay NULL
 * (nfc_poller_alloc on one of them is a furi_check failure, not a jump to
 * nowhere).  One line per protocol as each one is hooked up.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <nfc/protocols/nfc_poller_defs.h>

#include <nfc/protocols/iso14443_3a/iso14443_3a_poller_defs.h>
#include <nfc/protocols/iso14443_4a/iso14443_4a_poller_defs.h>

const NfcPollerBase* const nfc_pollers_api[NfcProtocolNum] = {
    [NfcProtocolIso14443_3a] = &nfc_poller_iso14443_3a,
    [NfcProtocolIso14443_4a] = &nfc_poller_iso14443_4a,
};
