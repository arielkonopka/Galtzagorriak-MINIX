/* cm_mfplus.h - a software MIFARE Plus EV1 (ISO 14443-4A) for the
 * vendored MIFARE Plus poller tests (#451): it answers the NXP native
 * GetVersion (0x60, then 0xAF twice) and the WritePerso security-level
 * probe stock sends to a block that does not exist (0xA8 90 90: rejected,
 * nothing is written; status 0x09 in SL0, 0x06 in SL3).  Anything else:
 * status 0x0B (command not allowed).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #451.
 *
 * The version bytes follow NXP AN10833's layout (vendor 04, type 02 =
 * MIFARE Plus, major 0x11 = EV1, storage 0x16 = 2 KB); UID and batch are
 * made up.
 */
#ifndef CM_MFPLUS_H
#define CM_MFPLUS_H

#include "cardmodel.h"

typedef struct {
    uint8_t version[28];
    int part;            /* GetVersion frames sent */
    int sl;              /* 0 or 3 */
    unsigned perso_probes, others;
} cm_mfplus_t;

static size_t cm_mfplus_app(void* ctx, const uint8_t* c, size_t n, uint8_t* r) {
    cm_mfplus_t* k = ctx;
    if(n >= 1 && c[0] == 0x60) {
        k->part = 1;
        r[0] = 0xAF;
        memcpy(r + 1, k->version, 7);
        return 8;
    }
    if(n >= 1 && c[0] == 0xAF && k->part == 1) {
        k->part = 2;
        r[0] = 0xAF;
        memcpy(r + 1, k->version + 7, 7);
        return 8;
    }
    if(n >= 1 && c[0] == 0xAF && k->part == 2) {
        k->part = 0;
        r[0] = 0x90; /* a Plus ends GetVersion with a non-zero status */
        memcpy(r + 1, k->version + 14, 14);
        return 15;
    }
    if(n == 19 && c[0] == 0xA8 && c[1] == 0x90 && c[2] == 0x90) {
        k->perso_probes++;
        r[0] = k->sl == 0 ? 0x09 : 0x06;
        return 1;
    }
    k->others++;
    r[0] = 0x0B;
    return 1;
}

static void cm_mfplus_init(cm_mfplus_t* k, cm_4a_t* c, int sl) {
    static const uint8_t v[28] = {
        0x04, 0x02, 0x01, 0x11, 0x00, 0x16, 0x05,   /* hw: NXP, Plus, EV1, 2K */
        0x04, 0x02, 0x01, 0x11, 0x00, 0x16, 0x05,   /* sw */
        0x04, 0x6B, 0x21, 0x5A, 0x33, 0x18, 0x80,   /* UID */
        0xBA, 0x7C, 0x40, 0x11, 0x22, 0x30, 0x21};  /* batch, week, year */
    memset(k, 0, sizeof *k);
    memset(c, 0, sizeof *c);
    memcpy(k->version, v, sizeof v);
    k->sl = sl;
    static const uint8_t uid[7] = {0x04, 0x6B, 0x21, 0x5A, 0x33, 0x18, 0x80};
    memcpy(c->a.uid, uid, 7);
    c->a.uid_len = 7;
    c->a.atqa[0] = 0x44;
    c->a.sak = 0x20;
    static const uint8_t ats[] = {0x0C, 0x75, 0x77, 0x80, 0x02, 0xC1, 0x05, 0x2F, 0x2F, 0x01, 0xBC, 0xD6};
    memcpy(c->ats, ats, sizeof ats);
    c->dep.app = cm_mfplus_app;
    c->dep.app_ctx = k;
}

#endif
