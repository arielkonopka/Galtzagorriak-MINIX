/* cm_15693.h - a software ISO 15693 tag (NXP ICODE SLIX2 by default) for
 * the vendored 15693 and SLIX poller tests (#404).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Answers, non-addressed or addressed (then the UID must match): INVENTORY
 * (1 slot), GET SYSTEM INFO, READ SINGLE BLOCK, GET MULTIPLE BLOCK
 * SECURITY STATUS and the NXP custom GET NXP SYSTEM INFO and READ
 * SIGNATURE.  CRC: ISO 15693-3 (init FFFF, inverted), checked and added.
 * Error frames (flag 01, code) for what it does not know.
 */
#ifndef CM_15693_H
#define CM_15693_H

#include "cardmodel.h"

typedef struct {
    uint8_t uid[8];         /* as on the air: LSB first, uid[7] = 0xE0 */
    uint8_t dsfid, afi, ic_ref;
    uint8_t blocks, block_size;
    uint8_t mem[80 * 4];
    uint8_t lock[80];
    uint8_t signature[32];
    int nxp;                /* answers the NXP custom commands */
    unsigned inventories, reads, custom;
} cm_15693_t;

static size_t cm_v_err(uint8_t* r, uint8_t code) {
    r[0] = 0x01;
    r[1] = code;
    return 2;
}

static int cm_15693_xfer(void* ctx, const uint8_t* tx, size_t bits, uint8_t* rx, size_t room,
                         size_t* rxb, uint32_t fwt, uint32_t flags) {
    cm_15693_t* k = ctx;
    (void)fwt;
    (void)flags;
    size_t n = bits / 8, out = 0;
    if(bits % 8 || !cm_crc_ok(tx, n, CM_CRC_B)) return -1;
    n -= 2;
    if(n < 2) return -1;
    uint8_t f = tx[0], cmd = tx[1];
    size_t p = 2;
    int custom = cmd >= 0xA0;
    if(custom) {
        if(n < 3 || tx[2] != 0x04 || !k->nxp) return -1; /* not our maker: silent */
        p = 3;
    }
    if(!(f & 0x04) && (f & 0x20)) { /* addressed */
        if(n < p + 8 || memcmp(tx + p, k->uid, 8)) return -1;
        p += 8;
    }
    if(f & 0x04) { /* inventory flags */
        if(cmd != 0x01) return -1;
        k->inventories++;
        rx[0] = 0x00;
        rx[1] = k->dsfid;
        memcpy(rx + 2, k->uid, 8);
        out = 10;
    } else if(cmd == 0x2B) {
        rx[0] = 0x00;
        rx[1] = 0x0F;
        memcpy(rx + 2, k->uid, 8);
        rx[10] = k->dsfid;
        rx[11] = k->afi;
        rx[12] = k->blocks - 1;
        rx[13] = k->block_size - 1;
        rx[14] = k->ic_ref;
        out = 15;
    } else if(cmd == 0x20) {
        if(n != p + 1) out = cm_v_err(rx, 0x02);
        else if(tx[p] >= k->blocks) out = cm_v_err(rx, 0x10);
        else {
            rx[0] = 0x00;
            memcpy(rx + 1, k->mem + tx[p] * k->block_size, k->block_size);
            out = 1 + k->block_size;
            k->reads++;
        }
    } else if(cmd == 0x2C) {
        if(n != p + 2 || tx[p] + tx[p + 1] >= k->blocks) out = cm_v_err(rx, 0x10);
        else {
            rx[0] = 0x00;
            memcpy(rx + 1, k->lock + tx[p], tx[p + 1] + 1);
            out = 2 + tx[p + 1];
        }
    } else if(cmd == 0xAB) {
        k->custom++;
        /* flags, PP pointer, PP condition, lock bits, feature flags (LE) */
        memcpy(rx, "\x00\x00\x00\x00\x37\x01\x1F\x00", 8);
        out = 8;
    } else if(cmd == 0xBD) {
        k->custom++;
        rx[0] = 0x00;
        memcpy(rx + 1, k->signature, 32);
        out = 33;
    } else
        out = cm_v_err(rx, 0x01);
    if(8 * (out + 2) > room) return -5;
    *rxb = 8 * cm_crc_add(rx, out, CM_CRC_B);
    return 0;
}

/* An ICODE SLIX2: E0 04 01, type indicator 01 (bits 3-4 of the 4th byte). */
static void cm_15693_slix2(cm_15693_t* k) {
    static const uint8_t stored[8] = {0xE0, 0x04, 0x01, 0x08, 0x12, 0x34, 0x56, 0x78};
    memset(k, 0, sizeof *k);
    for(int i = 0; i < 8; i++) k->uid[i] = stored[7 - i];
    k->dsfid = 0x00;
    k->afi = 0x00;
    k->ic_ref = 0x01;
    k->blocks = 80;
    k->block_size = 4;
    for(int i = 0; i < 80 * 4; i++) k->mem[i] = (uint8_t)(i * 7 + 1);
    k->lock[3] = 0x01;
    for(int i = 0; i < 32; i++) k->signature[i] = (uint8_t)(0x55 ^ i);
    k->nxp = 1;
}

#endif
