/* nfc_sha1.h - SHA-1 (FIPS 180-4) for the NFC parsers (#412).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #412.
 *
 * The tree had no SHA-1; disney_infinity derives its sector keys from
 * SHA-1(seed) (mbedtls_sha1 in stock, shim/mbedtls/sha1.h maps it here).
 * One call, all state on the caller's stack (~100 bytes), nothing
 * resident.  Not for new security uses: SHA-1 is broken for collisions.
 */
#ifndef _LIBNFC_NFC_SHA1_H
#define _LIBNFC_NFC_SHA1_H

#include <stddef.h>
#include <stdint.h>

void nfc_sha1(const uint8_t* data, size_t len, uint8_t digest[20]);

#endif
