/* cm_mfc.h - a software MIFARE Classic 1K/4K for the vendored poller (#404).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #404.
 *
 * The card #392 modelled for libnfc's own reader, redone at the level the
 * vendored mf_classic poller talks: the first AUTH and its nonce in plain
 * frames (flags 0, the nonce without CRC), everything after it encrypted
 * in custom-parity frames (NFC_XF_PARITY: 9 bits per byte, data LSB first,
 * then the parity bit).  Crypto-1 is a private copy (cmc_ prefix) of the
 * public algorithm, not the vendored helper the poller uses, so a wrong
 * helper cannot pass its own test.  Wrong key, unknown block or a block
 * the access bits would refuse: the card stays silent, as a real one does.
 * Supports AUTH, READ, WRITE, INCREMENT/DECREMENT/RESTORE + TRANSFER and
 * HALT; a nested AUTH (an encrypted AUTH inside a session) too.
 */
#ifndef CM_MFC_H
#define CM_MFC_H

#include "cardmodel.h"

/* ---- Crypto-1 (Garcia et al.; Crapto1) ---- */
typedef struct { uint32_t odd, even; } cmc_t;
#define CMC_BIT(x, n) ((uint32_t)((x) >> (n)) & 1u)

static uint32_t cmc_filter(uint32_t x) {
    uint32_t f;
    f = 0xf22c0u >> (x & 0xf) & 16u;
    f |= 0x6c9c0u >> (x >> 4 & 0xf) & 8u;
    f |= 0x3c8b0u >> (x >> 8 & 0xf) & 4u;
    f |= 0x1e458u >> (x >> 12 & 0xf) & 2u;
    f |= 0x0d938u >> (x >> 16 & 0xf) & 1u;
    return CMC_BIT(0xEC57E80Au, f);
}
static void cmc_init(cmc_t* c, uint64_t key) {
    c->odd = c->even = 0;
    for(int i = 47; i > 0; i -= 2) {
        c->odd = c->odd << 1 | CMC_BIT(key, (i - 1) ^ 7);
        c->even = c->even << 1 | CMC_BIT(key, i ^ 7);
    }
}
static uint8_t cmc_bit(cmc_t* c, uint8_t in, int enc) {
    uint32_t out = cmc_filter(c->odd), feed = (out & (enc ? 1u : 0u)) ^ (in ? 1u : 0u);
    feed ^= (0x29CE5Cu & c->odd) ^ (0x870804u & c->even);
    feed ^= feed >> 16;
    feed ^= feed >> 8;
    feed ^= feed >> 4;
    feed ^= feed >> 2;
    feed ^= feed >> 1;
    c->even = c->even << 1 | (feed & 1u);
    uint32_t t = c->odd;
    c->odd = c->even;
    c->even = t;
    return (uint8_t)out;
}
static uint8_t cmc_byte(cmc_t* c, uint8_t in, int enc) {
    uint8_t r = 0;
    for(int i = 0; i < 8; i++) r |= cmc_bit(c, CMC_BIT(in, i), enc) << i;
    return r;
}
static uint32_t cmc_word(cmc_t* c, uint32_t in, int enc) {
    uint32_t r = 0;
    for(int i = 0; i < 32; i++) r |= (uint32_t)cmc_bit(c, CMC_BIT(in, i ^ 24), enc) << (24 ^ i);
    return r;
}
static uint32_t cmc_swap(uint32_t x) {
    x = (x >> 8 & 0x00ff00ffu) | (x & 0x00ff00ffu) << 8;
    return x >> 16 | x << 16;
}
static uint32_t cmc_succ(uint32_t x, uint32_t n) {
    x = cmc_swap(x);
    while(n--) x = x >> 1 | (CMC_BIT(x, 16) ^ CMC_BIT(x, 18) ^ CMC_BIT(x, 19) ^ CMC_BIT(x, 21)) << 31;
    return cmc_swap(x);
}
static int cmc_oddpar(uint8_t b) {
    b ^= b >> 4;
    b ^= b >> 2;
    b ^= b >> 1;
    return !(b & 1);
}

