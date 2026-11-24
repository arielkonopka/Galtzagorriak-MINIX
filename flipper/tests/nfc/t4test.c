/* t4test.c - host tests of libnfc's ISO-DEP layer, the Type 4 Tag NDEF
 * reader and the read-only service scanner (#451), against cm_t4.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #451.
 *
 * libnfc asks the front end for CRC_A (NFC_XF_CRC_TX/RX) as the /dev/nfc
 * driver does it; the card models want frames with their CRC, so the
 * adapter below adds and checks it, as the ST25R3916 would.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "iso_dep.h"
#include "aid_scan.h"
#include "t4t.h"
#include "iso14443_3a.h"
#include "cm_t4.h"

static int fails, oks;
#define CHECK(c, what) do { if(c) oks++; else { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); } } while(0)

typedef struct {
    cm_4a_t card;
    cm_t4_t t4;
    int drop;          /* drop the n-th ISO-DEP frame (1-based), 0 never */
    unsigned frames;
} rig_t;

static int rig_xfer(void* ctx, const uint8_t* tx, size_t bits, uint8_t* rx, size_t room,
                    size_t* rxb, uint32_t fwt, uint32_t flags) {
    rig_t* g = ctx;
    uint8_t f[300], a[300];
    size_t n = bits / 8, ab = 0;
    int r;
    if(bits > 8 * 290) return NFC_ERR_OVERFLOW;
    memcpy(f, tx, (bits + 7) / 8);
    if(flags & NFC_XF_CRC_TX) {
        n = cm_crc_add(f, n, CM_CRC_A);
        bits = 8 * n;
    }
    if(g->card.rats_done && (flags & NFC_XF_CRC_TX)) {
        g->frames++;
        if(g->drop && g->frames == (unsigned)g->drop) return NFC_ERR_TIMEOUT;
    }
    r = cm_4a_xfer(&g->card, f, bits, a, sizeof a * 8, &ab, fwt, 0);
    if(r) return r == -5 ? NFC_ERR_OVERFLOW : NFC_ERR_TIMEOUT;
    if(flags & NFC_XF_CRC_RX) {
        if(!cm_crc_ok(a, ab / 8, CM_CRC_A)) return NFC_ERR_CRC;
        ab -= 16;
    }
    if(ab > room) return NFC_ERR_OVERFLOW;
    memcpy(rx, a, (ab + 7) / 8);
    *rxb = ab;
    return NFC_OK;
}

static void rig_init(rig_t* g, nfc_xfer_t* x) {
    memset(g, 0, sizeof *g);
    cm_t4_init(&g->t4, &g->card);
    memset(x, 0, sizeof *x);
    x->transceive = rig_xfer;
    x->ctx = g;
}

static int activate(nfc_xfer_t* x, iso_dep_t* d) {
    iso14443_3a_tag_t t;
    if(iso14443_3a_poll(x, 0, &t) != NFC_OK || t.sak != 0x20) return -1;
    return iso_dep_activate(d, x);
}

static void test_ats(void) {
    iso_dep_t d;
    static const uint8_t a1[] = {0x06, 0x75, 0x77, 0x71, 0x02, 0x80};
    static const uint8_t a2[] = {0x01};
    static const uint8_t a3[] = {0x05, 0x78, 0x80, 0x81, 0x02};
    static const uint8_t bad[] = {0x07, 0x75};
    CHECK(iso_dep_parse_ats(&d, a1, sizeof a1) == NFC_OK && d.fsc == 64
              && d.fwt == 4096u << 7, "ATS: FSCI 5 -> 64, FWI 7");
    CHECK(iso_dep_parse_ats(&d, a2, sizeof a2) == NFC_OK && d.fsc == 32
              && d.fwt == 4096u << 4, "ATS: TL only -> defaults");
    CHECK(iso_dep_parse_ats(&d, a3, sizeof a3) == NFC_OK && d.fsc == 256
              && d.fwt == 4096u << 8, "ATS: FSCI 8 -> 256, FWI 8");
    CHECK(iso_dep_parse_ats(&d, bad, sizeof bad) == NFC_ERR_PROTO, "ATS: TL wrong refused");
}

