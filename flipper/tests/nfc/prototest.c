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

#include <nfc/nfc.h>
#include <nfc/nfc_poller.h>
#include <nfc/protocols/iso14443_4a/iso14443_4a_poller.h>
#include <toolbox/bit_buffer.h>

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

int main(void) {
    printf("libnfc vendored-poller tests (#404)\n");
    test_iso14443_4a();
    if(fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("all vendored-poller tests passed\n");
    return 0;
}