/* ---- the 9-bit parity stream ---- */
static size_t cmc_unpack(const uint8_t* s, size_t bits, uint8_t* out) {
    size_t n = bits / 9;
    for(size_t i = 0; i < n; i++) {
        uint8_t b = 0;
        for(int j = 0; j < 8; j++) {
            size_t p = i * 9 + j;
            b |= ((s[p / 8] >> (p % 8)) & 1) << j;
        }
        out[i] = b;
    }
    return n;
}
static void cmc_put(uint8_t* s, size_t p, int v) {
    if(v) s[p / 8] |= 1u << (p % 8);
    else s[p / 8] &= ~(1u << (p % 8));
}

/* ---- the card ---- */
typedef struct {
    cm_3a_t a;
    unsigned sectors;          /* 16 (1K) or 40 (4K) */
    uint8_t block[256][16];    /* trailers hold the keys */
    int phase;                 /* 0 idle, 1 awaiting {nr}{ar}, 2 authed */
    int pending;               /* 0, or a WRITE/value cmd waiting for data */
    uint8_t pend_blk;
    int32_t value_reg;
    uint8_t sector;
    int key_b;
    uint32_t nt, rng, uid;
    cmc_t c;
    unsigned auths, auth_fail, reads, writes, halts;
} cm_mfc_t;

static unsigned cmc_first_block(unsigned s) { return s < 32 ? s * 4 : 128 + (s - 32) * 16; }
static unsigned cmc_blocks(unsigned s) { return s < 32 ? 4 : 16; }
static int cmc_sector_of(unsigned blk) { return blk < 128 ? (int)(blk / 4) : 32 + (int)((blk - 128) / 16); }
static unsigned cmc_trailer(unsigned s) { return cmc_first_block(s) + cmc_blocks(s) - 1; }
static uint64_t cmc_key(const cm_mfc_t* k, unsigned s, int b) {
    const uint8_t* t = k->block[cmc_trailer(s)] + (b ? 10 : 0);
    uint64_t v = 0;
    for(int i = 0; i < 6; i++) v = v << 8 | t[i];
    return v;
}

/* A fresh 1K/4K: every key FFFFFFFFFFFF, transport access bits FF0780. */
static void cm_mfc_init(cm_mfc_t* k, const uint8_t uid[4], unsigned sectors) {
    memset(k, 0, sizeof *k);
    memcpy(k->a.uid, uid, 4);
    k->a.uid_len = 4;
    k->a.atqa[0] = sectors > 16 ? 0x02 : 0x04;
    k->a.sak = sectors > 16 ? 0x18 : 0x08;
    k->sectors = sectors;
    k->uid = (uint32_t)uid[0] << 24 | (uint32_t)uid[1] << 16 | (uint32_t)uid[2] << 8 | uid[3];
    k->rng = 0x1F2E3D4Cu;
    for(unsigned s = 0; s < sectors; s++) {
        uint8_t* t = k->block[cmc_trailer(s)];
        memset(t, 0xFF, 16);
        t[6] = 0xFF, t[7] = 0x07, t[8] = 0x80, t[9] = 0x69;
    }
    memcpy(k->block[0], uid, 4);
    k->block[0][4] = uid[0] ^ uid[1] ^ uid[2] ^ uid[3];
    k->block[0][5] = k->a.sak;
    k->block[0][6] = k->a.atqa[1];
    k->block[0][7] = k->a.atqa[0];
}
static void cm_mfc_set_key(cm_mfc_t* k, unsigned s, int b, uint64_t key) {
    uint8_t* t = k->block[cmc_trailer(s)] + (b ? 10 : 0);
    for(int i = 5; i >= 0; i--, key >>= 8) t[i] = (uint8_t)key;
}