static void test_t4t(int chunk, int wtx, int drop) {
    rig_t g;
    nfc_xfer_t x;
    iso_dep_t* d = malloc(sizeof *d);
    t4t_t* t = malloc(sizeof *t);
    char s[64];
    rig_init(&g, &x);
    g.card.dep.chunk = (size_t)chunk;
    g.card.dep.wtx_left = wtx;
    g.drop = drop;
    CHECK(activate(&x, d) == NFC_OK && d->fsc == 64, "T4T: 3A + RATS, FSC 64");
    CHECK(g.card.rats_seen == 1, "T4T: one RATS");
    int r = t4t_read(d, t);
    CHECK(r == NFC_OK, "T4T: read");
    CHECK(t->ver == 0x20 && t->mle == 0x3B && t->fid == 0xE104 && t->fmax == 200,
          "T4T: CC parsed");
    CHECK(t->nlen == 18 && t->got == 18, "T4T: NLEN 18 read whole");
    t4t_ndef_summary(t->ndef, t->got, s, sizeof s);
    CHECK(!strcmp(s, "URI https://example.org/t"), "T4T: URI record");
    if(wtx) CHECK(d->wtx == (unsigned)wtx, "T4T: S(WTX) answered");
    if(chunk) CHECK(g.card.dep.chains_out > 0, "T4T: chained answers gathered");
    CHECK(g.t4.ins[0xD6] == 0 && g.t4.ins[0xE2] == 0, "T4T: nothing written");
    free(d);
    free(t);
}

/* A long Text record: chaining of a 150-byte message, read in MLe pieces. */
static void test_t4t_long(void) {
    rig_t g;
    nfc_xfer_t x;
    iso_dep_t* d = malloc(sizeof *d);
    t4t_t* t = malloc(sizeof *t);
    char s[200];
    rig_init(&g, &x);
    g.card.dep.chunk = 20;
    uint8_t* m = g.t4.ndef + 2;
    m[0] = 0xD1; m[1] = 1; m[2] = 146; m[3] = 'T'; m[4] = 2; m[5] = 'e'; m[6] = 'u';
    for(int i = 0; i < 143; i++) m[7 + i] = (uint8_t)('a' + i % 26);
    g.t4.ndef[0] = 0; g.t4.ndef[1] = 150;
    CHECK(activate(&x, d) == NFC_OK && t4t_read(d, t) == NFC_OK, "T4T long: read");
    CHECK(t->nlen == 150 && t->got == 150 && !memcmp(t->ndef, m, 150), "T4T long: 150 bytes");
    t4t_ndef_summary(t->ndef, t->got, s, 20);
    CHECK(!strcmp(s, "Text eu: abcdefghij"), "T4T long: summary cut to the room");
    CHECK(g.t4.ins[0xB0] == 5, "T4T long: CC, NLEN and three MLe (59 B) pieces");
    free(d);
    free(t);
}

static void test_t4t_no_ndef(void) {
    rig_t g;
    nfc_xfer_t x;
    iso_dep_t d;
    t4t_t t;
    rig_init(&g, &x);
    g.t4.app[0].aid[6] = 0x02; /* not the NDEF app */
    CHECK(activate(&x, &d) == NFC_OK && t4t_read(&d, &t) == NFC_ERR_NOTSUP,
          "T4T: no NDEF application -> NOTSUP");
}

