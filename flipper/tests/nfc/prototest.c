/* prototest.c - the vendored Unleashed pollers (src/lib/nfc) over the
 * libnfc shim and nfc_xfer, against software cards (issue #404).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14.  Run by `make test` (tests/host
 * run-nfc) under ASan/UBSan.  One section per protocol.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nfc_shim.h"
#include "cardmodel.h"
#include "cm_desfire.h"
#include "cm_15693.h"

#include <nfc/nfc.h>
#include <nfc/nfc_poller.h>
#include <nfc/protocols/iso14443_4a/iso14443_4a_poller.h>
#include <nfc/protocols/iso14443_4b/iso14443_4b_poller.h>
#include <nfc/protocols/mf_desfire/mf_desfire_poller.h>
#include <nfc/protocols/iso15693_3/iso15693_3_poller.h>
#include <nfc/protocols/slix/slix_poller.h>
#include <toolbox/bit_buffer.h>
#include <toolbox/simple_array.h>

static int fails;
#define CHECK(c, msg)                         \
    do {                                      \
        if(!(c)) {                            \
            printf("FAIL: %s\n", msg);        \
            fails++;                          \
        } else                                \
            printf("ok   %s\n", msg);         \
    } while(0)

/* ---- ISO 14443-4A ---- */

/* The card's application: SELECT (00 A4) -> 90 00, anything else is
 * echoed back with 90 00 appended. */
static size_t app_echo(void* ctx, const uint8_t* cmd, size_t n, uint8_t* resp) {
    (void)ctx;
    if(n >= 2 && cmd[0] == 0x00 && cmd[1] == 0xA4) {
        resp[0] = 0x90;
        resp[1] = 0x00;
        return 2;
    }
    memcpy(resp, cmd, n);
    resp[n] = 0x90;
    resp[n + 1] = 0x00;
    return n + 2;
}

typedef struct {
    int ready, errors;
    uint8_t resp[64];
    size_t resp_len;
    int err2;
    uint8_t resp2[64];
    size_t resp2_len;
} t4a_ctx_t;

static NfcCommand t4a_cb(NfcGenericEvent event, void* context) {
    t4a_ctx_t* c = context;
    const Iso14443_4aPollerEvent* e = event.event_data;
    if(event.protocol != NfcProtocolIso14443_4a) return NfcCommandContinue;
    if(e->type != Iso14443_4aPollerEventTypeReady) {
        c->errors++;
        return NfcCommandStop;
    }
    c->ready++;
    BitBuffer* tx = bit_buffer_alloc(64);
    BitBuffer* rx = bit_buffer_alloc(64);
    static const uint8_t sel[] = {0x00, 0xA4, 0x04, 0x00, 0x02, 0x3F, 0x00};
    bit_buffer_copy_bytes(tx, sel, sizeof sel);
    if(iso14443_4a_poller_send_block(event.instance, tx, rx) == Iso14443_4aErrorNone) {
        c->resp_len = bit_buffer_get_size_bytes(rx);
        bit_buffer_write_bytes(rx, c->resp, c->resp_len);
    }
    static const uint8_t echo[] = {0x80, 0xCA, 0x9F, 0x7F, 0x00};
    bit_buffer_copy_bytes(tx, echo, sizeof echo);
    c->err2 = iso14443_4a_poller_send_block(event.instance, tx, rx);
    if(c->err2 == Iso14443_4aErrorNone) {
        c->resp2_len = bit_buffer_get_size_bytes(rx);
        bit_buffer_write_bytes(rx, c->resp2, c->resp2_len);
    }
    bit_buffer_free(tx);
    bit_buffer_free(rx);
    return NfcCommandStop;
}

static void cm_4a_init(cm_4a_t* k) {
    memset(k, 0, sizeof *k);
    k->a = (cm_3a_t){{0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66}, 7, {0x44, 0x03}, 0x20, 0, 0};
    /* TL=7, T0=0x78 (FSCI 8, TA TB TC), TA=0x80, TB=0x70 (FWI 7), TC=0x02,
     * historical bytes 0x80 0x31 */
    static const uint8_t ats[] = {0x07, 0x78, 0x80, 0x70, 0x02, 0x80, 0x31};
    memcpy(k->ats, ats, sizeof ats);
}

