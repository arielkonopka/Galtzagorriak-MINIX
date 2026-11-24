/* cm_t4.h - a software ISO 14443-4A card with ISO 7816-4 applications:
 * an NFC Forum Type 4 Tag (NDEF application, CC file E103, NDEF file
 * E104) plus a few other applications that only answer SELECT, for the
 * Type 4 Tag reader and the service scanner tests (#451).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #451.
 *
 * Every command is counted by its INS, so a test can prove the scanner
 * sent SELECT and nothing else.  The AIDs are the public ones of the
 * example dictionary; nothing here is a real card's data.
 */
#ifndef CM_T4_H
#define CM_T4_H

#include "cardmodel.h"

#define CM_T4_APPS 4
typedef struct {
    struct {
        uint8_t aid[16], len;
        uint16_t sw;      /* its SELECT answer */
        uint8_t fci_len;  /* FCI bytes before the SW (0x6F...) */
    } app[CM_T4_APPS];
    int napps;
    int sel;              /* selected application, -1 none */
    uint16_t file;        /* selected EF */
    uint8_t cc[15];
    uint8_t ndef[200];    /* NLEN + message */
    size_t ndef_size;
    unsigned ins[256];    /* commands seen, by INS */
} cm_t4_t;

static size_t cm_t4_sw(uint8_t* r, size_t n, uint16_t sw) {
    r[n] = (uint8_t)(sw >> 8);
    r[n + 1] = (uint8_t)sw;
    return n + 2;
}

static size_t cm_t4_app(void* ctx, const uint8_t* c, size_t n, uint8_t* r) {
    cm_t4_t* k = ctx;
    if(n < 4) return cm_t4_sw(r, 0, 0x6700);
    k->ins[c[1]]++;
    if(c[0] != 0x00) return cm_t4_sw(r, 0, 0x6E00);
    if(c[1] == 0xA4 && c[2] == 0x04 && n >= 5 && n >= 5u + c[4]) {
        for(int i = 0; i < k->napps; i++)
            if(c[4] == k->app[i].len && !memcmp(c + 5, k->app[i].aid, c[4])) {
                size_t m = 0;
                if(k->app[i].sw == 0x9000 || (k->app[i].sw >> 8) == 0x61) k->sel = i;
                if(k->app[i].fci_len) { /* 6F L 84 len AID, padded with A5 */
                    r[0] = 0x6F;
                    r[1] = (uint8_t)(k->app[i].fci_len - 2);
                    r[2] = 0x84;
                    r[3] = k->app[i].len;
                    memcpy(r + 4, k->app[i].aid, k->app[i].len);
                    m = 4 + k->app[i].len;
                    while(m < k->app[i].fci_len) r[m++] = 0xA5;
                }
                k->file = 0;
                return cm_t4_sw(r, m, k->app[i].sw);
            }
        k->sel = -1;
        return cm_t4_sw(r, 0, 0x6A82);
    }
    if(c[1] == 0xA4 && c[2] == 0x00 && n >= 7 && c[4] == 2) {
        uint16_t fid = (uint16_t)(c[5] << 8 | c[6]);
        if(k->sel != 0 || (fid != 0xE103 && fid != 0xE104)) return cm_t4_sw(r, 0, 0x6A82);
        k->file = fid;
        return cm_t4_sw(r, 0, 0x9000);
    }
    if(c[1] == 0xB0 && n >= 5) {
        const uint8_t* f = k->file == 0xE103 ? k->cc : k->file == 0xE104 ? k->ndef : NULL;
        size_t fs = k->file == 0xE103 ? sizeof k->cc : k->ndef_size;
        size_t off = (size_t)(c[2] << 8 | c[3]), le = c[4] ? c[4] : 256;
        if(!f) return cm_t4_sw(r, 0, 0x6986);
        if(off >= fs) return cm_t4_sw(r, 0, 0x6B00);
        if(le > fs - off) le = fs - off;
        memcpy(r, f + off, le);
        return cm_t4_sw(r, le, 0x9000);
    }
    return cm_t4_sw(r, 0, 0x6D00);
}

/* The card: NDEF app first, a URI record "https://example.org/t4t". */
static void cm_t4_init(cm_t4_t* k, cm_4a_t* c) {
    static const uint8_t ndef_aid[7] = {0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01};
    static const uint8_t msg[] = {0xD1, 0x01, 0x0E, 'U', 0x04, 'e', 'x', 'a', 'm', 'p',
                                  'l', 'e', '.', 'o', 'r', 'g', '/', 't'};
    memset(k, 0, sizeof *k);
    memset(c, 0, sizeof *c);
    k->sel = -1;
    memcpy(k->app[0].aid, ndef_aid, 7);
    k->app[0].len = 7;
    k->app[0].sw = 0x9000;
    k->napps = 1;
    /* CC: CCLEN 15, v2.0, MLe 0x3B, MLc 0x34, NDEF TLV fid E104 max 200 */
    static const uint8_t cc[15] = {0x00, 0x0F, 0x20, 0x00, 0x3B, 0x00, 0x34, 0x04,
                                   0x06, 0xE1, 0x04, 0x00, 0xC8, 0x00, 0x00};
    memcpy(k->cc, cc, sizeof cc);
    k->ndef[0] = 0;
    k->ndef[1] = sizeof msg;
    memcpy(k->ndef + 2, msg, sizeof msg);
    k->ndef_size = sizeof k->ndef;
    /* 7-byte UID, ATQA 0344, SAK 20; ATS: FSCI 5 (64), FWI 7 */
    static const uint8_t uid[7] = {0x04, 0x51, 0x7A, 0x22, 0x31, 0x6E, 0x80};
    memcpy(c->a.uid, uid, 7);
    c->a.uid_len = 7;
    c->a.atqa[0] = 0x44;
    c->a.atqa[1] = 0x03;
    c->a.sak = 0x20;
    static const uint8_t ats[] = {0x06, 0x75, 0x77, 0x71, 0x02, 0x80};
    memcpy(c->ats, ats, sizeof ats);
    c->dep.app = cm_t4_app;
    c->dep.app_ctx = k;
}

static void cm_t4_add(cm_t4_t* k, const uint8_t* aid, uint8_t len, uint16_t sw, uint8_t fci) {
    memcpy(k->app[k->napps].aid, aid, len);
    k->app[k->napps].len = len;
    k->app[k->napps].sw = sw;
    k->app[k->napps].fci_len = fci;
    k->napps++;
}

#endif