/* Chaining out: a command longer than the card's FSC - 3. */
static void test_chain_cmd(void) {
    rig_t g;
    nfc_xfer_t x;
    iso_dep_t d;
    uint8_t cmd[5 + 16 + 1], resp[8];
    size_t rn;
    static const uint8_t aid16[16] = {0xA0, 0, 0, 0x06, 0x47, 0x2F, 0, 1, 1, 2, 3, 4, 5, 6, 7, 8};
    rig_init(&g, &x);
    cm_t4_add(&g.t4, aid16, 16, 0x9000, 0);
    static const uint8_t ats[] = {0x05, 0x70, 0x80, 0x00, 0x02}; /* FSCI 0: 16 bytes */
    memcpy(g.card.ats, ats, sizeof ats);
    CHECK(activate(&x, &d) == NFC_OK && d.fsc == 16, "chain: FSC 16");
    cmd[0] = 0; cmd[1] = 0xA4; cmd[2] = 4; cmd[3] = 0; cmd[4] = 16;
    memcpy(cmd + 5, aid16, 16);
    cmd[21] = 0;
    CHECK(iso_dep_xchg(&d, cmd, sizeof cmd, resp, sizeof resp, &rn) == NFC_OK
              && iso_dep_sw(resp, rn) == 0x9000, "chain: 22-byte SELECT in two blocks");
    CHECK(g.card.dep.chains_in == 1, "chain: one chained I-block");
    /* the block numbers stay in step: a second exchange works */
    CHECK(iso_dep_xchg(&d, cmd, sizeof cmd, resp, sizeof resp, &rn) == NFC_OK
              && iso_dep_sw(resp, rn) == 0x9000, "chain: a second exchange");
}

static const char* dict_path = "/tmp/t4test_aid.txt";

static void write_dict(void) {
    FILE* f = fopen(dict_path, "w");
    fputs("# test dictionary\n"
          "[Payments] Bank cards: the app a shop terminal talks to\n"
          "A0000000031010 Visa credit/debit\n"
          "A0000000041010   Mastercard\n"
          "a000000004 bad: odd? no, 5 bytes ok lower case\n"
          "A00000 too short\n"
          "A0000000031010XY bad hex\n"
          "[Phones] Phones and tags: NDEF and wallet services\n"
          "D2760000850101 NFC Forum Type 4 Tag (NDEF)\n"
          "[Access] Doors and logins\n"
          "A000000308000010000100 PIV\n"
          "[Bad\n"
          "A0000006472F0001 FIDO U2F\n", f);
    fclose(f);
}

static unsigned progress_calls;
static int progress(void* ctx, unsigned tried) {
    (void)ctx;
    (void)tried;
    progress_calls++;
    return 0;
}

static void test_dict(void) {
    aid_dict_t d;
    aid_entry_t e;
    char desc[AID_DESC_MAX];
    int n = 0;
    write_dict();
    CHECK(aid_dict_open(&d, dict_path) == 0, "dict: open");
    while(aid_dict_next(&d, &e) == 1) {
        n++;
        if(n == 1) CHECK(e.aid_len == 7 && e.aid[6] == 0x10 && !strcmp(e.name, "Visa credit/debit")
                             && !strcmp(e.group, "Payments"), "dict: first entry");
        if(n == 2) CHECK(!strcmp(e.name, "Mastercard"), "dict: name after blanks");
        if(n == 3) CHECK(e.aid_len == 5 && e.aid[0] == 0xA0, "dict: lower-case hex");
        if(n == 4) CHECK(!strcmp(e.group, "Phones"), "dict: group changes");
        if(n == 6) CHECK(!strcmp(e.group, "Access") && e.aid_len == 8, "dict: bad group line keeps the group");
    }
    CHECK(n == 6, "dict: 6 entries");
    CHECK(d.bad == 3, "dict: 3 bad lines skipped");
    aid_dict_close(&d);
    CHECK(aid_dict_group_desc(dict_path, "Phones", desc, sizeof desc) == 0
              && !strcmp(desc, "Phones and tags: NDEF and wallet services"), "dict: group description");
    CHECK(aid_dict_group_desc(dict_path, "Nope", desc, sizeof desc) < 0, "dict: unknown group");
    CHECK(aid_dict_open(&d, "/nonexistent/x") < 0, "dict: missing file");
}

