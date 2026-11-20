/* cm_emv.h - a software EMV contactless card for the tests (#412).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #412.
 *
 * An ISO 14443-4A card (cardmodel.h) whose application answers the EMV
 * Book 1/3 commands the vendored poller sends: SELECT 2PAY.SYS.DDF01
 * (PPSE) -> FCI with one directory entry; SELECT AID -> FCI with the
 * label; GET PROCESSING OPTIONS -> format 2 (77: AIP, AFL = SFI 2 record
 * 1); READ RECORD SFI 2 rec 1 -> 70 (PAN, expiry, name); GET DATA 9F17 ->
 * PIN try counter 3, 9F36 ATC; anything else -> 6A 82.  All values are
 * synthetic (PAN 4000 0000 0000 0002, a test number); nothing is a real
 * card's, and nothing is written anywhere.
 *
 * SFI 2, not the usual 1: stock's emv_poller_read_afl() marks records
 * in a mask at bit record + (sfi - 2) * 8, a negative shift for SFI 1
 * (undefined; UBSan stops the test).  See src/lib/libnfc/README.
 */
#ifndef CM_EMV_H
#define CM_EMV_H

#include "cardmodel.h"

static const uint8_t cm_emv_aid[7] = {0xA0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10};

typedef struct {
    unsigned selects, gpo, records, get_data;
} cm_emv_t;

static size_t cm_emv_ok(uint8_t* resp, const uint8_t* d, size_t n) {
    memcpy(resp, d, n);
    resp[n] = 0x90;
    resp[n + 1] = 0x00;
    return n + 2;
}

static size_t cm_emv_app(void* ctx, const uint8_t* c, size_t n, uint8_t* resp) {
    cm_emv_t* k = ctx;
    if(n >= 5 && c[0] == 0x00 && c[1] == 0xA4 && c[2] == 0x04) {
        k->selects++;
        if(c[4] == 14 && n >= 19 && !memcmp(c + 5, "2PAY.SYS.DDF01", 14)) {
            static const uint8_t fci[] = {
                0x6F, 0x2C, 0x84, 0x0E, '2', 'P', 'A', 'Y', '.', 'S', 'Y', 'S', '.', 'D', 'D', 'F',
                '0', '1', 0xA5, 0x1A, 0xBF, 0x0C, 0x17, 0x61, 0x15, 0x4F, 0x07, 0xA0, 0x00, 0x00,
                0x00, 0x03, 0x10, 0x10, 0x50, 0x07, 'T', 'E', 'S', 'T', 'A', 'P', 'P', 0x87,
                0x01, 0x01};
            return cm_emv_ok(resp, fci, sizeof fci);
        }
        if(c[4] == 7 && n >= 12 && !memcmp(c + 5, cm_emv_aid, 7)) {
            static const uint8_t fci[] = {
                0x6F, 0x17, 0x84, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10, 0xA5, 0x0C,
                0x50, 0x07, 'T', 'E', 'S', 'T', 'A', 'P', 'P', 0x87, 0x01, 0x01};
            return cm_emv_ok(resp, fci, sizeof fci);
        }
    } else if(n >= 5 && c[0] == 0x80 && c[1] == 0xA8) {
        k->gpo++;
        static const uint8_t gpo[] = {0x77, 0x0A, 0x82, 0x02, 0x20, 0x00, 0x94, 0x04, 0x10, 0x01, 0x01, 0x00};
        return cm_emv_ok(resp, gpo, sizeof gpo);
    } else if(n >= 4 && c[0] == 0x00 && c[1] == 0xB2) {
        k->records++;
        if(c[2] == 1 && c[3] == ((2 << 3) | 4)) {
            static const uint8_t rec[] = {
                0x70, 0x1C, 0x5A, 0x08, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x5F,
                0x24, 0x03, 0x30, 0x12, 0x31, 0x5F, 0x20, 0x09, 'T', 'E', 'S', 'T', '/', 'C',
                'A', 'R', 'D'};
            return cm_emv_ok(resp, rec, sizeof rec);
        }
        resp[0] = 0x6A;
        resp[1] = 0x83;
        return 2;
    } else if(n >= 4 && c[0] == 0x80 && c[1] == 0xCA) {
        k->get_data++;
        if(c[2] == 0x9F && c[3] == 0x17) {
            static const uint8_t ptc[] = {0x9F, 0x17, 0x01, 0x03};
            return cm_emv_ok(resp, ptc, sizeof ptc);
        }
        if(c[2] == 0x9F && c[3] == 0x36) {
            static const uint8_t atc[] = {0x9F, 0x36, 0x02, 0x00, 0x2A};
            return cm_emv_ok(resp, atc, sizeof atc);
        }
    }
    resp[0] = 0x6A;
    resp[1] = 0x82;
    return 2;
}

#endif
