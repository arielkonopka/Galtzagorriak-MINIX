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
#include <nfc/nfc_device.h>
#include <bit_lib/bit_lib.h>
#include <locale/locale.h>
#include <nfc/protocols/mf_classic/mf_classic_poller_sync.h>

#include <nfc/protocols/mf_desfire/mf_desfire.h>
#include <plugins/supported_cards/nfc_supported_card_plugin.h>

#include "nfc_supported_cards.h"

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

/* ---- supported_cards: the pattern ----
 * A card the way the reader app sees it: a plain read gave the UID and
 * type (here set directly), then nfc_supported_cards_read() finds the
 * parser whose verify() accepts the card, the parser reads it with its
 * own keys, and nfc_supported_cards_parse() turns the dump into text. */
static NfcDevice* nsc_device(const uint8_t* uid, MfClassicType type) {
    MfClassicData* d = mf_classic_alloc();
    d->type = type;
    mf_classic_set_uid(d, uid, 4);
    NfcDevice* dev = nfc_device_alloc();
    nfc_device_set_data(dev, NfcProtocolMfClassic, d);
    mf_classic_free(d);
    return dev;
}

/* Run read + parse on the card in `card`; returns the text (static). */
static const char* nsc_run(const uint8_t* uid, MfClassicType type, const char* want, int* ok) {
    static char text[NFCSHIM_STR_MAX + 1];
    Nfc* nfc = card_nfc();
    NfcDevice* dev = nsc_device(uid, type);
    FuriString* out = furi_string_alloc();
    int r = nfc_supported_cards_read(nfc, dev);
    int p = nfc_supported_cards_parse(dev, out);
    *ok = r >= 0 && p == r && !strcmp(nfc_supported_cards_name((size_t)p), want);
    if(!*ok)
        printf("     %s: read by %s, parsed by %s\n", want,
               r >= 0 ? nfc_supported_cards_name((size_t)r) : "none",
               p >= 0 ? nfc_supported_cards_name((size_t)p) : "none");
    snprintf(text, sizeof text, "%s", furi_string_get_cstr(out));
    furi_string_free(out);
    nfc_device_free(dev);
    nfc_free(nfc);
    return text;
}

/* MiZIP (mizip.c): keys of sectors 1-4 derived from the UID by XOR
 * tables, written here again from the published derivation. */
static void test_mizip(void) {
    static const uint8_t uid[4] = {0xDE, 0xAD, 0x12, 0x34};
    static const uint8_t xa[4][6] = {{0x09, 0x12, 0x5A, 0x25, 0x89, 0xE5},
                                     {0xAB, 0x75, 0xC9, 0x37, 0x92, 0x2F},
                                     {0xE2, 0x72, 0x41, 0xAF, 0x2C, 0x09},
                                     {0x31, 0x7A, 0xB7, 0x2F, 0x44, 0x90}};
    static const uint8_t xb[4][6] = {{0xF1, 0x2C, 0x84, 0x53, 0xD8, 0x21},
                                     {0x73, 0xE7, 0x99, 0xFE, 0x32, 0x41},
                                     {0xAA, 0x4D, 0x13, 0x76, 0x56, 0xAE},
                                     {0xB0, 0x13, 0x27, 0x27, 0x2D, 0xFD}};
    static const uint8_t oa[6] = {0, 1, 2, 3, 0, 1}, ob[6] = {2, 3, 0, 1, 2, 3};
    cm_mfc_init(&card, uid, 5);
    card.a.sak = 0x09; /* MIFARE Mini */
    cm_mfc_set_key(&card, 0, 0, 0xA0A1A2A3A4A5ULL);
    cm_mfc_set_key(&card, 0, 1, 0xB4C132439EEFULL);
    for(int s = 1; s < 5; s++) {
        uint64_t ka = 0, kb = 0;
        for(int i = 0; i < 6; i++) {
            ka = ka << 8 | (uint8_t)(uid[oa[i]] ^ xa[s - 1][i]);
            kb = kb << 8 | (uint8_t)(uid[ob[i]] ^ xb[s - 1][i]);
        }
        cm_mfc_set_key(&card, s, 0, ka);
        cm_mfc_set_key(&card, s, 1, kb);
    }
    card.block[8][1] = 1234 & 0xff, card.block[8][2] = 1234 >> 8;  /* current */
    card.block[9][1] = 1000 & 0xff, card.block[9][2] = 1000 >> 8;  /* previous */
    card.block[10][0] = 0x00;                                      /* 8 is current */
    int ok;
    const char* t = nsc_run(uid, MfClassicTypeMini, "mizip", &ok);
    CHECK(ok, "mizip: verified, read with derived keys, parsed");
    CHECK(strstr(t, "MiZIP Card") && strstr(t, "UID: DE AD 12 34"), "mizip: title and UID");
    CHECK(strstr(t, "Current Credit: 12.34 E") && strstr(t, "Previous Credit: 10.00 E"),
          "mizip: current and previous credit");
    card.block[10][0] = 0x55; /* the pointer swaps the two */
    t = nsc_run(uid, MfClassicTypeMini, "mizip", &ok);
    CHECK(ok && strstr(t, "Current Credit: 10.00 E"), "mizip: credit pointer in block 10");
    cm_mfc_set_key(&card, 0, 1, 0x112233445566ULL); /* not a MiZIP */
    Nfc* nfc = card_nfc();
    NfcDevice* dev = nsc_device(uid, MfClassicTypeMini);
    CHECK(nfc_supported_cards_read(nfc, dev) < 0, "mizip: other card not taken");
    nfc_device_free(dev);
    nfc_free(nfc);
}