/* Encrypt n bytes of plain (CRC already in) into the parity stream. */
static size_t cmc_send(cm_mfc_t* k, const uint8_t* plain, size_t n, uint8_t* rx) {
    memset(rx, 0, (n * 9 + 7) / 8);
    for(size_t i = 0; i < n; i++) {
        uint8_t e = plain[i] ^ cmc_byte(&k->c, 0, 0);
        for(int j = 0; j < 8; j++) cmc_put(rx, i * 9 + j, (e >> j) & 1);
        cmc_put(rx, i * 9 + 8, cmc_oddpar(plain[i]) ^ (int)cmc_filter(k->c.odd));
    }
    return n * 9;
}
static size_t cmc_send_nibble(cm_mfc_t* k, uint8_t v, uint8_t* rx) {
    uint8_t e = 0;
    for(int j = 0; j < 4; j++) e |= (uint8_t)((((v >> j) & 1) ^ cmc_bit(&k->c, 0, 0)) << j);
    rx[0] = e;
    return 4;
}
static void cmc_value_set(uint8_t* b, int32_t v, uint8_t addr) {
    uint32_t u = (uint32_t)v;
    for(int i = 0; i < 4; i++) {
        b[i] = (uint8_t)(u >> (8 * i));
        b[4 + i] = (uint8_t)~b[i];
        b[8 + i] = b[i];
    }
    b[12] = addr, b[13] = (uint8_t)~addr, b[14] = addr, b[15] = (uint8_t)~addr;
}
static int cmc_value_get(const uint8_t* b, int32_t* v) {
    for(int i = 0; i < 4; i++)
        if(b[i] != b[8 + i] || (uint8_t)~b[i] != b[4 + i]) return 0;
    *v = (int32_t)((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24);
    return 1;
}

static int cmc_auth_start(cm_mfc_t* k, const uint8_t* cmd, uint8_t* nt_out) {
    int s = cmc_sector_of(cmd[1]);
    if(cmd[1] >= cmc_first_block(k->sectors) || s < 0) return -1;
    k->sector = (uint8_t)s;
    k->key_b = cmd[0] == 0x61;
    k->rng = k->rng * 1103515245u + 12345u;
    k->nt = k->rng ? k->rng : 0x01020304u; /* a hard PRNG: any 32-bit nonce */
    k->auths++;
    for(int i = 0; i < 4; i++) nt_out[i] = (uint8_t)(k->nt >> (24 - 8 * i));
    return 0;
}

/* Field off: the card loses power, so halt and Crypto-1 state go. */
static int cm_mfc_field(void* ctx, int on, uint32_t tech) {
    cm_mfc_t* k = ctx;
    (void)tech;
    if(!on) {
        k->a.halted = k->a.active = 0;
        k->phase = k->pending = 0;
    }
    return 0;
}

static int cm_mfc_xfer(void* ctx, const uint8_t* tx, size_t bits, uint8_t* rx, size_t room,
                       size_t* rxb, uint32_t fwt, uint32_t flags) {
    cm_mfc_t* k = ctx;
    uint8_t p[32];
    int err;
    (void)fwt;
    (void)room;
    *rxb = 0;
    if(!(flags & NFC_XF_PARITY)) {
        if(cm_3a(&k->a, tx, bits, rx, rxb, &err)) {
            k->phase = 0;
            k->pending = 0;
            return err ? -1 : 0;
        }
        /* plain AUTH: 60/61 blk CRC -> 4-byte nonce, no CRC */
        size_t n = bits / 8;
        if(!k->a.active || bits % 8 || n != 4 || (tx[0] != 0x60 && tx[0] != 0x61) ||
           !cm_crc_ok(tx, n, CM_CRC_A))
            return -1;
        if(cmc_auth_start(k, tx, rx)) return -1;
        cmc_init(&k->c, cmc_key(k, k->sector, k->key_b));
        (void)cmc_word(&k->c, k->uid ^ k->nt, 0);
        k->phase = 1;
        *rxb = 32;
        return 0;
    }
    if(!k->a.active || bits > sizeof p * 9) return -1;
    size_t n = cmc_unpack(tx, bits, p);
    if(k->phase == 1) { /* {nr}{ar} */
        k->phase = 0;
        if(n != 8) return -1;
        uint32_t nr = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        uint32_t ar = (uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7];
        (void)cmc_word(&k->c, nr, 1);
        ar ^= cmc_word(&k->c, 0, 0);
        if(ar != cmc_succ(k->nt, 64)) {
            k->auth_fail++;
            return -1; /* wrong key: silent */
        }
        uint32_t at = cmc_succ(k->nt, 96);
        uint8_t a4[4] = {(uint8_t)(at >> 24), (uint8_t)(at >> 16), (uint8_t)(at >> 8), (uint8_t)at};
        *rxb = cmc_send(k, a4, 4, rx);
        k->phase = 2;
        return 0;
    }
    if(k->phase != 2 || n == 0) return -1;
    for(size_t i = 0; i < n; i++) p[i] ^= cmc_byte(&k->c, 0, 0);
    if(k->pending) { /* data of a WRITE (16+CRC) or value cmd (4+CRC) */
        int cmd = k->pending;
        k->pending = 0;
        if(!cm_crc_ok(p, n, CM_CRC_A)) return -1;
        if(cmd == 0xA0 && n == 18) {
            memcpy(k->block[k->pend_blk], p, 16);
            k->writes++;
            *rxb = cmc_send_nibble(k, 0x0A, rx);
            return 0;
        }
        if(n == 6) { /* INC/DEC/RESTORE: no ACK */
            int32_t d = (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
                                  (uint32_t)p[3] << 24);
            if(cmd == 0xC1) k->value_reg += d;
            else if(cmd == 0xC0) k->value_reg -= d;
            return -1;
        }
        return -1;
    }
    if(!cm_crc_ok(p, n, CM_CRC_A) || n != 4) return -1;
    uint8_t blk = p[1];
    int in_sector = cmc_sector_of(blk) == k->sector && blk < cmc_first_block(k->sectors);
    switch(p[0]) {
    case 0x30: { /* READ: a trailer reads with key A masked as zero */
        if(!in_sector) return -1;
        uint8_t out[18];
        memcpy(out, k->block[blk], 16);
        if(blk == cmc_trailer(k->sector)) memset(out, 0, 6);
        cm_crc_add(out, 16, CM_CRC_A);
        k->reads++;
        *rxb = cmc_send(k, out, 18, rx);
        return 0;
    }
    case 0xA0: case 0xC0: case 0xC1: case 0xC2:
        if(!in_sector) return -1;
        if(p[0] != 0xA0 && !cmc_value_get(k->block[blk], &k->value_reg)) return -1;
        k->pending = p[0];
        k->pend_blk = blk;
        *rxb = cmc_send_nibble(k, 0x0A, rx);
        return 0;
    case 0xB0: /* TRANSFER */
        if(!in_sector) return -1;
        cmc_value_set(k->block[blk], k->value_reg, blk);
        *rxb = cmc_send_nibble(k, 0x0A, rx);
        return 0;
    case 0x50: /* HALT */
        k->halts++;
        k->a.halted = 1;
        k->a.active = 0;
        k->phase = 0;
        return -1;
    case 0x60: case 0x61: { /* nested AUTH: nonce encrypted under the old key */
        uint8_t nt[4];
        if(cmc_auth_start(k, p, nt)) return -1;
        uint32_t ntw = k->nt;
        uint64_t key = cmc_key(k, k->sector, k->key_b);
        memset(rx, 0, 5);
        uint32_t ks = 0;
        cmc_t c2;
        cmc_init(&c2, key);
        ks = cmc_word(&c2, k->uid ^ ntw, 0);
        for(int i = 0; i < 4; i++) {
            uint8_t e = nt[i] ^ (uint8_t)(ks >> (24 - 8 * i));
            for(int j = 0; j < 8; j++) cmc_put(rx, i * 9 + j, (e >> j) & 1);
            cmc_put(rx, i * 9 + 8, cmc_oddpar(nt[i]) ^ (int)cmc_filter(k->c.odd));
        }
        k->c = c2;
        k->phase = 1;
        *rxb = 36;
        return 0;
    }
    }
    return -1;
}

#endif
