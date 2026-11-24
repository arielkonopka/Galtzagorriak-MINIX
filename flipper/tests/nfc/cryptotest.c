/* cryptotest.c - known-answer tests for the vendored Mbed TLS AES and
 * DES/3DES (src/lib/mbedtls, issue #447) as libnfc builds them
 * (nfc_mbedtls_config.h, ROM tables, the shim's zeroize).
 * Vectors: FIPS-197 C.1, SP 800-38A F.2.1/F.2.2, the textbook DES example
 * (key 133457799BBCDFF1), and 2-key 3DES-CBC values cross-checked with
 * OpenSSL des-ede-cbc.  Then the upstream Mbed TLS self-tests.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>
#include <mbedtls/aes.h>
#include <mbedtls/include/mbedtls/des.h>

static int fails;
#define CHECK(c, msg)                  \
    do {                               \
        if(!(c)) {                     \
            printf("FAIL: %s\n", msg); \
            fails++;                   \
        } else                         \
            printf("ok: %s\n", msg);   \
    } while(0)

static void hex(const char* s, unsigned char* out) {
    for(size_t i = 0; s[2 * i]; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (unsigned char)v;
    }
}

int main(void) {
    unsigned char k[32], iv[16], in[32], want[32], out[32], back[32];

    /* AES-128 ECB, FIPS-197 appendix C.1 */
    mbedtls_aes_context a;
    mbedtls_aes_init(&a);
    hex("000102030405060708090a0b0c0d0e0f", k);
    hex("00112233445566778899aabbccddeeff", in);
    hex("69c4e0d86a7b0430d8cdb78070b4c55a", want);
    CHECK(mbedtls_aes_setkey_enc(&a, k, 128) == 0, "AES-128 setkey_enc");
    mbedtls_aes_crypt_ecb(&a, MBEDTLS_AES_ENCRYPT, in, out);
    CHECK(memcmp(out, want, 16) == 0, "AES-128 ECB encrypt (FIPS-197 C.1)");
    CHECK(mbedtls_aes_setkey_dec(&a, k, 128) == 0, "AES-128 setkey_dec");
    mbedtls_aes_crypt_ecb(&a, MBEDTLS_AES_DECRYPT, want, back);
    CHECK(memcmp(back, in, 16) == 0, "AES-128 ECB decrypt (FIPS-197 C.1)");

    /* AES-128 CBC, SP 800-38A F.2.1 / F.2.2 (first two blocks) */
    hex("2b7e151628aed2a6abf7158809cf4f3c", k);
    hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51", in);
    hex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2", want);
    hex("000102030405060708090a0b0c0d0e0f", iv);
    mbedtls_aes_setkey_enc(&a, k, 128);
    CHECK(mbedtls_aes_crypt_cbc(&a, MBEDTLS_AES_ENCRYPT, 32, iv, in, out) == 0 &&
              memcmp(out, want, 32) == 0,
          "AES-128 CBC encrypt (SP 800-38A F.2.1)");
    hex("000102030405060708090a0b0c0d0e0f", iv);
    mbedtls_aes_setkey_dec(&a, k, 128);
    CHECK(mbedtls_aes_crypt_cbc(&a, MBEDTLS_AES_DECRYPT, 32, iv, want, back) == 0 &&
              memcmp(back, in, 32) == 0,
          "AES-128 CBC decrypt (SP 800-38A F.2.2)");
    CHECK(mbedtls_aes_crypt_cbc(&a, MBEDTLS_AES_DECRYPT, 15, iv, want, back) != 0,
          "AES CBC rejects a partial block");
    mbedtls_aes_free(&a);
    {
        static const unsigned char zero[sizeof a];
        CHECK(memcmp(&a, zero, sizeof a) == 0, "AES context zeroed by free");
    }

    /* single DES ECB, the textbook worked example */
    mbedtls_des_context d;
    mbedtls_des_init(&d);
    hex("133457799bbcdff1", k);
    hex("0123456789abcdef", in);
    hex("85e813540f0ab405", want);
    mbedtls_des_setkey_enc(&d, k);
    mbedtls_des_crypt_ecb(&d, in, out);
    CHECK(memcmp(out, want, 8) == 0, "DES ECB encrypt");
    mbedtls_des_free(&d);

    /* 2-key 3DES CBC (the Ultralight C cipher), OpenSSL des-ede-cbc */
    mbedtls_des3_context t;
    mbedtls_des3_init(&t);
    hex("0123456789abcdeffedcba9876543210", k);
    hex("000102030405060708090a0b0c0d0e0f", in);
    hex("52c5c0705d9089e1fd2f0cf61dd3bc0a", want);
    memset(iv, 0, 8);
    CHECK(mbedtls_des3_set2key_enc(&t, k) == 0, "3DES set2key_enc");
    CHECK(mbedtls_des3_crypt_cbc(&t, MBEDTLS_DES_ENCRYPT, 16, iv, in, out) == 0 &&
              memcmp(out, want, 16) == 0,
          "3DES-2key CBC encrypt");
    memset(iv, 0, 8);
    CHECK(mbedtls_des3_set2key_dec(&t, k) == 0, "3DES set2key_dec");
    CHECK(mbedtls_des3_crypt_cbc(&t, MBEDTLS_DES_DECRYPT, 16, iv, want, back) == 0 &&
              memcmp(back, in, 16) == 0,
          "3DES-2key CBC decrypt");
    mbedtls_des3_free(&t);

#ifdef MBEDTLS_SELF_TEST
    CHECK(mbedtls_aes_self_test(0) == 0, "Mbed TLS AES self-test");
    CHECK(mbedtls_des_self_test(0) == 0, "Mbed TLS DES self-test");
#endif

    if(fails) {
        printf("%d crypto test(s) FAILED\n", fails);
        return 1;
    }
    printf("cryptotest: all passed\n");
    return 0;
}
