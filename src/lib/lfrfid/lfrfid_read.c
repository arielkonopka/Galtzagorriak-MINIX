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
