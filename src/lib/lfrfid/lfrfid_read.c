/* SPDX-License-Identifier: GPL-3.0-or-later
 * liblfrfid: the stock read worker's decision.  See lfrfid_read.h. */
#include <furi.h>
#include "lfrfid_read.h"

ProtocolDict* lfrfid_dict_alloc(void) {
    return protocol_dict_alloc(lfrfid_protocols, LFRFIDProtocolMax);
}

void lfrfid_reader_start(LfrfidReader* r, ProtocolDict* dict, uint32_t feature) {
    furi_check(protocol_dict_get_max_data_size(dict) <= LFRFID_DATA_MAX);
    r->dict = dict;
    r->feature = feature;
    r->last = PROTOCOL_NO;
    r->count = 0;
    protocol_dict_decoders_start(dict);
}

ProtocolId lfrfid_reader_feed(LfrfidReader* r, bool level, uint32_t duration) {
    ProtocolId p = protocol_dict_decoders_feed_by_feature(r->dict, r->feature, level, duration);
    size_t n;

    if(p == PROTOCOL_NO) return PROTOCOL_NO;
    n = protocol_dict_get_data_size(r->dict, p);
    protocol_dict_get_data(r->dict, p, r->data, n);
    protocol_dict_decoders_start(r->dict);
    if(p == r->last && memcmp(r->last_data, r->data, n) == 0) {
        if(++r->count >= protocol_dict_get_validate_count(r->dict, p)) {
            r->last = PROTOCOL_NO;
            r->count = 0;
            return p;
        }
    } else {
        r->last = p;
        memcpy(r->last_data, r->data, n);
        r->count = 0;
    }
    return PROTOCOL_NO;
}

void lfrfid_reader_gap(LfrfidReader* r) {
    protocol_dict_decoders_start(r->dict);
}

ProtocolId lfrfid_reader_feed_edges(
    LfrfidReader* r,
    const uint32_t* duration,
    size_t n,
    bool first_level) {
    size_t i;

    for(i = 0; i < n; i++) {
        ProtocolId p = lfrfid_reader_feed(r, first_level ^ (i & 1), duration[i]);
        if(p != PROTOCOL_NO) return p;
    }
    return PROTOCOL_NO;
}

bool lfrfid_normalize(ProtocolDict* dict, ProtocolId p, LFRFIDWriteRequest* req) {
    memset(req, 0, sizeof(*req));
    req->write_type = LFRFIDWriteTypeT5577;
    return protocol_dict_get_write_data(dict, p, req);
}

#define SIGW 48 /* runs that mark a frame's start */
#define MAXY 40000 /* longest repeat looked for, in runs */
#define WARM 4 /* runs skipped first: an encoder's first run can differ from
                  the same place a frame later (PAC/Stanley splits a run at
                  its start, the FSK oscillator starts at phase 0) */

/* The waveform as runs: an encoder whose equal levels in a row are merged.
 * A run is its duration << 1 | its level. */
typedef struct {
    const ProtocolBase* b;
    void* e;
    uint32_t next; /* the level read ahead, 0 if none */
} EmuRun;

static uint32_t emu_yield(EmuRun* r) {
    LevelDuration l = r->b->encoder.yield(r->e);
    return level_duration_get_duration(l) << 1 | level_duration_get_level(l);
}

static uint32_t emu_run(EmuRun* r) {
    uint32_t w = r->next ? r->next : emu_yield(r), x;

    for(;;) {
        x = emu_yield(r);
        if((x & 1) != (w & 1) || (x >> 1) == 0) break;
        w += x & ~1u;
    }
    r->next = x;
    return w;
}

/* A fresh encoder, `skip` runs into its output: Unleashed's encoder_start
 * builds the frame but not all of them reset the bit position (Viking),
 * and a new (zeroed) instance has it at 0. */
static void emu_new(EmuRun* r, const ProtocolBase* b, const uint8_t* data, size_t size, long skip) {
    r->b = b;
    r->e = b->alloc();
    r->next = 0;
    memcpy(b->get_data(r->e), data, size);
    b->encoder.start(r->e);
    while(skip-- > 0)
        (void)emu_run(r);
}

/* Does the waveform repeat after L runs, for MAXY runs (more than any
 * frame)? */
