/* p5test.c - NFC part 5 (#412): the vendored Ultralight/NTAG, FeliCa,
 * ST25TB and EMV pollers over the libnfc shim, against software cards.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14.  Run by `make test` under
 * ASan/UBSan.  One section per protocol.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nfc_shim.h"
#include "mf_ultralight.h" /* libnfc's own reader (#387) */
#include "cm_mful.h"
#include "cm_felica.h"
#include "cm_st25tb.h"

#include <nfc/nfc.h>
#include <nfc/nfc_poller.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller_sync.h>
#include <nfc/protocols/felica/felica_poller.h>
#include <nfc/protocols/felica/felica_poller_sync.h>
#include <nfc/protocols/st25tb/st25tb_poller.h>
#include <nfc/protocols/st25tb/st25tb_poller_sync.h>

_Static_assert(sizeof(MfUltralightConfigPages) == 16, "config pages: 4 pages, 16 bytes");

static int fails;
#define CHECK(c, msg)                         \
    do {                                      \
        if(!(c)) {                            \
            printf("FAIL: %s\n", msg);        \
            fails++;                          \
        } else                                \
            printf("ok   %s\n", msg);         \
    } while(0)

/* ---- MIFARE Ultralight / NTAG ---- */

typedef struct {
    int success, failed;
} mful_ctx_t;

static NfcCommand mful_cb(NfcGenericEvent event, void* context) {
    mful_ctx_t* c = context;
    const MfUltralightPollerEvent* e = event.event_data;
    if(event.protocol != NfcProtocolMfUltralight) return NfcCommandContinue;
    if(e->type == MfUltralightPollerEventTypeReadSuccess) {
        c->success++;
        return NfcCommandStop;
    }
    if(e->type == MfUltralightPollerEventTypeReadFailed) {
        c->failed++;
        return NfcCommandStop;
    }
    if(e->type == MfUltralightPollerEventTypeAuthRequest) e->data->auth_context.skip_auth = true;
    return NfcCommandContinue;
}

static void test_mful(void) {
    static cm_mful_t k;
    nfc_xfer_t x = {0};
    mful_ctx_t c = {0};

    printf("-- MIFARE Ultralight / NTAG\n");
    cm_mful_ntag215(&k);
    x.transceive = cm_mful_xfer;
    x.ctx = &k;

    Nfc* nfc = nfc_alloc_xfer(&x);
    NfcPoller* poller = nfc_poller_alloc(nfc, NfcProtocolMfUltralight);
    nfc_poller_start(poller, mful_cb, &c);
    nfc_poller_stop(poller);
    const MfUltralightData* d = (const MfUltralightData*)nfc_poller_get_data(poller);
    CHECK(c.success == 1 && c.failed == 0, "UL: vendored poller reads the NTAG215");
    CHECK(d->type == MfUltralightTypeNTAG215, "UL: GET_VERSION -> NTAG215");
    CHECK(d->pages_total == 135, "UL: 135 pages in all");
    /* PWD/PACK are not counted when the default password fails. */
    CHECK(d->pages_read == 133, "UL: 133 pages read (PWD/PACK not readable)");
    int same = 1;
    for(int p = 0; p < 133; p++)
        if(memcmp(d->page[p].data, k.mem[p], 4)) same = 0;
    CHECK(same, "UL: page contents equal the card's");
    CHECK(!memcmp(d->signature.data, k.sig, 32), "UL: READ_SIG, 32 bytes");
    CHECK(d->counter[2].counter == 0x123, "UL: READ_CNT 2");
    CHECK(k.pwd_tries == 1, "UL: default password tried once (AUTHLIM 0)");
    size_t ul = 0;
    const uint8_t* uid = mf_ultralight_get_uid(d, &ul);
    CHECK(ul == 7 && !memcmp(uid, k.a.uid, 7), "UL: 7-byte UID");

    /* The sync layer on the same card. */
    MfUltralightPage pg;
    CHECK(mf_ultralight_poller_sync_read_page(nfc, 10, &pg) == MfUltralightErrorNone &&
              !memcmp(pg.data, k.mem[10], 4),
          "UL sync: read_page 10");
    MfUltralightVersion v;
    CHECK(mf_ultralight_poller_sync_read_version(nfc, &v) == MfUltralightErrorNone &&
              !memcmp(&v, k.version, 8),
          "UL sync: read_version");
    MfUltralightData* sd = mf_ultralight_alloc();
    CHECK(mf_ultralight_poller_sync_read_card(nfc, sd, NULL) == MfUltralightErrorNone &&
              sd->type == MfUltralightTypeNTAG215 && sd->pages_read >= 133 &&
              !memcmp(sd->page[50].data, k.mem[50], 4),
          "UL sync: read_card");
    mf_ultralight_free(sd);
    nfc_poller_free(poller);

    /* With the right password the poller reads PWD/PACK too. */
    cm_mful_ntag215(&k);
    k.pwd[0] = k.pwd[1] = k.pwd[2] = k.pwd[3] = 0xFF; /* the default */
    memset(&c, 0, sizeof c);
    poller = nfc_poller_alloc(nfc, NfcProtocolMfUltralight);
    nfc_poller_start(poller, mful_cb, &c);
    nfc_poller_stop(poller);
    d = (const MfUltralightData*)nfc_poller_get_data(poller);
    CHECK(c.success == 1 && d->pages_read == 135, "UL: default password -> all 135 pages");
    CHECK(d->page[134].data[0] == 0x80 && d->page[134].data[1] == 0x80, "UL: PACK from PWD_AUTH");
    nfc_poller_free(poller);
    nfc_free(nfc);

    /* libnfc's hand-written reader (#387) on the same model. */
    cm_mful_ntag215(&k);
    k.a.active = 1; /* selected by the caller's 3A layer */
    const mful_type_t* t = mful_get_version(&x);
    CHECK(t && t->total_pages == 135, "UL (libnfc reader): GET_VERSION -> 135 pages");
    static uint8_t pages[MFUL_MAX_PAGES][MFUL_PAGE_SIZE];
    uint16_t got = 0;
    int r = mful_read_card(&x, 135, pages, &got);
    CHECK(r == 0 && got == 135, "UL (libnfc reader): 135 pages");
    CHECK(!memcmp(pages[40], k.mem[40], 4) && !memcmp(pages[3], k.mem[3], 4),
          "UL (libnfc reader): same contents as the vendored poller");

    /* No card. */
    {
        nfc_xfer_t nx = {0}; /* no transceiver: nothing answers */
        Nfc* n2 = nfc_alloc_xfer(&nx);
        memset(&c, 0, sizeof c);
        NfcPoller* p2 = nfc_poller_alloc(n2, NfcProtocolMfUltralight);
        nfc_poller_start(p2, mful_cb, &c);
        nfc_poller_stop(p2);
        CHECK(c.success == 0, "UL: no card, no read, no hang");
        nfc_poller_free(p2);
        nfc_free(n2);
    }
}