static void test_scan(int chunk, int drop) {
    rig_t g;
    nfc_xfer_t x;
    aid_scan_t* s = malloc(sizeof *s);
    static const uint8_t visa[7] = {0xA0, 0, 0, 0, 0x03, 0x10, 0x10};
    static const uint8_t piv[11] = {0xA0, 0, 0, 0x03, 0x08, 0, 0, 0x10, 0, 0x01, 0};
    char h[40];
    rig_init(&g, &x);
    g.card.dep.chunk = (size_t)chunk;
    g.drop = drop;
    cm_t4_add(&g.t4, visa, 7, 0x9000, 120); /* a long FCI */
    cm_t4_add(&g.t4, piv, 11, 0x6283, 0);   /* blocked, still there */
    progress_calls = 0;
    int r = aid_scan(&x, dict_path, s, progress, NULL);
    CHECK(r == NFC_OK, "scan: runs");
    CHECK(s->sak == 0x20 && s->atqa[0] == 0x44 && s->uid_len == 7, "scan: SAK/ATQA/UID");
    CHECK(s->ats_len == 6 && s->ats[1] == 0x75, "scan: ATS kept");
    CHECK(s->tried == 6 && progress_calls == 6, "scan: every AID tried");
    CHECK(s->nhits == 3, "scan: three services found");
    aid_hex(h, s->hit[0].e.aid, s->hit[0].e.aid_len);
    CHECK(!strcmp(h, "A0000000031010") && !strcmp(s->hit[0].e.group, "Payments")
              && s->hit[0].sw == 0x9000, "scan: Visa (long FCI)");
    CHECK(!strcmp(s->hit[1].e.name, "NFC Forum Type 4 Tag (NDEF)"), "scan: NDEF");
    CHECK(!strcmp(s->hit[2].e.name, "PIV") && s->hit[2].sw == 0x6283, "scan: PIV blocked");
    CHECK(s->bad_lines == 3, "scan: bad lines counted");
    /* read-only: SELECT and nothing else reached the card */
    unsigned other = 0;
    for(int i = 0; i < 256; i++)
        if(i != 0xA4) other += g.t4.ins[i];
    CHECK(other == 0 && g.t4.ins[0xA4] >= 6, "scan: only SELECT sent");
    if(drop) CHECK(s->lost == 0, "scan: a lost frame recovered by R(NAK)");
    free(s);
}

static void test_scan_not4(void) {
    rig_t g;
    nfc_xfer_t x;
    aid_scan_t s;
    rig_init(&g, &x);
    g.card.a.sak = 0x08;
    CHECK(aid_scan(&x, dict_path, &s, NULL, NULL) == NFC_ERR_NOTSUP && s.sak == 0x08,
          "scan: a MIFARE Classic is not ISO 14443-4");
    CHECK(aid_scan(&x, "/nonexistent", &s, NULL, NULL) == AID_SCAN_NODICT, "scan: no dictionary");
}

static void test_example_dict(void) {
    aid_dict_t d;
    aid_entry_t e;
    int n = 0;
    CHECK(aid_dict_open(&d, "../../rootfs/usr/share/nfc/aid_scan.nfc") == 0, "example dictionary: present");
    while(aid_dict_next(&d, &e) == 1) n++;
    CHECK(n >= 20 && d.bad == 0, "example dictionary: parses, no bad lines");
    printf("example dictionary: %d AIDs\n", n);
    aid_dict_close(&d);
}

int main(void) {
    test_ats();
    test_t4t(0, 0, 0);
    test_t4t(16, 0, 0);
    test_t4t(0, 2, 0);
    test_t4t(0, 0, 4);
    test_t4t_long();
    test_t4t_no_ndef();
    test_chain_cmd();
    test_dict();
    test_scan(0, 0);
    test_scan(24, 0);
    test_scan(0, 3);
    test_scan_not4();
    test_example_dict();
    unlink(dict_path);
    printf("t4test: %d ok, %d failed\n", oks, fails);
    return fails != 0;
}
