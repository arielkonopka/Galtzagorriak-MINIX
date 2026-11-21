/* cm_felica.h - a software FeliCa Lite-S for the tests (#412).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #412.
 *
 * JIS X 6319-4 frames: LEN, command, ..., CRC (CRC-16/CCITT, init 0,
 * sent high byte first); written here from the standard, not from the
 * vendored felica_crc.c.  Polling (0x00) -> IDm, PMm; Read Without
 * Encryption (0x06) on service 0x000B -> the blocks asked for.  The
 * Lite-S memory map: S_PAD0-13 (0x00-0x0D), REG (0x0E), RC..MC
 * (0x80-0x88), WCNT/MAC_A/STATE (0x90-0x92), CRC_CHECK (0xA0); CK
 * (0x87) and RC (0x80) are write-only: status flags 01 A8 for them.
 */
#ifndef CM_FELICA_H
#define CM_FELICA_H

#include <stdint.h>
#include <string.h>

static uint16_t cm_crc_f(const uint8_t* p, size_t n) {
    uint16_t crc = 0;
    for(size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)p[i] << 8;
        for(int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

typedef struct {
    uint8_t idm[8], pmm[8];
    uint8_t blk[256][16];
    unsigned polls, reads;
    int lost; /* answers nothing */
} cm_felica_t;

static void cm_felica_lite_s(cm_felica_t* k) {
    static const uint8_t idm[8] = {0x01, 0x2E, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    static const uint8_t pmm[8] = {0x00, 0xF1, 0x00, 0x00, 0x00, 0x01, 0x43, 0x00};
    memset(k, 0, sizeof *k);
    memcpy(k->idm, idm, 8);
    memcpy(k->pmm, pmm, 8);
    for(int b = 0; b < 14; b++)
        for(int i = 0; i < 16; i++) k->blk[b][i] = (uint8_t)(b * 16 + i);
    memcpy(k->blk[0x82], idm, 8);            /* ID: IDm, then DFC etc. */
    memcpy(k->blk[0x83], idm, 8);            /* D_ID */
    k->blk[0x85][0] = 0x88; k->blk[0x85][1] = 0xB4; /* SYS_C 88B4 */
    k->blk[0x88][0] = 0xFF; k->blk[0x88][1] = 0xFF; /* MC */
}

static int cm_felica_xfer(void* ctx, const uint8_t* tx, size_t bits, uint8_t* rx, size_t room,
                          size_t* rxb, uint32_t fwt, uint32_t flags) {
    cm_felica_t* k = ctx;
    (void)fwt;
    (void)flags;
    size_t n = bits / 8, out = 0;
    if(k->lost || bits % 8 || n < 4 || tx[0] != n - 2) return -1;
    uint16_t c = cm_crc_f(tx, n - 2);
    if(tx[n - 2] != (c >> 8) || tx[n - 1] != (c & 0xff)) return -1;
    n -= 2;
    if(tx[1] == 0x00 && n == 6) { /* Polling: SC, RC, TSN */
        if(!((tx[2] == 0xFF || tx[2] == 0x88) && (tx[3] == 0xFF || tx[3] == 0xB4))) return -1;
        k->polls++;
        rx[1] = 0x01;
        memcpy(rx + 2, k->idm, 8);
        memcpy(rx + 10, k->pmm, 8);
        out = 18;
    } else if(tx[1] == 0x06 && n >= 14 && !memcmp(tx + 2, k->idm, 8)) {
        if(tx[10] != 1 || tx[11] != 0x0B || tx[12] != 0x00) return -1; /* one service, 000B */
        unsigned nb = tx[13], p = 14;
        uint8_t list[4];
        if(nb < 1 || nb > 4) return -1;
        for(unsigned i = 0; i < nb; i++) {
            if(p >= n) return -1;
            if(tx[p] & 0x80) { list[i] = tx[p + 1]; p += 2; }
            else { list[i] = tx[p + 1]; p += 3; } /* 3-byte element, block < 256 here */
        }
        k->reads++;
        rx[1] = 0x07;
        memcpy(rx + 2, k->idm, 8);
        int bad = 0;
        for(unsigned i = 0; i < nb; i++) {
            uint8_t b = list[i];
            int ok = b <= 0x0E || (b >= 0x81 && b <= 0x88 && b != 0x87) || (b >= 0x90 && b <= 0x92) || b == 0xA0;
            if(!ok) bad = 1;
        }
        if(bad) {
            rx[10] = 0x01; rx[11] = 0xA8;
            out = 12;
        } else {
            rx[10] = 0x00; rx[11] = 0x00; rx[12] = (uint8_t)nb;
            for(unsigned i = 0; i < nb; i++) memcpy(rx + 13 + 16 * i, k->blk[list[i]], 16);
            out = 13 + 16 * nb;
        }
    } else
        return -1;
    rx[0] = (uint8_t)out;
    if(8 * (out + 2) > room) return -5;
    c = cm_crc_f(rx, out);
    rx[out] = c >> 8;
    rx[out + 1] = c & 0xff;
    *rxb = 8 * (out + 2);
    return 0;
}

#endif
