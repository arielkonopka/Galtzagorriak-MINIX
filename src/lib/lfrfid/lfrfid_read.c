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

/* The load as the Flipper's pull pin drives it reaches a reader's envelope
 * detector about 2 carrier periods late on its falling side: measured with
 * the reference Flipper (Unleashed rfid raw_read), a 4-of-8 load showed as
 * 6 of 8 and the 4 periods unloaded of an RF/8 FSK cycle vanished, so no
 * FSK frame read.  Each loaded part is cut by that much (never below 1 and
 * never a segment loaded for all of it, a deliberate long load): FSK and
 * PSK now read, ASK keeps working (2 of 32 is noise to it). */
#define EMU_TRIM 2

static uint32_t emu_trim(uint32_t dur, uint32_t pul) {
    if(pul == 0 || pul > dur) return pul; /* none, or all of the segment */
    return pul > EMU_TRIM ? pul - EMU_TRIM : 1;
}

/* Does the waveform repeat after L runs?  Checked for EMU_FRAMES whole
 * repeats, and for at least EMU_MINY runs, which is longer than any
 * constant stretch of a frame (a preamble of equal cycles repeats after a
 * cycle for all of its length), at most MAXY: an encoder is a state machine
 * over its frame, so a shift that holds for several frames holds for good.
 * Checking MAXY runs for every candidate made a frame with many candidates
 * take seconds on the device (Pyramid 13.8 s). */
#define EMU_FRAMES 4
#define EMU_MINY 8192

static bool emu_period(const ProtocolBase* b, const uint8_t* data, size_t size, long L) {
    EmuRun x, y;
    long i, lim = EMU_FRAMES * L;

    if(lim < EMU_MINY) lim = EMU_MINY;
    if(lim > MAXY) lim = MAXY;
    emu_new(&x, b, data, size, WARM);
    emu_new(&y, b, data, size, WARM + L);
    for(i = 0; i < lim; i++)
        if(emu_run(&x) != emu_run(&y)) break;
    b->free(x.e);
    b->free(y.e);
    return i == lim;
}

/* The repeat's length in runs: the first shift L at which the waveform
 * repeats (emu_period).  A candidate is a shift at which the last HASHW
 * runs hash as the first HASHW do: a rolling hash, kept by a second encoder
 * HASHW runs behind the first (no buffer), so FSK frames, whose runs take
 * only a few values, give few false candidates (48 runs compared gave
 * Pyramid 1130 of them, each checked from the start: 13.8 s on the device).
 * -1: none. */
#define HASHW 512
#define HASHB 0x9E3779B1u

static long emu_len(const ProtocolBase* b, const uint8_t* data, size_t size) {
    uint32_t sig = 0, h = 0, bw = 1, w;
    long L = -1, i;
    EmuRun r, t;

    emu_new(&r, b, data, size, WARM);
    emu_new(&t, b, data, size, WARM);
    for(i = 1; i < HASHW; i++)
        bw *= HASHB; /* HASHB^(HASHW-1) */
    for(i = 0; i < HASHW; i++)
        sig = sig * HASHB + emu_run(&r);
    h = sig;
    for(i = HASHW; i < MAXY && L < 0; i++) {
        w = emu_run(&r);
        h = (h - emu_run(&t) * bw) * HASHB + w;
        if(h == sig && emu_period(b, data, size, i - HASHW + 1)) L = i - HASHW + 1;
    }
    b->free(r.e);
    b->free(t.e);
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

        pul = emu_trim(dur, pul);
        if(n > 0 && runs[4 * (n - 1)] == dur && runs[4 * (n - 1) + 1] == pul &&
           runs[4 * (n - 1) + 2] < 0xFFFF) {
            runs[4 * (n - 1) + 2]++;
        } else {
            if(n >= max || dur > 0xFFFF) {
                n = -2;
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
    uint32_t w;
    long L, i;
    int nseg = 0;
    EmuRun r;

    L = emu_len(b, data, size);
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
        pulse[nseg] = emu_trim(dur[nseg], w >> 1);
        nseg++;
        w = emu_run(&r);
    }
    b->free(r.e);
    return nseg;
}