static bool emu_period(const ProtocolBase* b, const uint8_t* data, size_t size, long L) {
    EmuRun x, y;
    long i;

    emu_new(&x, b, data, size, WARM);
    emu_new(&y, b, data, size, WARM + L);
    for(i = 0; i < MAXY; i++)
        if(emu_run(&x) != emu_run(&y)) break;
    b->free(x.e);
    b->free(y.e);
    return i == MAXY;
}

/* The repeat's length in runs: the first shift at which the first SIGW
 * runs come again and the whole waveform repeats (a preamble's constant
 * part repeats its start sooner: those fail the second test).  -1: none. */
static long emu_len(const ProtocolBase* b, const uint8_t* data, size_t size) {
    uint32_t sig[SIGW], ring[SIGW], w;
    long L = -1, i, k;
    EmuRun r;

    emu_new(&r, b, data, size, WARM);
    for(i = 0; i < SIGW; i++)
        sig[i] = ring[i] = emu_run(&r);
    for(i = SIGW; i < MAXY && L < 0; i++) {
        ring[i % SIGW] = w = emu_run(&r);
        if(w != sig[SIGW - 1]) continue;
        for(k = 0; k < SIGW; k++)
            if(ring[(i - SIGW + 1 + k) % SIGW] != sig[k]) break;
        if(k == SIGW && emu_period(b, data, size, i - SIGW + 1)) L = i - SIGW + 1;
    }
    b->free(r.e);
    return L;
}

int lfrfid_emu_runs(
    ProtocolId p,
    const uint8_t* data,
    size_t size,
    uint16_t* runs,
    int max) {
    const ProtocolBase* b = lfrfid_protocols[p];
    long L = emu_len(b, data, size), i;
    int n = 0;
    uint32_t w;
    EmuRun r;

    if(L < 2) return -1;
    emu_new(&r, b, data, size, WARM);
    w = emu_run(&r);
    if(!(w & 1)) w = emu_run(&r);
    for(i = 0; i < L; i += 2) {
        uint32_t lo = emu_run(&r), dur = (w >> 1) + (lo >> 1) - 1, pul = w >> 1;

        if(n > 0 && runs[4 * (n - 1)] == dur && runs[4 * (n - 1) + 1] == pul &&
           runs[4 * (n - 1) + 2] < 0xFFFF) {
            runs[4 * (n - 1) + 2]++;
        } else {
            if(n >= max || dur > 0xFFFF) {
                n = -1;
                break;
            }
            runs[4 * n] = (uint16_t)dur;
            runs[4 * n + 1] = (uint16_t)pul;
            runs[4 * n + 2] = 1;
            runs[4 * n + 3] = 0;
            n++;
        }
        w = emu_run(&r);
    }
    b->free(r.e);
    return n;
}

int lfrfid_emu_segments(
    ProtocolId p,
    const uint8_t* data,
    size_t size,
    uint32_t* dur,
    uint32_t* pulse,
    int max) {
    const ProtocolBase* b = lfrfid_protocols[p];
    uint32_t sig[SIGW], ring[SIGW], w;
    long L = -1, i, k;
    int nseg = 0;
    EmuRun r;

    /* the repeat's length in runs: the first shift at which the first SIGW
     * runs come again and the whole waveform repeats (a preamble's
     * constant part repeats its start sooner: those fail the second test) */
    emu_new(&r, b, data, size, WARM);
    for(i = 0; i < SIGW; i++)
        sig[i] = ring[i] = emu_run(&r);
    for(i = SIGW; i < MAXY && L < 0; i++) {
        ring[i % SIGW] = w = emu_run(&r);
        if(w != sig[SIGW - 1]) continue;
        for(k = 0; k < SIGW; k++)
            if(ring[(i - SIGW + 1 + k) % SIGW] != sig[k]) break;
        if(k == SIGW && emu_period(b, data, size, i - SIGW + 1)) L = i - SIGW + 1;
    }
    b->free(r.e);
    if(L < 0 || L < 2) return -1; /* none, or never loaded and unloaded */
    /* L runs from a loaded one: segments of a loaded run and the unloaded
     * one after it, round to the start again */
    emu_new(&r, b, data, size, WARM);
    w = emu_run(&r);
    if(!(w & 1)) w = emu_run(&r);
    for(i = 0; i < L; i += 2) {
        uint32_t lo = emu_run(&r);

        if(nseg >= max) {
            nseg = -1;
            break;
        }
        dur[nseg] = (w >> 1) + (lo >> 1) - 1;
        pulse[nseg++] = w >> 1;
        w = emu_run(&r);
    }
    b->free(r.e);
    return nseg;
}
