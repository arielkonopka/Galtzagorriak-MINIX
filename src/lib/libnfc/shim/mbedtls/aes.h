/* mbedtls/aes.h - the AES calls of the vendored Ultralight AES code (#412).
 * No mbedTLS in the tree yet: nfc_shim_crypto.c fails these closed (an
 * error, the output zeroed), so UL-AES authentication fails as with a
 * wrong key instead of computing something wrong.  SPDX: GPL-3.0-or-later */
#ifndef NFCSHIM_MBEDTLS_AES_H
#define NFCSHIM_MBEDTLS_AES_H
#include <stddef.h>
#include <stdint.h>
#define MBEDTLS_AES_ENCRYPT 1
#define MBEDTLS_AES_DECRYPT 0
typedef struct { int unused; } mbedtls_aes_context;
void mbedtls_aes_init(mbedtls_aes_context* ctx);
void mbedtls_aes_free(mbedtls_aes_context* ctx);
int mbedtls_aes_setkey_enc(mbedtls_aes_context* ctx, const unsigned char* key, unsigned int bits);
int mbedtls_aes_setkey_dec(mbedtls_aes_context* ctx, const unsigned char* key, unsigned int bits);
int mbedtls_aes_crypt_ecb(mbedtls_aes_context* ctx, int mode, const unsigned char in[16], unsigned char out[16]);
int mbedtls_aes_crypt_cbc(mbedtls_aes_context* ctx, int mode, size_t len, unsigned char iv[16],
                          const unsigned char* in, unsigned char* out);
#endif