/* ---- every parser in the table ----
 * Dumps no parser may claim (a blank, fully read 1K and 4K; an empty
 * DESFire), dumps of random bytes with random read masks that must not
 * crash or overrun any parser (ASan/UBSan are on), and a default-key card
 * that no parser's verify() may take for its own. */
static uint32_t sweep_rng = 0x9E3779B9u;
static uint32_t sweep_rand(void) {
    sweep_rng ^= sweep_rng << 13;
    sweep_rng ^= sweep_rng >> 17;
    sweep_rng ^= sweep_rng << 5;
    return sweep_rng;
}

static NfcDevice* sweep_mfc(MfClassicType type, int random) {
    static const uint8_t uid[4] = {0x04, 0x91, 0x7C, 0x22};
    MfClassicData* d = mf_classic_alloc();
    d->type = type;
    mf_classic_set_uid(d, uid, 4);
    uint16_t blocks = mf_classic_get_total_block_num(type);
    for(uint16_t b = 0; b < blocks; b++) {
        MfClassicBlock blk;
        for(int i = 0; i < 16; i++) blk.data[i] = random ? (uint8_t)sweep_rand() : 0;
        if(mf_classic_is_sector_trailer((uint8_t)b) && !random) {
            memset(blk.data, 0xFF, 16);
            blk.data[6] = 0xFF, blk.data[7] = 0x07, blk.data[8] = 0x80, blk.data[9] = 0x69;
        }
        if(!random || (sweep_rand() & 3)) mf_classic_set_block_read(d, (uint8_t)b, &blk);
    }
    uint8_t sectors = mf_classic_get_total_sectors_num(type);
    for(uint8_t s = 0; s < sectors; s++) {
        MfClassicSectorTrailer* t = mf_classic_get_sector_trailer_by_sector(d, s);
        if(!random || (sweep_rand() & 1))
            mf_classic_set_key_found(d, s, MfClassicKeyTypeA,
                                     bit_lib_bytes_to_num_be(t->key_a.data, 6));
        if(!random || (sweep_rand() & 1))
            mf_classic_set_key_found(d, s, MfClassicKeyTypeB,
                                     bit_lib_bytes_to_num_be(t->key_b.data, 6));
    }
    NfcDevice* dev = nfc_device_alloc();
    nfc_device_set_data(dev, NfcProtocolMfClassic, d);
    mf_classic_free(d);
    return dev;
}

