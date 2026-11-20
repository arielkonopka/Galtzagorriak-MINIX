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

#include <nfc/nfc.h>
#include <nfc/nfc_poller.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller_sync.h>

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

int main(void) {
    test_mful();
    if(fails) {
        printf("%d part-5 test(s) FAILED\n", fails);
        return 1;
    }
    printf("all part-5 poller tests passed\n");
    return 0;
}
