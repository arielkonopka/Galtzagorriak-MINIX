/* cm_st25tb.h - a software ST25TB / SRIX for the tests (#412).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #412.
 *
 * ISO 14443-2 type B framing with CRC_B (cardmodel.h) and the ST command
 * set (ST25TB04K data sheet): INITIATE 06 00 -> chip ID, SELECT 0E id ->
 * id, GET_UID 0B -> 8 bytes LSB first, READ_BLOCK 08 n -> 4 bytes,
 * WRITE_BLOCK 09 n d0..d3 -> no answer, COMPLETION 0F -> deactivated.
 * Block 0xFF is the system/OTP block.
 */
#ifndef CM_ST25TB_H
#define CM_ST25TB_H

#include "cardmodel.h"

typedef struct {
    uint8_t uid[8];    /* as the poller keeps it: uid[0] = 0xD0 (ST) */
    uint8_t chip_id;
    uint16_t blocks;
    uint32_t blk[128];
    uint32_t system;
    int selected, done, lost;
    unsigned reads, writes;
} cm_st25tb_t;

/* type_bits: the poller's uid[2] >> 2 (7 = ST25TB04K, 128 blocks; 6 =
 * ST25TB512-AC, 16 blocks). */
static void cm_st25tb_init(cm_st25tb_t* k, uint8_t type_bits, uint16_t blocks) {
    static const uint8_t uid[8] = {0xD0, 0x02, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    memset(k, 0, sizeof *k);
    memcpy(k->uid, uid, 8);
    k->uid[2] = (uint8_t)(type_bits << 2);
    k->chip_id = 0x5A;
    k->blocks = blocks;
    for(unsigned i = 0; i < blocks; i++) k->blk[i] = 0x01020304u * (i + 1);
    k->system = 0xFFFFFFFFu;
}

static int cm_st25tb_xfer(void* ctx, const uint8_t* tx, size_t bits, uint8_t* rx, size_t room,
                          size_t* rxb, uint32_t fwt, uint32_t flags) {
    cm_st25tb_t* k = ctx;
    (void)fwt;
    (void)flags;
    size_t n = bits / 8, out = 0;
    if(k->lost || bits % 8 || !cm_crc_ok(tx, n, CM_CRC_B)) return -1;
    n -= 2;
    if(n == 2 && tx[0] == 0x06 && tx[1] == 0x00) {
        if(k->done) return -1;
        rx[0] = k->chip_id;
        out = 1;
    } else if(n == 2 && tx[0] == 0x0E) {
        if(k->done || tx[1] != k->chip_id) return -1;
        k->selected = 1;
        rx[0] = k->chip_id;
        out = 1;
    } else if(!k->selected) {
        return -1;
    } else if(n == 1 && tx[0] == 0x0B) {
        for(int i = 0; i < 8; i++) rx[i] = k->uid[7 - i];
        out = 8;
    } else if(n == 2 && tx[0] == 0x08) {
        uint32_t v;
        if(tx[1] == 0xFF) v = k->system;
        else if(tx[1] < k->blocks) v = k->blk[tx[1]];
        else return -1;
        k->reads++;
        rx[0] = v & 0xff;
        rx[1] = (v >> 8) & 0xff;
        rx[2] = (v >> 16) & 0xff;
        rx[3] = v >> 24;
        out = 4;
    } else if(n == 6 && tx[0] == 0x09) {
        if(tx[1] < k->blocks)
            k->blk[tx[1]] = tx[2] | (uint32_t)tx[3] << 8 | (uint32_t)tx[4] << 16 | (uint32_t)tx[5] << 24;
        k->writes++;
        return -1; /* no answer to a write */
    } else if(n == 1 && tx[0] == 0x0F) {
        k->selected = 0;
        k->done = 1;
        return -1;
    } else
        return -1;
    if(8 * (out + 2) > room) return -5;
    *rxb = 8 * cm_crc_add(rx, out, CM_CRC_B);
    return 0;
}

#endif