static void test_iso14443_4a(void) {
    static cm_4a_t k;
    nfc_xfer_t x = {0};
    t4a_ctx_t c;

    printf("-- ISO 14443-4A\n");
    cm_4a_init(&k);
    k.dep.app = app_echo;
    k.dep.wtx_left = 1; /* the card asks for more time once */
    x.transceive = cm_4a_xfer;
    x.ctx = &k;

    Nfc* nfc = nfc_alloc_xfer(&x);
    NfcPoller* poller = nfc_poller_alloc(nfc, NfcProtocolIso14443_4a);
    memset(&c, 0, sizeof c);
    nfc_poller_start(poller, t4a_cb, &c);
    nfc_poller_stop(poller);

    CHECK(c.ready == 1 && c.errors == 0, "4A: activated (REQA, cascade 2, SELECT, RATS)");
    CHECK(k.rats_seen == 1, "4A: one RATS");
    const Iso14443_4aData* d = (const Iso14443_4aData*)nfc_poller_get_data(poller);
    size_t uid_len = 0;
    const uint8_t* uid = iso14443_4a_get_uid(d, &uid_len);
    CHECK(uid_len == 7 && memcmp(uid, k.a.uid, 7) == 0, "4A: 7-byte UID");
    uint32_t hb = 0;
    const uint8_t* h = iso14443_4a_get_historical_bytes(d, &hb);
    CHECK(hb == 2 && h[0] == 0x80 && h[1] == 0x31, "4A: ATS historical bytes");
    CHECK(d->ats_data.tb_1 == 0x70 && d->ats_data.tc_1 == 0x02, "4A: ATS TB(1), TC(1)");
    CHECK(c.resp_len == 2 && c.resp[0] == 0x90 && c.resp[1] == 0x00, "4A: SELECT -> 90 00 after S(WTX)");
    CHECK(k.dep.wtx_seen == 1, "4A: S(WTX) answered by the reader");
    CHECK(c.err2 == Iso14443_4aErrorNone && c.resp2_len == 7 && c.resp2[0] == 0x80 &&
              c.resp2[5] == 0x90,
          "4A: second I-block (block number toggled) echoed");
    CHECK(k.dep.iblocks == 2, "4A: two I-blocks");
    nfc_poller_free(poller);

    /* No card: the poller gives up after max_steps, no hang. */
    {
        static cm_4a_t none;
        nfc_xfer_t nx = {0};
        memset(&none, 0, sizeof none);
        none.a.halted = 1; /* never answers REQA */
        nx.transceive = cm_4a_xfer;
        nx.ctx = &none;
        Nfc* n2 = nfc_alloc_xfer(&nx);
        nfc_shim_set_max_steps(n2, 8);
        NfcPoller* p2 = nfc_poller_alloc(n2, NfcProtocolIso14443_4a);
        memset(&c, 0, sizeof c);
        nfc_poller_start(p2, t4a_cb, &c);
        nfc_poller_stop(p2);
        CHECK(c.ready == 0 && nfc_shim_frames(n2) > 0, "4A: no card -> stops after max_steps");
        nfc_poller_free(p2);
        nfc_free(n2);
    }
    nfc_free(nfc);
}

/* ---- ISO 14443-4B ---- */

static NfcCommand t4b_cb(NfcGenericEvent event, void* context) {
    t4a_ctx_t* c = context;
    const Iso14443_4bPollerEvent* e = event.event_data;
    if(event.protocol != NfcProtocolIso14443_4b) return NfcCommandContinue;
    if(e->type != Iso14443_4bPollerEventTypeReady) {
        c->errors++;
        return NfcCommandStop;
    }
    c->ready++;
    BitBuffer* tx = bit_buffer_alloc(64);
    BitBuffer* rx = bit_buffer_alloc(64);
    static const uint8_t apdu[] = {0x00, 0xB0, 0x00, 0x00, 0x04};
    bit_buffer_copy_bytes(tx, apdu, sizeof apdu);
    c->err2 = iso14443_4b_poller_send_block(event.instance, tx, rx);
    if(c->err2 == Iso14443_4bErrorNone) {
        c->resp_len = bit_buffer_get_size_bytes(rx);
        bit_buffer_write_bytes(rx, c->resp, c->resp_len);
    }
    bit_buffer_free(tx);
    bit_buffer_free(rx);
    return NfcCommandStop;
}

