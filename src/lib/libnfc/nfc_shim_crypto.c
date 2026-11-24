/* nfc_shim_crypto.c - the one Mbed TLS function libnfc provides itself.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issues #412, #447.
 *
 * AES and DES/3DES are the vendored Mbed TLS v2.28.9 ciphers
 * (src/lib/mbedtls/library/aes.c, des.c, built with nfc_mbedtls_config.h).
 * They wipe their contexts with mbedtls_platform_zeroize, which upstream
 * keeps in platform_util.c behind a volatile function pointer in .data.
 * Here it is a volatile byte loop: the compiler cannot drop the stores,
 * and the images keep 0 B of .data/.bss for the crypto (spec Phase 14).
 */
#include <stddef.h>

void mbedtls_platform_zeroize(void* buf, size_t len);

void mbedtls_platform_zeroize(void* buf, size_t len) {
    volatile unsigned char* p = (volatile unsigned char*)buf;
    while(len--) *p++ = 0;
}
