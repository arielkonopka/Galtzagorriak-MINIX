/* cm_mful.h - a software NTAG21x / MIFARE Ultralight EV1 for the tests (#412).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #412.
 *
 * Answers as the NXP data sheets (NTAG213/215/216, MF0ULx1) describe:
 * GET_VERSION, READ (4 pages, wrapping at the end), FAST_READ, READ_SIG,
 * READ_CNT (counter 2 only, if NFC_CNT_EN), PWD_AUTH -> PACK or NAK; any
 * other command NAKs (4-bit 0x0) and the card goes back to idle.  PWD and
 * PACK read as zeros.  Both reader styles are served: the vendored poller
 * puts CRC_A in the frame itself (flags 0), libnfc's own mf_ultralight.c
 * asks for NFC_XF_CRC_TX|NFC_XF_CRC_RX.
 */
#ifndef CM_MFUL_H
#define CM_MFUL_H

#include "cardmodel.h"

typedef struct {
    cm_3a_t a;
    uint8_t version[8];
    uint16_t pages;          /* total, config pages included */
    uint8_t mem[231][4];     /* NTAG216 is the largest here */
    uint8_t sig[32];
    uint32_t cnt2;
    uint8_t pwd[4], pack[2];
    uint16_t cfg;            /* first config page (MIRROR/AUTH0) */
    int authed;
    unsigned reads, pwd_tries, version_seen;
} cm_mful_t;

/* NTAG215: 135 pages, config at 131, user memory 4..129 filled with a
 * pattern, CC E1 10 3E 00. */
static void cm_mful_ntag215(cm_mful_t* k) {
    static const uint8_t uid[7] = {0x04, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0x80};
    static const uint8_t ver[8] = {0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x11, 0x03};
    memset(k, 0, sizeof *k);
    memcpy(k->a.uid, uid, 7);
    k->a.uid_len = 7;
    k->a.atqa[0] = 0x44;
    k->a.atqa[1] = 0x00;
    k->a.sak = 0x00;
    memcpy(k->version, ver, 8);
    k->pages = 135;
    k->cfg = 131;
    /* page 0: UID0-2, BCC0 = 0x88^UID0^UID1^UID2; page 1: UID3-6; page 2: BCC1, internal, lock */
    k->mem[0][0] = uid[0]; k->mem[0][1] = uid[1]; k->mem[0][2] = uid[2];
    k->mem[0][3] = 0x88 ^ uid[0] ^ uid[1] ^ uid[2];
    memcpy(k->mem[1], uid + 3, 4);
    k->mem[2][0] = uid[3] ^ uid[4] ^ uid[5] ^ uid[6];
    k->mem[2][1] = 0x48;
    k->mem[3][0] = 0xE1; k->mem[3][1] = 0x10; k->mem[3][2] = 0x3E; k->mem[3][3] = 0x00;
    for(int p = 4; p < 130; p++)
        for(int b = 0; b < 4; b++) k->mem[p][b] = (uint8_t)(p * 7 + b);
    k->mem[130][0] = 0x00; k->mem[130][1] = 0x00; k->mem[130][2] = 0x00; k->mem[130][3] = 0xBD;
    k->mem[131][0] = 0x04; k->mem[131][3] = 0xFF;   /* AUTH0 = FF: no protection */
    k->mem[132][0] = 0x10;                           /* ACCESS: NFC_CNT_EN, AUTHLIM 0 */
    for(int i = 0; i < 32; i++) k->sig[i] = (uint8_t)(0xA0 + i);
    k->cnt2 = 0x000123;
    k->pwd[0] = 0x12; k->pwd[1] = 0x34; k->pwd[2] = 0x56; k->pwd[3] = 0x78;
    k->pack[0] = 0x80; k->pack[1] = 0x80;
}

static void cm_mful_page(const cm_mful_t* k, unsigned p, uint8_t* out) {
    p %= k->pages;
    if(p == k->cfg + 2u || p == k->cfg + 3u) memset(out, 0, 4); /* PWD, PACK */
    else memcpy(out, k->mem[p], 4);
}

static int cm_mful_xfer(void* ctx, const uint8_t* tx, size_t bits, uint8_t* rx, size_t room,
                        size_t* rxb, uint32_t fwt, uint32_t flags) {
    cm_mful_t* k = ctx;
    int err;
    (void)fwt;
    if(cm_3a(&k->a, tx, bits, rx, rxb, &err)) {
        if(bits == 7) k->authed = 0;
        return err ? -1 : 0;
    }
    size_t n = bits / 8, out = 0;
    int crc_in = !(flags & NFC_XF_CRC_TX), crc_out = !(flags & NFC_XF_CRC_RX);
    if(!k->a.active || bits % 8) return -1;
    if(crc_in) {
        if(!cm_crc_ok(tx, n, CM_CRC_A)) return -1;
        n -= 2;
    }
    if(n < 1) return -1;
    switch(tx[0]) {
    case 0x60:
        if(n != 1) goto nak;
        k->version_seen++;
        memcpy(rx, k->version, 8);
        out = 8;
        break;
    case 0x30:
        if(n != 2 || tx[1] >= k->pages) goto nak;
        k->reads++;
        for(int i = 0; i < 4; i++) cm_mful_page(k, tx[1] + i, rx + 4 * i);
        out = 16;
        break;
    case 0x3A:
        if(n != 3 || tx[1] > tx[2] || tx[2] >= k->pages || (size_t)(tx[2] - tx[1] + 1) * 4 + 2 > room / 8)
            goto nak;
        for(unsigned p = tx[1]; p <= tx[2]; p++) {
            cm_mful_page(k, p, rx + out);
            out += 4;
        }
        break;
    case 0x3C:
        if(n != 2 || tx[1] != 0) goto nak;
        memcpy(rx, k->sig, 32);
        out = 32;
        break;
    case 0x39:
        if(n != 2 || tx[1] != 2 || !(k->mem[k->cfg + 1][0] & 0x10)) goto nak;
        rx[0] = k->cnt2 & 0xff;
        rx[1] = (k->cnt2 >> 8) & 0xff;
        rx[2] = (k->cnt2 >> 16) & 0xff;
        out = 3;
        break;
    case 0x1B:
        if(n != 5) goto nak;
        k->pwd_tries++;
        if(memcmp(tx + 1, k->pwd, 4)) goto nak;
        k->authed = 1;
        memcpy(rx, k->pack, 2);
        out = 2;
        break;
    default:
        goto nak;
    }
    if(8 * (out + 2) > room) return -5;
    *rxb = 8 * (crc_out ? cm_crc_add(rx, out, CM_CRC_A) : out);
    return 0;
nak:
    k->a.active = 0; /* a NAK sends the card back to IDLE */
    rx[0] = 0x00;
    *rxb = 4;
    return 0;
}

#endif