static void test_iso14443_4b(void) {
    static cm_4b_t k;
    nfc_xfer_t x = {0};
    t4a_ctx_t c;

    printf("-- ISO 14443-4B\n");
    memset(&k, 0, sizeof k);
    memcpy(k.pupi, "\x1A\x2B\x3C\x4D", 4);
    memcpy(k.app_data, "\x00\x00\x00\x00", 4);
    /* 106 kbit/s only; FSCI 8 (256), ISO 14443-4; FWI 7, ADC 0, FO 0 */
    memcpy(k.proto_info, "\x00\x81\x70", 3);
    k.dep.app = app_echo;
    x.transceive = cm_4b_xfer;
    x.ctx = &k;

    Nfc* nfc = nfc_alloc_xfer(&x);
    NfcPoller* poller = nfc_poller_alloc(nfc, NfcProtocolIso14443_4b);
    memset(&c, 0, sizeof c);
    nfc_poller_start(poller, t4b_cb, &c);
    nfc_poller_stop(poller);
    CHECK(c.ready == 1 && k.reqb_seen >= 1 && k.attrib_seen == 1, "4B: REQB, ATTRIB, ready");
    const Iso14443_4bData* d = (const Iso14443_4bData*)nfc_poller_get_data(poller);
    size_t uid_len = 0;
    const uint8_t* uid = iso14443_4b_get_uid(d, &uid_len);
    CHECK(uid_len == 4 && memcmp(uid, k.pupi, 4) == 0, "4B: PUPI as UID");
    CHECK(c.err2 == Iso14443_4bErrorNone && c.resp_len == 7 && c.resp[0] == 0x00 &&
              c.resp[1] == 0xB0 && c.resp[5] == 0x90,
          "4B: I-block answered");
    nfc_poller_free(poller);

    /* Stock's 4B poller (unlike 4A) does not answer S(WTX): the exchange
     * ends with a protocol error, not a hang or a wrong answer. */
    memset(&k.dep, 0, sizeof k.dep);
    k.halted = 0;
    k.dep.app = app_echo;
    k.dep.wtx_left = 1;
    poller = nfc_poller_alloc(nfc, NfcProtocolIso14443_4b);
    memset(&c, 0, sizeof c);
    nfc_poller_start(poller, t4b_cb, &c);
    nfc_poller_stop(poller);
    CHECK(c.ready == 1 && c.err2 == Iso14443_4bErrorProtocol && c.resp_len == 0,
          "4B: S(WTX) -> protocol error (stock behaviour)");
    nfc_poller_free(poller);
    nfc_free(nfc);
}

/* ---- MIFARE DESFire (vendored poller on 4A) ---- */

typedef struct {
    int success, failed;
} tdf_ctx_t;

static NfcCommand tdf_cb(NfcGenericEvent event, void* context) {
    tdf_ctx_t* c = context;
    const MfDesfirePollerEvent* e = event.event_data;
    if(event.protocol != NfcProtocolMfDesfire) return NfcCommandContinue;
    if(e->type == MfDesfirePollerEventTypeReadSuccess) c->success++;
    else if(e->type == MfDesfirePollerEventTypeReadFailed) c->failed++;
    else return NfcCommandContinue;
    return NfcCommandStop;
}

