/* p7test.c - NFC part 7 (#451): the vendored Type 4 Tag and MIFARE Plus
 * pollers over the libnfc shim, against software cards, and known-answer
 * tests of the MIFARE Plus AES layer (now on the real Mbed TLS AES, #447).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14.  Run by `make test` under
 * ASan/UBSan.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nfc_shim.h"
#include "cm_t4.h"
#include "cm_mfplus.h"

#include <nfc/nfc.h>
#include <nfc/nfc_poller.h>
#include <nfc/protocols/type_4_tag/type_4_tag_poller.h>
#include <nfc/protocols/mf_plus/mf_plus_poller.h>
#include <nfc/protocols/mf_plus/mf_plus_i.h>
#include <nfc/protocols/mf_plus/mf_plus_crypto.h>
#include <nfc/protocols/iso14443_4a/iso14443_4a.h>

static int fails;
#define CHECK(c, msg)                         \
    do {                                      \
        if(!(c)) {                            \
            printf("FAIL: %s\n", msg);        \
            fails++;                          \
        } else                                \
            printf("ok   %s\n", msg);         \
    } while(0)

static void unhex(const char* s, uint8_t* out) {
    for(size_t i = 0; s[2 * i]; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

/* ---- Type 4 Tag ---- */

typedef struct {
    int success, failed, mode_asked;
} t4_ctx_t;

static NfcCommand t4_cb(NfcGenericEvent event, void* context) {
    t4_ctx_t* c = context;
    const Type4TagPollerEvent* e = event.event_data;
    if(event.protocol != NfcProtocolType4Tag) return NfcCommandContinue;
    if(e->type == Type4TagPollerEventTypeRequestMode) {
        c->mode_asked++; /* leave the default: Read */
        return NfcCommandContinue;
    }
    if(e->type == Type4TagPollerEventTypeReadSuccess) c->success++;
    else c->failed++;
    return NfcCommandStop;
}

static void test_t4t(void) {
    static cm_4a_t k;
    static cm_t4_t app;
    nfc_xfer_t x = {0};
    t4_ctx_t c = {0};

    printf("-- Type 4 Tag (vendored poller)\n");
    cm_t4_init(&app, &k);
    x.transceive = cm_4a_xfer;
    x.ctx = &k;
    Nfc* nfc = nfc_alloc_xfer(&x);
    NfcPoller* poller = nfc_poller_alloc(nfc, NfcProtocolType4Tag);
    nfc_poller_start(poller, t4_cb, &c);
    nfc_poller_stop(poller);
    const Type4TagData* d = (const Type4TagData*)nfc_poller_get_data(poller);
    CHECK(c.mode_asked == 1 && c.success == 1 && c.failed == 0, "T4T: read mode, read success");
    CHECK(d->ndef_file_id == 0xE104 && d->ndef_max_len == 200 - 2, "T4T: CC (NDEF file E104)");
    CHECK(d->t4t_version.major == 2, "T4T: mapping version 2.0");
    CHECK(simple_array_get_count(d->ndef_data) == 18 &&
              !memcmp(simple_array_cget_data(d->ndef_data), app.ndef + 2, 18),
          "T4T: the NDEF message, 18 bytes");
    CHECK(app.ins[0xD6] == 0 && app.ins[0xE0] == 0 && app.ins[0xE2] == 0,
          "T4T: read mode writes and creates nothing");
    nfc_poller_free(poller);

    /* Chained answers (ISO 14443-4 chaining, a card with a small frame). */
    cm_t4_init(&app, &k);
    k.dep.chunk = 16;
    memset(&c, 0, sizeof c);
    poller = nfc_poller_alloc(nfc, NfcProtocolType4Tag);
    nfc_poller_start(poller, t4_cb, &c);
    nfc_poller_stop(poller);
    d = (const Type4TagData*)nfc_poller_get_data(poller);
    /* Stock's iso14443_4_layer does not gather a chained answer yet (its
     * "TODO: properly handle block chaining"); the read must end cleanly,
     * one event, no hang.  libnfc's own iso_dep (the device path) does
     * chain: t4test. */
    printf("     (vendored ISO-DEP with a chaining card: %s)\n",
           c.success ? "read" : "read failed, as its TODO says");
    CHECK(c.success + c.failed == 1 && k.dep.chains_out > 0,
          "T4T: a chained answer ends the vendored read cleanly (one event, no hang)");
    nfc_poller_free(poller);

    /* No NDEF application: the read fails, nothing is created. */
    cm_t4_init(&app, &k);
    app.app[0].aid[6] = 0x02;
    memset(&c, 0, sizeof c);
    poller = nfc_poller_alloc(nfc, NfcProtocolType4Tag);
    nfc_poller_start(poller, t4_cb, &c);
    nfc_poller_stop(poller);
    CHECK(c.success == 0 && c.failed == 1 && app.ins[0xE0] == 0,
          "T4T: no NDEF application -> read failed, nothing created");
    nfc_poller_free(poller);
    nfc_free(nfc);
}

