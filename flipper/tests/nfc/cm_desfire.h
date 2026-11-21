/* cm_desfire.h - a software MIFARE DESFire EV1 (the application layer of
 * a cm_4a_t card), for the vendored DESFire poller test (#404).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Only the commands stock's reader sends without keys: GetVersion (three
 * frames, 0xAF chaining), GetFreeMemory, GetKeySettings, GetKeyVersion,
 * GetApplicationIDs, SelectApplication, GetFileIDs, GetFileSettings,
 * ReadData, GetValue.  Native (plain) and ISO 7816-wrapped (90 INS 00 00
 * Lc data 00 -> data 91 SW) framing.  One application 0x563412 with
 *   file 1: standard data, 32 bytes, free read (access EEEE);
 *   file 2: value file, value 100, free read;
 *   file 3: standard data, 16 bytes, read with key 0 only (not read).
 * If iso_only, plain frames get 6700 (a DESFire Light), so the poller's
 * fallback to the wrapped mode is tested too.
 */
#ifndef CM_DESFIRE_H
#define CM_DESFIRE_H

#include "cardmodel.h"

typedef struct {
    uint8_t uid[7];
    int iso_only;
    int app_selected;
    const uint8_t* more; /* the rest of a chained answer */
    size_t more_len;
    uint8_t chain[28];
    uint8_t file1[32];
    unsigned reads, refused;
} cm_desfire_t;

/* native command -> status + data (data first in resp+1) */
static size_t cm_df_native(cm_desfire_t* k, const uint8_t* c, size_t n, uint8_t* r) {
    uint8_t* d = r + 1;
    size_t len = 0;
    r[0] = 0x00;
    if(n == 0) { r[0] = 0x7E; return 1; }
    switch(c[0]) {
    case 0x60: { /* GetVersion: 7 + 7 + 14 */
        static const uint8_t hw[7] = {0x04, 0x01, 0x01, 0x01, 0x00, 0x18, 0x05};
        static const uint8_t sw[7] = {0x04, 0x01, 0x01, 0x01, 0x04, 0x18, 0x05};
        memcpy(k->chain, sw, 7);
        memcpy(k->chain + 7, k->uid, 7);
        memcpy(k->chain + 14, "\xBA\x55\x00\x11\x22\x15\x23", 7);
        memcpy(d, hw, 7);
        k->more = k->chain;
        k->more_len = 21;
        r[0] = 0xAF;
        return 8;
    }
    case 0xAF:
        if(!k->more) { r[0] = 0x1C; return 1; }
        len = k->more_len > 7 ? 7 : k->more_len;
        memcpy(d, k->more, len);
        k->more += len;
        k->more_len -= len;
        r[0] = k->more_len ? 0xAF : 0x00;
        if(!k->more_len) k->more = NULL;
        return 1 + len;
    case 0x6E: memcpy(d, "\x00\x10\x00", 3); return 4;          /* 4096 B free */
    case 0x45:                                                     /* GetKeySettings */
        d[0] = 0x0F;                                               /* free dir list etc. */
        d[1] = k->app_selected ? 0x82 : 0x01;                      /* AES, 2 keys / 1 key */
        return 3;
    case 0x64: if(n < 2) { r[0] = 0x7E; return 1; } d[0] = 0x00; return 2;
    case 0x6A: if(k->app_selected) { r[0] = 0x1C; return 1; } memcpy(d, "\x56\x34\x12", 3); return 4;
    case 0x5A:
        if(n != 4) { r[0] = 0x7E; return 1; }
        if(!memcmp(c + 1, "\x00\x00\x00", 3)) { k->app_selected = 0; return 1; }
        if(memcmp(c + 1, "\x56\x34\x12", 3)) { r[0] = 0xA0; return 1; }
        k->app_selected = 1;
        return 1;
    case 0x6F: if(!k->app_selected) { r[0] = 0x1C; return 1; } memcpy(d, "\x01\x02\x03", 3); return 4;
    case 0xF5:
        if(n != 2 || !k->app_selected) { r[0] = 0x9D; return 1; }
        if(c[1] == 1) { memcpy(d, "\x00\x00\xEE\xEE\x20\x00\x00", 7); return 8; }
        if(c[1] == 2) {
            memcpy(d, "\x02\x00\xEE\xEE\x00\x00\x00\x00\xE8\x03\x00\x00\x00\x00\x00\x00\x00", 17);
            return 18;
        }
        if(c[1] == 3) { memcpy(d, "\x00\x00\x00\x00\x10\x00\x00", 7); return 8; }
        r[0] = 0xF0;
        return 1;
    case 0xBD: {
        if(n != 8 || c[1] != 1) { k->refused++; r[0] = 0xAE; return 1; }
        size_t off = c[2] | c[3] << 8 | (size_t)c[4] << 16, l = c[5] | c[6] << 8 | (size_t)c[7] << 16;
        if(off + l > sizeof k->file1 || l > 59) { r[0] = 0xBE; return 1; }
        memcpy(d, k->file1 + off, l);
        k->reads++;
        return 1 + l;
    }
    case 0x6C:
        if(n != 2 || c[1] != 2) { k->refused++; r[0] = 0xAE; return 1; }
        memcpy(d, "\x64\x00\x00\x00", 4);
        return 5;
    }
    r[0] = 0x1C;
    return 1;
}

static size_t cm_df_app(void* ctx, const uint8_t* c, size_t n, uint8_t* resp) {
    cm_desfire_t* k = ctx;
    uint8_t tmp[64];
    if(n >= 5 && c[0] == 0x90 && c[2] == 0 && c[3] == 0) { /* ISO-wrapped */
        uint8_t cmd[64];
        size_t cn = 1;
        cmd[0] = c[1];
        if(n > 5) {
            size_t lc = c[4];
            if(5 + lc + 1 != n || lc > sizeof cmd - 1) { memcpy(resp, "\x67\x00", 2); return 2; }
            memcpy(cmd + 1, c + 5, lc);
            cn += lc;
        }
        size_t rn = cm_df_native(k, cmd, cn, tmp);
        memcpy(resp, tmp + 1, rn - 1);
        resp[rn - 1] = 0x91;
        resp[rn] = tmp[0];
        return rn + 1;
    }
    if(k->iso_only) { memcpy(resp, "\x67\x00", 2); return 2; }
    return cm_df_native(k, c, n, resp);
}

#endif