static void desfire_read(cm_desfire_t* df, const char* tag) {
    static cm_4a_t k;
    nfc_xfer_t x = {0};
    tdf_ctx_t c = {0};
    char msg[96];

    cm_4a_init(&k);
    memcpy(k.a.uid, df->uid, 7);
    k.dep.app = cm_df_app;
    k.dep.app_ctx = df;
    x.transceive = cm_4a_xfer;
    x.ctx = &k;
    Nfc* nfc = nfc_alloc_xfer(&x);
    nfc_shim_set_max_steps(nfc, 256);
    NfcPoller* poller = nfc_poller_alloc(nfc, NfcProtocolMfDesfire);
    nfc_poller_start(poller, tdf_cb, &c);
    nfc_poller_stop(poller);

    snprintf(msg, sizeof msg, "DESFire %s: read success", tag);
    CHECK(c.success == 1 && c.failed == 0, msg);
    const MfDesfireData* d = (const MfDesfireData*)nfc_poller_get_data(poller);
    snprintf(msg, sizeof msg, "DESFire %s: version (hw EV1, 4K) and UID", tag);
    CHECK(d->version.hw_type == 0x01 && d->version.hw_storage == 0x18 && d->version.sw_major == 0x01 &&
              !memcmp(d->version.uid, df->uid, 7) && d->version.prod_year == 0x23,
          msg);
    snprintf(msg, sizeof msg, "DESFire %s: free memory 4096", tag);
    CHECK(d->free_memory.bytes_free == 4096, msg);
    snprintf(msg, sizeof msg, "DESFire %s: one application 563412", tag);
    CHECK(simple_array_get_count(d->application_ids) == 1 &&
              !memcmp(((const MfDesfireApplicationId*)simple_array_cget(d->application_ids, 0))->data,
                      "\x56\x34\x12", 3),
          msg);
    if(simple_array_get_count(d->applications) == 1) {
        const MfDesfireApplication* app = simple_array_cget(d->applications, 0);
        snprintf(msg, sizeof msg, "DESFire %s: three files with settings", tag);
        CHECK(simple_array_get_count(app->file_ids) == 3 &&
                  simple_array_get_count(app->file_settings) == 3,
              msg);
        const MfDesfireFileData* f1 = simple_array_cget(app->file_data, 0);
        const MfDesfireFileData* f2 = simple_array_cget(app->file_data, 1);
        const MfDesfireFileData* f3 = simple_array_cget(app->file_data, 2);
        snprintf(msg, sizeof msg, "DESFire %s: free file 1 read (32 bytes)", tag);
        CHECK(simple_array_get_count(f1->data) == 32 &&
                  !memcmp(simple_array_cget_data(f1->data), df->file1, 32),
              msg);
        snprintf(msg, sizeof msg, "DESFire %s: value file 2 = 100", tag);
        CHECK(simple_array_get_count(f2->data) == 4 &&
                  ((const uint8_t*)simple_array_cget_data(f2->data))[0] == 100,
              msg);
        snprintf(msg, sizeof msg, "DESFire %s: keyed file 3 not read, no command sent", tag);
        CHECK(simple_array_get_count(f3->data) == 0 && df->refused == 0, msg);
    } else {
        snprintf(msg, sizeof msg, "DESFire %s: application read", tag);
        CHECK(0, msg);
    }
    nfc_poller_free(poller);
    nfc_free(nfc);
}

static void test_desfire(void) {
    static cm_desfire_t df;
    printf("-- MIFARE DESFire\n");
    memset(&df, 0, sizeof df);
    memcpy(df.uid, "\x04\x5A\x6B\x7C\x8D\x9E\xAF", 7);
    for(int i = 0; i < 32; i++) df.file1[i] = (uint8_t)(0xA0 + i);
    desfire_read(&df, "native");
    memset(&df, 0, sizeof df);
    memcpy(df.uid, "\x04\x01\x02\x03\x04\x05\x06", 7);
    for(int i = 0; i < 32; i++) df.file1[i] = (uint8_t)(3 * i);
    df.iso_only = 1;
    desfire_read(&df, "ISO-wrapped fallback");
}

/* ---- ISO 15693 / SLIX ---- */

typedef struct {
    int ready, errors, privacy_asked;
} tv_ctx_t;