/* ---- MIFARE Plus ---- */

typedef struct {
    int success, failed;
} mfp_ctx_t;

static NfcCommand mfp_cb(NfcGenericEvent event, void* context) {
    mfp_ctx_t* c = context;
    const MfPlusPollerEvent* e = event.event_data;
    if(event.protocol != NfcProtocolMfPlus) return NfcCommandContinue;
    if(e->type == MfPlusPollerEventTypeRequestMode) {
        e->data->mode_request.mode = MfPlusPollerModeInfo;
        return NfcCommandContinue;
    }
    if(e->type == MfPlusPollerEventTypeReadSuccess) c->success++;
    else if(e->type == MfPlusPollerEventTypeReadFailed) c->failed++;
    else return NfcCommandContinue;
    return NfcCommandStop;
}

static void test_mfp_poll(int sl) {
    static cm_4a_t k;
    static cm_mfplus_t app;
    nfc_xfer_t x = {0};
    mfp_ctx_t c = {0};
    char msg[80];

    cm_mfplus_init(&app, &k, sl);
    x.transceive = cm_4a_xfer;
    x.ctx = &k;
    Nfc* nfc = nfc_alloc_xfer(&x);
    NfcPoller* poller = nfc_poller_alloc(nfc, NfcProtocolMfPlus);
    nfc_poller_start(poller, mfp_cb, &c);
    nfc_poller_stop(poller);
    const MfPlusData* d = (const MfPlusData*)nfc_poller_get_data(poller);
    snprintf(msg, sizeof msg, "MF Plus SL%d: info read", sl);
    CHECK(c.success == 1 && c.failed == 0, msg);
    snprintf(msg, sizeof msg, "MF Plus SL%d: EV1 2K from GetVersion", sl);
    CHECK(d->type == MfPlusTypeEV1 && d->size == MfPlusSize2K && d->version.hw_vendor == 0x04, msg);
    snprintf(msg, sizeof msg, "MF Plus SL%d: security level from the probe", sl);
    CHECK(d->security_level == (sl == 0 ? MfPlusSecurityLevel0 : MfPlusSecurityLevel3), msg);
    CHECK(app.perso_probes == 1 && app.others == 0,
          "MF Plus: one WritePerso probe (invalid block, nothing written), nothing else");
    nfc_poller_free(poller);
    nfc_free(nfc);
}

/* SL1/SL2 cards answer as MIFARE Classic (SAK 08/18, 10/11): the level
 * comes from the SAK once GetVersion said "Plus" (AN10833). */
