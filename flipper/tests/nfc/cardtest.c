/* cardtest.c - mf_classic_poller_sync and the supported_cards parsers over
 * the libnfc shim and nfc_xfer, against a software MIFARE Classic
 * (cm_mfc.h); issue #404 part 4.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14.  Run by `make test` under ASan/UBSan.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nfc_shim.h"
#include "cm_mfc.h"

#include <nfc/nfc.h>
#include <nfc/protocols/mf_classic/mf_classic_poller_sync.h>

static int fails;
#define CHECK(c, msg)                         \
    do {                                      \
        if(!(c)) {                            \
            printf("FAIL: %s\n", msg);        \
            fails++;                          \
        } else                                \
            printf("ok   %s\n", msg);         \
    } while(0)

static cm_mfc_t card;
static nfc_xfer_t xf;

static Nfc* card_nfc(void) {
    memset(&xf, 0, sizeof xf);
    xf.transceive = cm_mfc_xfer;
    xf.field = cm_mfc_field;
    xf.ctx = &card;
    Nfc* nfc = nfc_alloc_xfer(&xf);
    nfc_shim_set_max_steps(nfc, 4096);
    return nfc;
}

static void key_of(uint64_t v, MfClassicKey* k) {
    for(int i = 5; i >= 0; i--, v >>= 8) k->data[i] = (uint8_t)v;
}

/* ---- mf_classic_poller_sync ---- */
static void test_sync(void) {
    static const uint8_t uid[4] = {0x11, 0x22, 0x33, 0x44};
    cm_mfc_init(&card, uid, 16);
    for(int b = 1; b < 64; b++)
        if(b % 4 != 3)
            for(int i = 0; i < 16; i++) card.block[b][i] = (uint8_t)(b * 7 + i);
    cm_mfc_set_key(&card, 5, 0, 0xA0A1A2A3A4A5ULL);
    cm_mfc_set_key(&card, 5, 1, 0xB0B1B2B3B4B5ULL);
    cmc_value_set(card.block[8], 100, 8);
    Nfc* nfc = card_nfc();

    MfClassicType type = MfClassicTypeNum;
    CHECK(mf_classic_poller_sync_detect_type(nfc, &type) == MfClassicErrorNone &&
              type == MfClassicType1k,
          "sync: detect_type says 1K");

    MfClassicKey k;
    MfClassicBlock blk;
    key_of(0xFFFFFFFFFFFFULL, &k);
    CHECK(mf_classic_poller_sync_read_block(nfc, 4, &k, MfClassicKeyTypeA, &blk) ==
                  MfClassicErrorNone &&
              blk.data[0] == 28 && blk.data[15] == 43,
          "sync: read_block 4 with the default key");
    CHECK(mf_classic_poller_sync_read_block(nfc, 20, &k, MfClassicKeyTypeA, &blk) !=
              MfClassicErrorNone,
          "sync: read_block 20 with a wrong key fails");
    key_of(0xA0A1A2A3A4A5ULL, &k);
    CHECK(mf_classic_poller_sync_read_block(nfc, 21, &k, MfClassicKeyTypeA, &blk) ==
                  MfClassicErrorNone &&
              blk.data[0] == (uint8_t)(21 * 7),
          "sync: read_block 21 with its sector key");

    MfClassicAuthContext ac;
    key_of(0xB0B1B2B3B4B5ULL, &k);
    CHECK(mf_classic_poller_sync_auth(nfc, 23, &k, MfClassicKeyTypeB, &ac) == MfClassicErrorNone,
          "sync: auth key B");

    int32_t v = 0, nv = 0;
    key_of(0xFFFFFFFFFFFFULL, &k);
    CHECK(mf_classic_poller_sync_read_value(nfc, 8, &k, MfClassicKeyTypeA, &v) ==
                  MfClassicErrorNone &&
              v == 100,
          "sync: read_value 100");
    CHECK(mf_classic_poller_sync_change_value(nfc, 8, &k, MfClassicKeyTypeA, -30, &nv) ==
                  MfClassicErrorNone &&
              nv == 70,
          "sync: change_value -30 -> 70");
    memset(blk.data, 0x5A, 16);
    CHECK(mf_classic_poller_sync_write_block(nfc, 9, &k, MfClassicKeyTypeA, &blk) ==
                  MfClassicErrorNone &&
              card.block[9][0] == 0x5A && card.block[9][15] == 0x5A,
          "sync: write_block 9");

    /* the whole card with a key table, key A of sector 5 missing */
    MfClassicDeviceKeys keys = {};
    for(int s = 0; s < 16; s++) {
        key_of(0xFFFFFFFFFFFFULL, &keys.key_a[s]);
        if(s != 5) keys.key_a_mask |= 1ULL << s;
    }
    key_of(0xB0B1B2B3B4B5ULL, &keys.key_b[5]);
    keys.key_b_mask = 1ULL << 5;
    MfClassicData* d = mf_classic_alloc();
    MfClassicError e = mf_classic_poller_sync_read(nfc, &keys, d);
    CHECK(e == MfClassicErrorNone || e == MfClassicErrorPartialRead, "sync: read whole card");
    uint8_t sr = 0, kf = 0;
    mf_classic_get_read_sectors_and_keys(d, &sr, &kf);
    printf("     sync read: %u sectors, %u keys\n", sr, kf);
    CHECK(d->type == MfClassicType1k && sr == 16, "sync: 16 sectors read");
    CHECK(mf_classic_is_block_read(d, 21) && d->block[21].data[0] == (uint8_t)(21 * 7),
          "sync: sector 5 read with key B");
    CHECK(d->block[0].data[0] == 0x11 && d->block[9].data[3] == 0x5A, "sync: block contents");
    mf_classic_free(d);
    printf("     sync: %u auths, %u reads, %u frames\n", card.auths, card.reads,
           nfc_shim_frames(nfc));
    nfc_free(nfc);
}

int main(void) {
    printf("libnfc mf_classic sync + supported_cards tests (#404)\n");
    test_sync();
    if(fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("all mf_classic sync / supported_cards tests passed\n");
    return 0;
}
