/* mbedtls/sha1.h - the one-shot SHA-1 disney_infinity calls, on
 * libnfc's own nfc_sha1.c (#412).  SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NFCSHIM_MBEDTLS_SHA1_H
#define NFCSHIM_MBEDTLS_SHA1_H
#include "nfc_sha1.h"
static inline int mbedtls_sha1(const unsigned char* in, size_t n, unsigned char out[20]) {
    nfc_sha1(in, n, out);
    return 0;
}
#endif