static void test_mfp_sak(uint8_t sak, MfPlusSecurityLevel want, const char* msg) {
    Iso14443_4aData* iso = iso14443_4a_alloc();
    MfPlusData* d = mf_plus_alloc();
    static const uint8_t v[28] = {0x04, 0x02, 0x01, 0x22, 0x00, 0x18, 0x05};
    memcpy(&d->version, v, sizeof v);
    iso->iso14443_3a_data->sak = sak;
    CHECK(mf_plus_get_type_from_version(iso, MfPlusSecurityLevelUnknown, d) == MfPlusErrorNone &&
              d->type == MfPlusTypeEV2 && d->size == MfPlusSize4K && d->security_level == want,
          msg);
    mf_plus_free(d);
    iso14443_4a_free(iso);
}

static void test_mfp_crypto(void) {
    uint8_t key[16], in[64], out[64], exp[64], iv[16];

    printf("-- MIFARE Plus AES (known answers)\n");
    /* FIPS-197 Appendix C.1 */
    unhex("000102030405060708090a0b0c0d0e0f", key);
    unhex("00112233445566778899aabbccddeeff", in);
    unhex("69c4e0d86a7b0430d8cdb78070b4c55a", exp);
    mf_plus_crypto_ecb_encrypt(key, in, out);
    CHECK(!memcmp(out, exp, 16), "AES-128 ECB encrypt (FIPS-197 C.1)");
    mf_plus_crypto_ecb_decrypt(key, exp, out);
    CHECK(!memcmp(out, in, 16), "AES-128 ECB decrypt (FIPS-197 C.1)");
    /* SP 800-38A F.2.1 CBC-AES128, two blocks */
    unhex("2b7e151628aed2a6abf7158809cf4f3c", key);
    unhex("000102030405060708090a0b0c0d0e0f", iv);
    unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51", in);
    unhex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2", exp);
    mf_plus_crypto_cbc_encrypt(key, iv, in, out, 32);
    CHECK(!memcmp(out, exp, 32), "AES-128 CBC encrypt (SP 800-38A F.2.1)");
    mf_plus_crypto_cbc_decrypt(key, iv, exp, out, 32);
    CHECK(!memcmp(out, in, 32), "AES-128 CBC decrypt (SP 800-38A F.2.2)");
    /* RFC 4493 / SP 800-38B D.1 AES-CMAC examples 1, 2, 3 */
    mf_plus_crypto_cmac(key, in, 0, out);
    unhex("bb1d6929e95937287fa37d129b756746", exp);
    CHECK(!memcmp(out, exp, 16), "AES-CMAC, empty message (RFC 4493 ex. 1)");
    mf_plus_crypto_cmac(key, in, 16, out);
    unhex("070a16b46b4d4144f79bdd9dd04a287c", exp);
    CHECK(!memcmp(out, exp, 16), "AES-CMAC, 16 bytes (RFC 4493 ex. 2)");
    unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411", in);
    mf_plus_crypto_cmac(key, in, 40, out);
    unhex("dfa66747de9ae63030ca32611497c827", exp);
    CHECK(!memcmp(out, exp, 16), "AES-CMAC, 40 bytes (RFC 4493 ex. 3)");
    /* MFP's 8-byte MAC: the odd-indexed bytes of the CMAC */
    uint8_t m8[8];
    mf_plus_crypto_cmac8(key, in, 40, m8);
    CHECK(m8[0] == exp[1] && m8[1] == exp[3] && m8[7] == exp[15], "CMAC8: odd bytes of the CMAC");
}

int main(void) {
    test_t4t();
    printf("-- MIFARE Plus (vendored poller)\n");
    test_mfp_poll(3);
    test_mfp_poll(0);
    test_mfp_sak(0x08, MfPlusSecurityLevel1, "MF Plus: SAK 08 -> SL1");
    test_mfp_sak(0x18, MfPlusSecurityLevel1, "MF Plus: SAK 18 -> SL1 (4K)");
    test_mfp_sak(0x11, MfPlusSecurityLevel2, "MF Plus: SAK 11 -> SL2");
    test_mfp_crypto();
    if(fails) {
        printf("%d part-7 test(s) FAILED\n", fails);
        return 1;
    }
    printf("all part-7 poller tests passed\n");
    return 0;
}
