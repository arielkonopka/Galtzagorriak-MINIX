/* SPDX-License-Identifier: GPL-3.0-or-later
 * liblfrfid (spec R9.0.1, R9.0.3): the stock read worker's decision, for
 * MINIX.  Unleashed lib/lfrfid/lfrfid_worker_modes.c feeds every decoder of
 * one feature (ASK or PSK) each level and duration, and accepts a protocol
 * only when it decoded the same bytes validate_count + 1 times in a row
 * (after every decode all decoders start over).  The worker thread, the
 * timer capture and the varint buffer are not ported: the caller feeds the
 * edges it has (a /dev/rfid capture in `rfid`, a synthetic one in
 * make lfrfidtest), so both run exactly this. */
#ifndef LFRFID_READ_H
#define LFRFID_READ_H

#include <toolbox/protocols/protocol_dict.h>
#include <lfrfid/protocols/lfrfid_protocols.h>

#define LFRFID_DATA_MAX 32 /* > the largest protocol (Indala224: 28 B) */

typedef struct {
    ProtocolDict* dict;
    uint32_t feature; /* LFRFIDFeatureASK or LFRFIDFeaturePSK */
    ProtocolId last; /* the protocol seen last, PROTOCOL_NO for none */
    size_t count; /* how often in a row, after the first */
    uint8_t last_data[LFRFID_DATA_MAX];
    uint8_t data[LFRFID_DATA_MAX];
} LfrfidReader;

/* All 25 Unleashed protocols, in the stock order. */
ProtocolDict* lfrfid_dict_alloc(void);

void lfrfid_reader_start(LfrfidReader* r, ProtocolDict* dict, uint32_t feature);

/* One level (true: the antenna loaded / comparator high) and its length in
 * us.  Returns the protocol once it is validated, else PROTOCOL_NO.  Its
 * bytes are then in r->data; the decoders have started over, so put them
 * back with protocol_dict_set_data() before rendering them. */
ProtocolId lfrfid_reader_feed(LfrfidReader* r, bool level, uint32_t duration);

/* A gap in the edges (the next /dev/rfid capture): the decoders start over,
 * the count of equal reads goes on, as the stock worker's 2 s window. */
void lfrfid_reader_gap(LfrfidReader* r);

/* Feed n durations whose levels alternate, the first `first_level`.
 * Returns the first protocol validated (r->data), else PROTOCOL_NO. */
ProtocolId lfrfid_reader_feed_edges(
    LfrfidReader* r,
    const uint32_t* duration,
    size_t n,
    bool first_level);

/* Make data as the protocol's writer and a real tag would send it (parity,
 * fixed bits): Unleashed's write_data corrects the data it is given (FDX-A
 * parity, FDX-B by re-decoding...); emulation and `.rfid` data go through
 * it too.  Returns false if the protocol cannot be written to a T5577. */
bool lfrfid_normalize(ProtocolDict* dict, ProtocolId p, LFRFIDWriteRequest* req);

/* Emulation (R9.0.3): one frame of protocol p's encoder as RFIDIOCEMU
 * segments, as the stock emulation's pulse_glue makes them: a segment is
 * a loaded part and the unloaded rest, dur[i] = its length in carrier
 * periods - 1, pulse[i] = the loaded part.  The frame is the period of the
 * encoder's endless output, checked over a whole second frame, and starts
 * where a loaded part does, so the list repeats seamlessly.  Returns the
 * number of segments, -1 if more than max (or no period was found). */
int lfrfid_emu_segments(
    ProtocolId p,
    const uint8_t* data,
    size_t size,
    uint32_t* dur,
    uint32_t* pulse,
    int max);

#endif