static NfcCommand tv_cb(NfcGenericEvent event, void* context) {
    tv_ctx_t* c = context;
    if(event.protocol == NfcProtocolIso15693_3) {
        const Iso15693_3PollerEvent* e = event.event_data;
        if(e->type == Iso15693_3PollerEventTypeReady) c->ready++;
        else c->errors++;
        return NfcCommandStop;
    }
    if(event.protocol == NfcProtocolSlix) {
        const SlixPollerEvent* e = event.event_data;
        if(e->type == SlixPollerEventTypePrivacyUnlockRequest) {
            c->privacy_asked++; /* no password given: read on as is */
            return NfcCommandContinue;
        }
        if(e->type == SlixPollerEventTypeReady) c->ready++;
        else c->errors++;
        return NfcCommandStop;
    }
    return NfcCommandContinue;
}

static void test_iso15693(void) {
    static cm_15693_t k;
    nfc_xfer_t x = {0};
    tv_ctx_t c;

    printf("-- ISO 15693 / SLIX\n");
    cm_15693_slix2(&k);
    x.transceive = cm_15693_xfer;
    x.ctx = &k;
    Nfc* nfc = nfc_alloc_xfer(&x);

    NfcPoller* poller = nfc_poller_alloc(nfc, NfcProtocolIso15693_3);
    memset(&c, 0, sizeof c);
    nfc_poller_start(poller, tv_cb, &c);
    nfc_poller_stop(poller);
    CHECK(c.ready == 1 && c.errors == 0, "15693: inventory + system info + read");
    const Iso15693_3Data* d = (const Iso15693_3Data*)nfc_poller_get_data(poller);
    size_t ul = 0;
    const uint8_t* uid = iso15693_3_get_uid(d, &ul);
    CHECK(ul == 8 && uid[0] == 0xE0 && uid[1] == 0x04 && uid[7] == 0x78, "15693: UID (MSB first)");
    CHECK(d->system_info.block_count == 80 && d->system_info.block_size == 4 &&
              d->system_info.ic_ref == 0x01,
          "15693: 80 blocks of 4 bytes");
    CHECK(k.reads == 80 && !memcmp(simple_array_cget_data(d->block_data), k.mem, 320),
          "15693: all 80 blocks read");
    CHECK(((const uint8_t*)simple_array_cget_data(d->block_security))[3] == 0x01,
          "15693: block 3 locked");
    nfc_poller_free(poller);

    poller = nfc_poller_alloc(nfc, NfcProtocolSlix);
    memset(&c, 0, sizeof c);
    k.reads = 0;
    nfc_poller_start(poller, tv_cb, &c);
    nfc_poller_stop(poller);
    CHECK(c.ready == 1 && c.errors == 0, "SLIX: read on 15693");
    const SlixData* sd = (const SlixData*)nfc_poller_get_data(poller);
    CHECK(slix_get_type(sd) == SlixTypeSlix2, "SLIX: identified as SLIX2 from the UID");
    CHECK(k.custom == 2 && !memcmp(sd->signature, k.signature, 32),
          "SLIX: NXP system info + originality signature read");
    CHECK(c.privacy_asked == 1, "SLIX: privacy password asked once, none given");
    CHECK(slix_get_counter(sd) == (uint16_t)(k.mem[79 * 4] | k.mem[79 * 4 + 1] << 8),
          "SLIX: counter block 79");
    nfc_poller_free(poller);

    /* A non-NXP 15693 tag is no SLIX. */
    k.uid[6] = 0x07; /* TI */
    k.nxp = 0;
    poller = nfc_poller_alloc(nfc, NfcProtocolSlix);
    memset(&c, 0, sizeof c);
    nfc_poller_start(poller, tv_cb, &c);
    nfc_poller_stop(poller);
    CHECK(c.ready == 0 && c.errors >= 1, "SLIX: TI tag refused");
    nfc_poller_free(poller);
    nfc_free(nfc);
}

int main(void) {
    printf("libnfc vendored-poller tests (#404)\n");
    test_iso14443_4a();
    test_iso14443_4b();
    test_desfire();
    test_iso15693();
    if(fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("all vendored-poller tests passed\n");
    return 0;
}
