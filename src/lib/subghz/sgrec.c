/* SPDX-License-Identifier: GPL-3.0-or-later
 * sgrec - the Read RAW recorder's core (issue #419); see sgrec.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sgrec.h"

void sgrec_init(SgRec* r, int (*out)(void*, const char*, int), void* ctx,
                unsigned long max, unsigned long long max_us) {
    memset(r, 0, sizeof(*r));
    r->out = out;
    r->ctx = ctx;
    r->max = max;
    r->max_us = max_us;
}

static void flush(SgRec* r) {
    if(r->len > 0 && !r->err && r->out(r->ctx, r->buf, r->len) != r->len) r->err = 1;
    r->len = 0;
}

static void text(SgRec* r, const char* s) {
    int n = (int)strlen(s);

    if(r->len + n > SGREC_BUF) flush(r);
    memcpy(r->buf + r->len, s, n);
    r->len += n;
}

void sgrec_header(SgRec* r, unsigned long hz) {
    char t[24];

    text(r, "Filetype: Flipper SubGhz RAW File\nVersion: 1\nFrequency: ");
    snprintf(t, sizeof(t), "%lu\n", hz);
    text(r, t);
    text(r, "Preset: FuriHalSubGhzPresetOok650Async\nProtocol: RAW\n");
}

static void value(SgRec* r, long v) {
    char t[16];

    if(r->vals == 0) text(r, "RAW_Data:");
    snprintf(t, sizeof(t), " %ld", v);
    text(r, t);
    if(++r->vals == SGREC_LINE) {
        text(r, "\n");
        r->vals = 0;
    }
    r->n++;
}

static int done(SgRec* r) {
    return r->err || (r->max && r->n >= r->max) || (r->max_us && r->us >= r->max_us);
}

int sgrec_put(SgRec* r, bool level, uint32_t us) {
    if(done(r)) return 1;
    if(us == 0) return 0;
    r->us += us;
    if(r->on && (int)level == r->level) {
        r->dur += us;
        return done(r);
    }
    if(r->on) value(r, r->level ? (long)r->dur : -(long)r->dur);
    r->on = 1;
    r->level = level;
    r->dur = us;
    return done(r);
}

int sgrec_end(SgRec* r) {
    if(r->on && !(r->max && r->n >= r->max)) value(r, r->level ? (long)r->dur : -(long)r->dur);
    r->on = 0;
    if(r->vals > 0) text(r, "\n");
    r->vals = 0;
    flush(r);
    return r->err ? -1 : 0;
}

int sgrec_rssi(const char* line, int* dbm) {
    const char* p;

    if((p = strstr(line, "peak=")) != NULL)
        p += 5;
    else if((p = strstr(line, "rssi=")) != NULL)
        p += 5;
    else
        return 0;
    *dbm = atoi(p);
    return 1;
}
