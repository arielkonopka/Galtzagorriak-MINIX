/* nfc_sha1.c - SHA-1 (FIPS 180-4), see nfc_sha1.h (#412).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include "nfc_sha1.h"

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(uint32_t h[5], const uint8_t* p) {
    uint32_t w[16], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for(int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for(int t = 0; t < 80; t++) {
        uint32_t f, k;
        if(t >= 16) {
            uint32_t x = w[(t - 3) & 15] ^ w[(t - 8) & 15] ^ w[(t - 14) & 15] ^ w[t & 15];
            w[t & 15] = ROL(x, 1);
        }
        if(t < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if(t < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if(t < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        uint32_t tmp = ROL(a, 5) + f + e + k + w[t & 15];
        e = d;
        d = c;
        c = ROL(b, 30);
        b = a;
        a = tmp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void nfc_sha1(const uint8_t* data, size_t len, uint8_t digest[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint8_t blk[64];
    size_t n = len;
    while(n >= 64) {
        sha1_block(h, data);
        data += 64;
        n -= 64;
    }
    memset(blk, 0, sizeof blk);
    memcpy(blk, data, n);
    blk[n] = 0x80;
    if(n >= 56) {
        sha1_block(h, blk);
        memset(blk, 0, sizeof blk);
    }
    uint64_t bits = (uint64_t)len * 8;
    for(int i = 0; i < 8; i++) blk[63 - i] = (uint8_t)(bits >> (8 * i));
    sha1_block(h, blk);
    for(int i = 0; i < 5; i++) {
        digest[4 * i] = (uint8_t)(h[i] >> 24);
        digest[4 * i + 1] = (uint8_t)(h[i] >> 16);
        digest[4 * i + 2] = (uint8_t)(h[i] >> 8);
        digest[4 * i + 3] = (uint8_t)h[i];
    }
}
