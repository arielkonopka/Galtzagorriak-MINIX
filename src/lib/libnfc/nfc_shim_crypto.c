/* nfc_shim_crypto.c - mbedTLS AES/3DES for the vendored Ultralight poller,
 * failed closed (#412).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #412.
 *
 * The tree has no AES or DES and mbedTLS (Apache-2.0) is not vendored
 * (src/lib/nfc/README).  Every call returns an error and zeroes its
 * output, so Ultralight C (3DES) and Ultralight AES authentication fail
 * the way a wrong key does: the poller reads the open pages and marks the
 * rest unread.  Plain Ultralight / NTAG (the password is PWD_AUTH, no
 * cipher) needs none of this.  A real cipher replaces this file only.
 */
#include <string.h>
#include <mbedtls/aes.h>
#include <mbedtls/include/mbedtls/des.h>

#define NFCSHIM_CRYPTO_ERR (-0x0021) /* MBEDTLS_ERR_AES_INVALID_INPUT_LENGTH */

void mbedtls_aes_init(mbedtls_aes_context* ctx) { memset(ctx, 0, sizeof *ctx); }
void mbedtls_aes_free(mbedtls_aes_context* ctx) { memset(ctx, 0, sizeof *ctx); }
int mbedtls_aes_setkey_enc(mbedtls_aes_context* ctx, const unsigned char* key, unsigned int bits) {
    (void)ctx; (void)key; (void)bits;
    return NFCSHIM_CRYPTO_ERR;
}
int mbedtls_aes_setkey_dec(mbedtls_aes_context* ctx, const unsigned char* key, unsigned int bits) {
    (void)ctx; (void)key; (void)bits;
    return NFCSHIM_CRYPTO_ERR;
}
int mbedtls_aes_crypt_ecb(mbedtls_aes_context* ctx, int mode, const unsigned char in[16], unsigned char out[16]) {
    (void)ctx; (void)mode; (void)in;
    memset(out, 0, 16);
    return NFCSHIM_CRYPTO_ERR;
}
int mbedtls_aes_crypt_cbc(mbedtls_aes_context* ctx, int mode, size_t len, unsigned char iv[16],
                          const unsigned char* in, unsigned char* out) {
    (void)ctx; (void)mode; (void)iv; (void)in;
    memset(out, 0, len);
    return NFCSHIM_CRYPTO_ERR;
}

void mbedtls_des3_init(mbedtls_des3_context* ctx) { memset(ctx, 0, sizeof *ctx); }
void mbedtls_des3_free(mbedtls_des3_context* ctx) { memset(ctx, 0, sizeof *ctx); }
int mbedtls_des3_set2key_enc(mbedtls_des3_context* ctx, const unsigned char key[16]) {
    (void)ctx; (void)key;
    return NFCSHIM_CRYPTO_ERR;
}
int mbedtls_des3_set2key_dec(mbedtls_des3_context* ctx, const unsigned char key[16]) {
    (void)ctx; (void)key;
    return NFCSHIM_CRYPTO_ERR;
}
int mbedtls_des3_crypt_cbc(mbedtls_des3_context* ctx, int mode, size_t len, unsigned char iv[8],
                           const unsigned char* in, unsigned char* out) {
    (void)ctx; (void)mode; (void)iv; (void)in;
    memset(out, 0, len);
    return NFCSHIM_CRYPTO_ERR;
}