/* ---- FeliCa (Lite-S) ---- */

typedef struct {
    int ready, incomplete, error, auth_req;
} felica_ctx_t;

static NfcCommand felica_cb(NfcGenericEvent event, void* context) {
    felica_ctx_t* c = context;
    const FelicaPollerEvent* e = event.event_data;
    if(event.protocol != NfcProtocolFelica) return NfcCommandContinue;
    switch(e->type) {
    case FelicaPollerEventTypeRequestAuthContext:
        c->auth_req++;
        e->data->auth_context->skip_auth = true; /* no card key here */
        return NfcCommandContinue;
    case FelicaPollerEventTypeReady: c->ready++; break;
    case FelicaPollerEventTypeIncomplete: c->incomplete++; break;
    default: c->error++; break;
    }
    return NfcCommandStop;
}

static void test_felica(void) {
    static cm_felica_t k;
    nfc_xfer_t x = {0};
    felica_ctx_t c = {0};

    printf("-- FeliCa\n");
    cm_felica_lite_s(&k);
    x.transceive = cm_felica_xfer;
    x.ctx = &k;
    Nfc* nfc = nfc_alloc_xfer(&x);
    NfcPoller* poller = nfc_poller_alloc(nfc, NfcProtocolFelica);
    nfc_poller_start(poller, felica_cb, &c);
    nfc_poller_stop(poller);
    const FelicaData* d = (const FelicaData*)nfc_poller_get_data(poller);
    CHECK(c.auth_req == 1 && c.error == 0, "FeliCa: polled, auth context asked once");
    /* Without the card key the poller reports the Lite-S read incomplete. */
    CHECK(c.incomplete == 1 && c.ready == 0, "FeliCa: Lite-S read, incomplete without CK");
    CHECK(d->workflow_type == FelicaLite, "FeliCa: PMm F1 -> Lite workflow");
    CHECK(!memcmp(d->idm.data, k.idm, 8) && !memcmp(d->pmm.data, k.pmm, 8), "FeliCa: IDm, PMm");
    CHECK(d->blocks_total == 28, "FeliCa: all 28 Lite-S blocks asked");
    CHECK(!memcmp(d->data.fs.spad[5].data, k.blk[5], 16) && d->data.fs.spad[5].SF1 == 0,
          "FeliCa: S_PAD5 contents");
    CHECK(!memcmp(d->data.fs.id.data, k.idm, 8), "FeliCa: ID block");
    CHECK(d->data.fs.ck.SF1 == 0x01 && d->data.fs.ck.SF2 == 0xA8, "FeliCa: CK unreadable (01 A8)");
    nfc_poller_free(poller);

    FelicaData* sd = felica_alloc();
    FelicaError se = felica_poller_sync_read(nfc, sd, NULL);
    CHECK((se == FelicaErrorNone || se == FelicaErrorNotPresent) && !memcmp(sd->idm.data, k.idm, 8),
          "FeliCa sync: read gives IDm");
    felica_free(sd);
    nfc_free(nfc);

    k.lost = 1;
    nfc = nfc_alloc_xfer(&x);
    memset(&c, 0, sizeof c);
    poller = nfc_poller_alloc(nfc, NfcProtocolFelica);
    nfc_poller_start(poller, felica_cb, &c);
    nfc_poller_stop(poller);
    CHECK(c.ready == 0 && c.incomplete == 0, "FeliCa: no card, no read, no hang");
    nfc_poller_free(poller);
    nfc_free(nfc);
}

