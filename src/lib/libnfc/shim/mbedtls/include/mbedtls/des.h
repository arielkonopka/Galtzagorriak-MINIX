/* mbedtls/des.h - the 3DES calls of the vendored Ultralight C code (#412),
 * failed closed by nfc_shim_crypto.c (see mbedtls/aes.h).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NFCSHIM_MBEDTLS_DES_H
#define NFCSHIM_MBEDTLS_DES_H
#include <stddef.h>
#include <stdint.h>
#define MBEDTLS_DES_ENCRYPT 1
#define MBEDTLS_DES_DECRYPT 0
typedef struct { int unused; } mbedtls_des3_context;
void mbedtls_des3_init(mbedtls_des3_context* ctx);
void mbedtls_des3_free(mbedtls_des3_context* ctx);
int mbedtls_des3_set2key_enc(mbedtls_des3_context* ctx, const unsigned char key[16]);
int mbedtls_des3_set2key_dec(mbedtls_des3_context* ctx, const unsigned char key[16]);
int mbedtls_des3_crypt_cbc(mbedtls_des3_context* ctx, int mode, size_t len, unsigned char iv[8],
                           const unsigned char* in, unsigned char* out);
#endif