static void test_sweep(void) {
    size_t n = nfc_supported_cards_count();
    FuriString* out = furi_string_alloc();
    unsigned claimed_blank = 0, runs = 0, mfc = 0, desfire = 0;
    static const MfClassicType types[] = {MfClassicTypeMini, MfClassicType1k, MfClassicType4k};

    MfDesfireData* df = mf_desfire_alloc();
    NfcDevice* dfdev = nfc_device_alloc();
    nfc_device_set_data(dfdev, NfcProtocolMfDesfire, df);
    mf_desfire_free(df);

    for(size_t i = 0; i < n; i++) {
        const NfcSupportedCardsPlugin* p = nfc_supported_cards_plugin(i);
        if(p->protocol == NfcProtocolMfDesfire) {
            desfire++;
            furi_string_reset(out);
            if(p->parse && p->parse(dfdev, out)) {
                printf("     %s claims an empty DESFire\n", nfc_supported_cards_name(i));
                claimed_blank++;
            }
            continue;
        }
        if(p->protocol != NfcProtocolMfClassic) continue;
        mfc++;
        for(size_t t = 0; t < COUNT_OF(types); t++) {
            NfcDevice* dev = sweep_mfc(types[t], 0);
            furi_string_reset(out);
            if(p->parse && p->parse(dev, out)) {
                printf("     %s claims a blank card\n", nfc_supported_cards_name(i));
                claimed_blank++;
            }
            nfc_device_free(dev);
            for(int r = 0; r < 40; r++) {
                dev = sweep_mfc(types[t], 1);
                furi_string_reset(out);
                if(p->parse) (void)p->parse(dev, out);
                runs++;
                nfc_device_free(dev);
            }
        }
    }
    nfc_device_free(dfdev);
    printf("     sweep: %zu parsers (%u Classic, %u DESFire), %u random dumps\n", n, mfc, desfire,
           runs);
    CHECK(claimed_blank == 0, "sweep: no parser claims a blank card");
    CHECK(runs > 0, "sweep: random dumps parsed without a crash");

    /* a fresh default-key 1K on the air: nobody's verify+read takes it */
    static const uint8_t uid[4] = {0x04, 0x91, 0x7C, 0x22};
    cm_mfc_init(&card, uid, 16);
    Nfc* nfc = card_nfc();
    NfcDevice* dev = nsc_device(uid, MfClassicType1k);
    int r = nfc_supported_cards_read(nfc, dev);
    if(r >= 0) printf("     read by %s\n", nfc_supported_cards_name((size_t)r));
    CHECK(r < 0, "sweep: a default-key card is read by no parser");
    nfc_device_free(dev);
    nfc_free(nfc);
    furi_string_free(out);
}

/* ---- the datetime/locale shim the dated parsers print with ---- */
static void test_datetime(void) {
    DateTime dt;
    FuriString* s = furi_string_alloc();
    datetime_timestamp_to_datetime(0, &dt);
    CHECK(dt.year == 1970 && dt.month == 1 && dt.day == 1 && dt.weekday == 4,
          "datetime: epoch is Thursday 1970-01-01");
    datetime_timestamp_to_datetime(951825599u, &dt); /* 2000-02-29 11:59:59 */
    CHECK(dt.year == 2000 && dt.month == 2 && dt.day == 29 && dt.hour == 11 && dt.second == 59,
          "datetime: leap day 2000");
    CHECK(datetime_datetime_to_timestamp(&dt) == 951825599u, "datetime: round trip");
    locale_format_date(s, &dt, locale_get_date_format(), ".");
    CHECK(!strcmp(furi_string_get_cstr(s), "29.02.2000"), "locale: D.M.Y");
    locale_format_time(s, &dt, locale_get_time_format(), true);
    CHECK(!strcmp(furi_string_get_cstr(s), "11:59:59"), "locale: 24 h");
    CHECK(!datetime_is_leap_year(1900) && datetime_get_days_per_month(true, 2) == 29,
          "datetime: 1900 not leap, Feb 29 days in leap years");
    furi_string_free(s);
}

int main(void) {
    printf("libnfc mf_classic sync + supported_cards tests (#404)\n");
    test_datetime();
    test_sync();
    test_mizip();
    test_sweep();
    if(fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("all mf_classic sync / supported_cards tests passed\n");
    return 0;
}