/* ---- ST25TB ---- */

typedef struct {
    int ready, success, failure;
    St25tbType type;
} st25tb_ctx_t;

static NfcCommand st25tb_cb(NfcGenericEvent event, void* context) {
    st25tb_ctx_t* c = context;
    const St25tbPollerEvent* e = event.event_data;
    if(event.protocol != NfcProtocolSt25tb) return NfcCommandContinue;
    switch(e->type) {
    case St25tbPollerEventTypeReady:
        c->ready++;
        c->type = e->data->ready.type;
        return NfcCommandContinue;
    case St25tbPollerEventTypeRequestMode:
        e->data->mode_request.mode = St25tbPollerModeRead;
        return NfcCommandContinue;
    case St25tbPollerEventTypeSuccess: c->success++; break;
    default: c->failure++; break;
    }
    return NfcCommandStop;
}

static void test_st25tb(void) {
    static cm_st25tb_t k;
    nfc_xfer_t x = {0};
    st25tb_ctx_t c = {0};

    printf("-- ST25TB\n");
    cm_st25tb_init(&k, 7, 128); /* ST25TB04K */
    x.transceive = cm_st25tb_xfer;
    x.ctx = &k;
    Nfc* nfc = nfc_alloc_xfer(&x);
    NfcPoller* poller = nfc_poller_alloc(nfc, NfcProtocolSt25tb);
    nfc_poller_start(poller, st25tb_cb, &c);
    nfc_poller_stop(poller);
    const St25tbData* d = (const St25tbData*)nfc_poller_get_data(poller);
    CHECK(c.ready == 1 && c.type == St25tbType04k, "ST25TB: INITIATE, SELECT, GET_UID -> 04K");
    CHECK(c.success == 1 && c.failure == 0, "ST25TB: read in Read mode");
    CHECK(!memcmp(d->uid, k.uid, 8), "ST25TB: UID byte order (D0 first)");
    int same = 1;
    for(int i = 0; i < 128; i++)
        if(d->blocks[i] != k.blk[i]) same = 0;
    CHECK(same && k.reads == 129, "ST25TB: 128 blocks + system block, little-endian");
    CHECK(d->system_otp_block == 0xFFFFFFFFu, "ST25TB: system/OTP block");
    nfc_poller_free(poller);

    uint32_t v = 0;
    St25tbType t = St25tbTypeNum;
    CHECK(st25tb_poller_sync_detect_type(nfc, &t) == St25tbErrorNone && t == St25tbType04k,
          "ST25TB sync: detect_type");
    CHECK(st25tb_poller_sync_read_block(nfc, 9, &v) == St25tbErrorNone && v == k.blk[9],
          "ST25TB sync: read_block 9");
    St25tbData* sd = st25tb_alloc();
    CHECK(st25tb_poller_sync_read(nfc, sd) == St25tbErrorNone && sd->blocks[100] == k.blk[100],
          "ST25TB sync: read");
    st25tb_free(sd);
    nfc_free(nfc);

    cm_st25tb_init(&k, 6, 16); /* ST25TB512-AC */
    nfc = nfc_alloc_xfer(&x);
    memset(&c, 0, sizeof c);
    poller = nfc_poller_alloc(nfc, NfcProtocolSt25tb);
    nfc_poller_start(poller, st25tb_cb, &c);
    nfc_poller_stop(poller);
    CHECK(c.success == 1 && c.type == St25tbType512Ac && k.reads == 17, "ST25TB: 512-AC, 16 blocks");
    nfc_poller_free(poller);

    k.lost = 1;
    memset(&c, 0, sizeof c);
    poller = nfc_poller_alloc(nfc, NfcProtocolSt25tb);
    nfc_poller_start(poller, st25tb_cb, &c);
    nfc_poller_stop(poller);
    CHECK(c.success == 0, "ST25TB: no card, no read, no hang");
    nfc_poller_free(poller);
    nfc_free(nfc);
}

int main(void) {
    test_mful();
    test_felica();
    test_st25tb();
    if(fails) {
        printf("%d part-5 test(s) FAILED\n", fails);
        return 1;
    }
    printf("all part-5 poller tests passed\n");
    return 0;
}
