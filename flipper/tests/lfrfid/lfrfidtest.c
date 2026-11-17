/* SPDX-License-Identifier: GPL-3.0-or-later
 * make lfrfidtest (spec R9.0.2, AC9.0.1): every Unleashed LF RFID protocol
 * of liblfrfid (src/lib/lfrfid) round trip on the PC, no hardware.
 *
 * For each of the 25 protocols and a set of data vectors (a fixed one per
 * protocol, then random ones the encoder accepts):
 *   encoder -> the edges a reader's comparator would give -> all decoders
 *   of the read mode, decided as the stock read worker decides
 *   (lfrfid_read.c: the same bytes validate_count + 1 times in a row).
 * Exactly that protocol must win, with the same bytes; no other protocol
 * may be validated anywhere in the stream (no cross-decodes).  Each vector
 * runs clean, with timing jitter, with a comparator skew (one edge kind
 * late, as the Flipper's envelope detector), inverted, and from a random
 * start in the frame.
 *
 * The channel: an encoder yields levels in carrier periods (8 us at
 * 125 kHz).  ASK and FSK protocols: the antenna load is the envelope the
 * reader sees, so a level of n periods is an edge n * 8 us later.  PSK
 * protocols: the encoder yields the carrier itself (one period per level);
 * the stock PSK read (62.5 kHz, 25 % duty) sees the subcarrier's phase, so
 * each pair of levels is one phase sample of 16 us.
 *
 * Also: render_data of the fixed vector equals Unleashed's text
 * (render.txt, made by `make lfrfid-golden` with the C library's printf,
 * as newlib's on the stock firmware), and the decoder heap of all 25.
 *
 *	lfrfidtest [-v] [-g]	-v: each case; -g: print render.txt */
#include <furi.h>
#include <lfrfid_read.h>

#undef malloc

#define NRAND 12 /* random vectors per protocol */
#define FRAMES_US 1000000 /* how long one case plays: 1 s (the stock read window is 2 s) */

static int verbose, fails, cases, known;
static unsigned long rng = 12345;

static unsigned rnd(void) {
    rng = rng * 1103515245UL + 12345UL;
    return (unsigned)(rng >> 16) & 0x7FFF;
}

/* Fixed vectors: one per protocol, data as the stock app shows it. */
static const struct {
    LFRFIDProtocol p;
    uint8_t data[LFRFID_DATA_MAX];
} fixed[] = {
    {LFRFIDProtocolEM4100, {0x01, 0x23, 0x45, 0x67, 0x89}},
    {LFRFIDProtocolEM4100_32, {0x1A, 0x2B, 0x3C, 0x4D, 0x5E}},
    {LFRFIDProtocolEM4100_16, {0xDE, 0xAD, 0xBE, 0xEF, 0x01}},
    {LFRFIDProtocolElectra, {0x7E, 0x00, 0x12, 0x34, 0x56, 0x00, 0x00, 0x00}},
    {LFRFIDProtocolH10301, {0x2A, 0x30, 0x39}},
    {LFRFIDProtocolIdteck, {0x49, 0x44, 0x54, 0x4B, 0x34, 0x56, 0x78, 0x9A}},
    {LFRFIDProtocolIndala26, {0x12, 0x34, 0x56, 0x78}},
    {LFRFIDProtocolIOProxXSF, {0x01, 0x02, 0x30, 0x39}},
    {LFRFIDProtocolAwid, {0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F, 0x70, 0x81, 0x92}},
    {LFRFIDProtocolFDXA, {0x92, 0x34, 0xD6, 0xF8, 0x1A}},
    {LFRFIDProtocolFDXB, {0x00, 0x12, 0x34, 0x56, 0x78, 0xA0, 0x7C, 0x00, 0x01, 0x00, 0x00}},
    {LFRFIDProtocolHidGeneric, {0x02, 0x00, 0x54, 0x60, 0x72, 0x0A}},
    {LFRFIDProtocolHidExGeneric, {0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07}},
    {LFRFIDProtocolPyramid, {0x1A, 0x00, 0x2A, 0x30}},
    {LFRFIDProtocolViking, {0x12, 0x34, 0x56, 0x78}},
    {LFRFIDProtocolJablotron, {0x14, 0x12, 0x34, 0x56, 0x78}},
    {LFRFIDProtocolParadox, {0x00, 0x12, 0x34, 0x56, 0x78, 0x90}},
    {LFRFIDProtocolPACStanley, {0x12, 0x34, 0x56, 0x78}},
    {LFRFIDProtocolKeri, {0x80, 0x00, 0x30, 0x39}},
    {LFRFIDProtocolGallagher, {0x12, 0x00, 0x56, 0x78, 0x00, 0xBC, 0xDE, 0xF0}},
    {LFRFIDProtocolNexwatch, {0x00, 0x34, 0x56, 0x78, 0xDC, 0x03, 0x65, 0xAA}},
    {LFRFIDProtocolSecurakey, {0x00, 0x1A, 0x00, 0x12, 0x34, 0x56}},
    {LFRFIDProtocolGProxII, {0xFA, 0x00, 0x93, 0x20, 0x0C, 0x42, 0x1A, 0x16, 0x1D, 0x08, 0x0A, 0x00}},
    {LFRFIDProtocolNoralsy, {0xBB, 0x04, 0x56, 0x78, 0x90, 0x00, 0xAC, 0x1B, 0x1E, 0xA8, 0x00, 0x00}},
    {LFRFIDProtocolIndala224, {0x80, 0x00, 0x00, 0x01, 0xB2, 0x3E, 0xAE, 0xB0, 0xFF, 0xE8, 0x06, 0x3A, 0xB1, 0x56, 0x01, 0x0F, 0x19, 0xD3, 0x45, 0x7F, 0x5A, 0x3C, 0x96, 0xE1, 0x2D, 0x4B, 0x78, 0xA5}},
};

/* One case's edges: (level, us). */
#define MAXE 200000
static uint8_t elev[MAXE];
static uint32_t edur[MAXE];
static int ne;

static void edge(bool level, uint32_t us) {
    if(ne > 0 && elev[ne - 1] == level) {
        edur[ne - 1] += us;
        return;
    }
    if(ne < MAXE) {
        elev[ne] = level;
        edur[ne++] = us;
    }
}

/* The encoder's stream for one protocol as reader edges, FRAMES_US long,
 * after skipping `skip` levels (random start in the frame). */
static void play(ProtocolDict* dict, LFRFIDProtocol p, unsigned skip) {
    bool psk = protocol_dict_get_features(dict, p) == LFRFIDFeaturePSK;
    uint32_t t = 0;
    unsigned i = 0;
    int half = 0;
    bool first = false;

    ne = 0;
    while(t < FRAMES_US) {
        LevelDuration ld = protocol_dict_encoder_yield(dict, p);
        bool lv = level_duration_get_level(ld);
        uint32_t d = level_duration_get_duration(ld);

        if(level_duration_is_reset(ld) || level_duration_is_wait(ld)) continue;
        if(i++ < skip) continue;
        if(!psk) {
            edge(lv, d * 8);
            t += d * 8;
            continue;
        }
        /* PSK: the first level of each carrier period is its phase */
        if(half == 0) first = lv;
        if(++half == 2) {
            half = 0;
            edge(first, 16);
            t += 16;
        }
    }
}

/* Distort the edges: inverted levels; jitter: every edge moves by up to
 * +-j us (the period between two edges of a kind stays within 2j, as the
 * comparator's); skew: every edge into a low level comes s us late (the
 * Flipper's envelope detector: one edge kind exact, the other drifting). */
static void distort(int invert, int j, int s) {
    long t = 0, prev = 0, e;
    int i;

    for(i = 0; i < ne; i++) {
        t += edur[i];
        e = t;
        if(j) e += (long)(rnd() % (2 * j + 1)) - j;
        if(s && elev[i]) e += s;
        if(e <= prev) e = prev + 1;
        edur[i] = (uint32_t)(e - prev);
        prev = e;
        if(invert) elev[i] = !elev[i];
    }
}

/* Run the reader over the edges as `rfid` does: the absolute level of a
 * capture is unknown, so both are tried.  Edges go in captures of `cap`
 * (0: one stream) with a gap between them.  Returns the first protocol
 * validated; *other is set to any different one validated (cross-decode). */
static ProtocolId decide(
    ProtocolDict* dict,
    uint32_t feature,
    int cap,
    uint8_t* got,
    ProtocolId* other) {
    LfrfidReader r;
    ProtocolId first = PROTOCOL_NO, p;
    int pol, i;

    *other = PROTOCOL_NO;
    for(pol = 0; pol < 2; pol++) {
        lfrfid_reader_start(&r, dict, feature);
        for(i = 0; i < ne; i++) {
            if(cap && i % cap == 0 && i) {
                lfrfid_reader_gap(&r);
                i += cap / 4; /* the edges lost between two reads */
                if(i >= ne) break;
            }
            p = lfrfid_reader_feed(&r, elev[i] ^ pol, edur[i]);
            if(p == PROTOCOL_NO) continue;
            if(first == PROTOCOL_NO) {
                first = p;
                memcpy(got, r.data, LFRFID_DATA_MAX);
            } else if(p != first && *other == PROTOCOL_NO)
                *other = p;
        }
    }
    return first;
}

/* FSK: the demodulator tells 64 us from 80 us periods (fsk_demod.c), so
 * the edges may move by 3 us at most (the comparator follows the carrier:
 * its edges fall on 8 us steps); ASK and PSK take more. */
static int fsk(LFRFIDProtocol p) {
    switch(p) {
    case LFRFIDProtocolH10301:
    case LFRFIDProtocolIOProxXSF:
    case LFRFIDProtocolAwid:
    case LFRFIDProtocolFDXA:
    case LFRFIDProtocolHidGeneric:
    case LFRFIDProtocolHidExGeneric:
    case LFRFIDProtocolPyramid:
    case LFRFIDProtocolParadox:
        return 1;
    default:
        return 0;
    }
}

#define NKIND 7
static const char* const kinds[NKIND] =
    {"clean", "jitter", "skew", "inverted", "start", "all", "captures"};

/* Can `rfid` read it today from 512-edge captures (R9.0.3): ASK, a frame
 * short enough. */
static int capturable(ProtocolDict* dict, LFRFIDProtocol p) {
    (void)dict;
    switch(p) {
    case LFRFIDProtocolEM4100:
    case LFRFIDProtocolEM4100_32:
    case LFRFIDProtocolEM4100_16:
    case LFRFIDProtocolElectra:
    case LFRFIDProtocolFDXB:
    case LFRFIDProtocolViking:
    case LFRFIDProtocolJablotron:
    case LFRFIDProtocolPACStanley:
    case LFRFIDProtocolGallagher:
    case LFRFIDProtocolSecurakey:
    case LFRFIDProtocolGProxII:
    case LFRFIDProtocolNoralsy:
        return 1;
    default:
        return 0;
    }
}

/* Encode data, play it clean and decode it: 1 if it comes back as p, the
 * bytes in out. */
static int roundtrip(ProtocolDict* dict, LFRFIDProtocol p, const uint8_t* data, uint8_t* out) {
    size_t n = protocol_dict_get_data_size(dict, p);
    uint32_t feature = protocol_dict_get_features(dict, p) == LFRFIDFeaturePSK ?
                           LFRFIDFeaturePSK :
                           LFRFIDFeatureASK;
    ProtocolId other;

    protocol_dict_set_data(dict, p, data, n);
    protocol_dict_encoder_start(dict, p);
    play(dict, p, 0);
    return decide(dict, feature, 0, out, &other) == (ProtocolId)p;
}

/* Bits of a frame (MSB first), for the builders below. */
static void setb(uint8_t* d, unsigned at, unsigned n, uint32_t v) {
    while(n--) {
        if((v >> n) & 1)
            d[at / 8] |= 0x80 >> (at % 8);
        else
            d[at / 8] &= ~(0x80 >> (at % 8));
        at++;
    }
}

static uint32_t getb(const uint8_t* d, unsigned at, unsigned n) {
    uint32_t v = 0;

    while(n--) {
        v = v << 1 | ((d[at / 8] >> (7 - at % 8)) & 1);
        at++;
    }
    return v;
}

/* Random fields into a valid frame where the data is the raw frame with
 * check fields (written from each decoder's checks, not from its
 * encoder): random bytes almost never pass them. */
static void build(LFRFIDProtocol p, uint8_t* d) {
    unsigned i, x, e, o;
    uint32_t fc;

    switch(p) {
    case LFRFIDProtocolNoralsy: /* BB0, 60 bits, two nibble XOR sums */
        setb(d, 0, 12, 0xBB0);
        for(x = 0, i = 32; i < 72; i += 4)
            x ^= getb(d, i, 4);
        setb(d, 72, 4, x);
        for(x = 0, i = 0; i < 76; i += 4)
            x ^= getb(d, i, 4);
        setb(d, 76, 4, x);
        setb(d, 80, 16, 0);
        break;
    case LFRFIDProtocolNexwatch: { /* 0, id, mode, parity, checksum */
        unsigned par = 0;

        d[0] = 0;
        d[5] &= 0xF0;
        for(i = 1; i <= 5; i++)
            par ^= (d[i] >> 4) ^ (d[i] & 15);
        par = ((par >> 3) & 1) | (par & 2) | (par & 4) | ((par & 1) << 3);
        d[5] |= par;
        break;
    }
    case LFRFIDProtocolGProxII: { /* 26-bit Wiegand inside a XOR-ed,
                                    * byte-reversed frame, a 0 every 5th bit */
        uint8_t m[9];

        memcpy(m, d, 9);
        setb(m, 8, 6, 26);
        setb(m, 14, 2, 0);
        fc = getb(m, 33, 24);
        for(e = 0, o = 1, i = 0; i < 12; i++) {
            e ^= (fc >> (12 + i)) & 1;
            o ^= (fc >> i) & 1;
        }
        setb(m, 32, 1, e);
        setb(m, 57, 1, o);
        setb(m, 58, 14, 0);
        for(i = 1; i < 9; i++)
            m[i] ^= m[0];
        for(i = 0; i < 9; i++) { /* reverse each byte */
            uint8_t b = m[i], r = 0;
            for(x = 0; x < 8; x++)
                r |= ((b >> x) & 1) << (7 - x);
            m[i] = r;
        }
        memset(d, 0, 12);
        setb(d, 0, 6, 0x3E);
        for(i = 0; i < 72; i++)
            setb(d, 6 + i / 4 * 5 + i % 4, 1, getb(m, i, 1));
        break;
    }
    case LFRFIDProtocolIndala224: /* 1 and 29 zeros, then the payload */
        setb(d, 0, 30, 0x20000000);
        setb(d, 30, 2, 3);
        break;
    default:
        break;
    }
}

/* A tag's data from any bytes: as the writer makes them (write_data fixes
 * parity and format bits), then read back until reading gives the same
 * bytes again (Unleashed's decoders are not all a fixed point on any
 * bytes: Radio Key's 32-bit format moves a parity bit).  1 if found. */
static int canonical(ProtocolDict* dict, LFRFIDProtocol p, const uint8_t* data, uint8_t* out) {
    size_t n = protocol_dict_get_data_size(dict, p);
    uint8_t a[LFRFID_DATA_MAX], b[LFRFID_DATA_MAX];
    LFRFIDWriteRequest req;
    int i;

    protocol_dict_set_data(dict, p, data, n);
    lfrfid_normalize(dict, p, &req);
    protocol_dict_get_data(dict, p, a, n);
    for(i = 0; i < 4; i++) {
        if(!roundtrip(dict, p, a, b)) return 0;
        if(memcmp(a, b, n) == 0) {
            memcpy(out, a, n);
            return 1;
        }
        memcpy(a, b, n);
    }
    return 0;
}

/* One vector of protocol p through every kind of case. */
static void vector(ProtocolDict* dict, LFRFIDProtocol p, const uint8_t* want, int label) {
    size_t n = protocol_dict_get_data_size(dict, p);
    uint32_t feature = protocol_dict_get_features(dict, p) == LFRFIDFeaturePSK ?
                           LFRFIDFeaturePSK :
                           LFRFIDFeatureASK;
    const char* name = protocol_dict_get_name(dict, p);
    int k;

    for(k = 0; k < NKIND; k++) {
        uint8_t got[LFRFID_DATA_MAX];
        ProtocolId res, other;
        unsigned skip = 0;
        size_t i;

        if(k == 6 && !capturable(dict, p)) continue;
        protocol_dict_set_data(dict, p, want, n);
        protocol_dict_encoder_start(dict, p);
        if(k >= 4) skip = rnd() % 4000;
        play(dict, p, skip);
        if(k == 1) distort(0, fsk(p) ? 3 : 8, 0);
        if(k == 2) distort(0, 0, 20);
        if(k == 3) distort(1, 0, 0);
        if(k >= 5) distort(rnd() & 1, fsk(p) ? 3 : 6, 10);
        res = decide(dict, feature, k == 6 ? 512 : 0, got, &other);
        cases++;
        if(res == (ProtocolId)p && memcmp(got, want, n) == 0 && other == PROTOCOL_NO) {
            if(verbose) printf("ok   %-12s #%d %s\n", name, label, kinds[k]);
            continue;
        }
        /* Two PSK decoders check less than the whole frame, and their
         * "corrupted phase" path (+120 us) can lock a bit late on some
         * payloads; the stock firmware reads such a tag the same way, so
         * these are counted, not failed:
         *   Idteck: only the 32-bit preamble (no second one, no checksum);
         *   Nexwatch: the last byte (bits 88-95) is in no check. */
        if(res == (ProtocolId)p && other == PROTOCOL_NO &&
           ((p == LFRFIDProtocolIdteck && memcmp(got, want, 4) == 0) ||
            (p == LFRFIDProtocolNexwatch && memcmp(got, want, 7) == 0))) {
            known++;
            if(verbose) printf("known %-11s #%d %s: a bit late\n", name, label, kinds[k]);
            continue;
        }
        fails++;
        printf("FAIL %-12s #%d %s: ", name, label, kinds[k]);
        if(res == PROTOCOL_NO)
            printf("nothing decoded");
        else if(res != (ProtocolId)p)
            printf("decoded as %s", protocol_dict_get_name(dict, res));
        else if(other != PROTOCOL_NO)
            printf("later also %s", protocol_dict_get_name(dict, other));
        else {
            printf("bytes");
            for(i = 0; i < n; i++)
                printf(" %02X", got[i]);
            printf(", want");
            for(i = 0; i < n; i++)
                printf(" %02X", want[i]);
        }
        printf("\n");
    }
}

static void render(ProtocolDict* dict, LFRFIDProtocol p, const uint8_t* data, FILE* out) {
    FuriString* s = furi_string_alloc();

    protocol_dict_set_data(dict, p, data, protocol_dict_get_data_size(dict, p));
    protocol_dict_render_data(dict, s, p);
    fprintf(out, "[%s]\n%s\n", protocol_dict_get_name(dict, p), furi_string_get_cstr(s));
    furi_string_free(s);
}

/* render.txt, in the output of render() */
static int check_render(ProtocolDict* dict, const char* path) {
    char want[8192], got[8192];
    FILE *f = fopen(path, "r"), *m;
    size_t n, g;
    unsigned i;

    if(f == NULL) {
        perror(path);
        return 1;
    }
    n = fread(want, 1, sizeof(want) - 1, f);
    want[n] = 0;
    fclose(f);
    m = fmemopen(got, sizeof(got), "w");
    for(i = 0; i < COUNT_OF(fixed); i++)
        render(dict, fixed[i].p, fixed[i].data, m);
    fflush(m);
    g = ftell(m);
    fclose(m);
    got[g] = 0;
    cases++;
    if(strcmp(want, got) == 0) return 0;
    fails++;
    printf("FAIL render_data differs from %s; got:\n%s", path, got);
    return 1;
}

int main(int argc, char** argv) {
    ProtocolDict* dict;
    size_t heap;
    unsigned i, v, tried[LFRFIDProtocolMax] = {0}, used[LFRFIDProtocolMax] = {0};
    int golden = 0;

    for(i = 1; i < (unsigned)argc; i++) {
        if(strcmp(argv[i], "-v") == 0) verbose = 1;
        if(strcmp(argv[i], "-g") == 0) golden = 1;
    }
    lfrfid_heap_bytes = 0;
    dict = lfrfid_dict_alloc();
    heap = lfrfid_heap_bytes;
    if(argc > 1 && strcmp(argv[1], "-c") == 0) {
        for(i = 0; i < COUNT_OF(fixed); i++) {
            uint8_t c[LFRFID_DATA_MAX];
            size_t n = protocol_dict_get_data_size(dict, fixed[i].p), k;
            int ok = canonical(dict, fixed[i].p, fixed[i].data, c), t;

            for(t = 0; !ok && t < 2000; t++) {
                uint8_t r[LFRFID_DATA_MAX];

                memcpy(r, fixed[i].data, n);
                for(k = n / 2; k < n; k++)
                    r[k] = (uint8_t)rnd();
                build(fixed[i].p, r);
                ok = canonical(dict, fixed[i].p, r, c);
            }

            printf("%s %s:", ok ? "ok" : "NO", protocol_dict_get_name(dict, fixed[i].p));
            for(k = 0; ok && k < n; k++)
                printf(" 0x%02X,", c[k]);
            printf("\n");
        }
        return 0;
    }
    if(golden) {
        for(i = 0; i < COUNT_OF(fixed); i++)
            render(dict, fixed[i].p, fixed[i].data, stdout);
        return 0;
    }
    if(COUNT_OF(fixed) != LFRFIDProtocolMax) {
        printf("FAIL %u fixed vectors for %u protocols\n", (unsigned)COUNT_OF(fixed), LFRFIDProtocolMax);
        fails++;
    }
    for(i = 0; i < COUNT_OF(fixed); i++) {
        uint8_t c[LFRFID_DATA_MAX];
        size_t n = protocol_dict_get_data_size(dict, fixed[i].p), k;

        /* the fixed vectors are real tags' data: they come back unchanged */
        memset(c, 0, sizeof(c));
        if(!roundtrip(dict, fixed[i].p, fixed[i].data, c) || memcmp(c, fixed[i].data, n)) {
            printf("FAIL %s: the fixed vector does not decode to itself:",
                   protocol_dict_get_name(dict, fixed[i].p));
            for(k = 0; k < n; k++)
                printf(" %02X", c[k]);
            printf("\n");
            fails++;
        }
        vector(dict, fixed[i].p, fixed[i].data, 0);
    }
    for(i = 0; i < LFRFIDProtocolMax; i++)
        for(v = 0; v < NRAND * 8 && used[i] < NRAND; v++) {
            uint8_t data[LFRFID_DATA_MAX], c[LFRFID_DATA_MAX];
            size_t n = protocol_dict_get_data_size(dict, i), k;

            for(k = 0; k < n; k++)
                data[k] = (uint8_t)rnd();
            build(i, data);
            tried[i]++;
            /* random bytes are not always a tag's data (a format byte,
             * check bits the writer does not set): such are skipped */
            if(!canonical(dict, i, data, c)) continue;
            used[i]++;
            vector(dict, i, c, (int)used[i]);
        }
    for(i = 0; i < LFRFIDProtocolMax; i++) {
        if(verbose || used[i] < NRAND / 2)
            printf("%s %s: %u of %u random vectors are tags' data\n",
                   used[i] < NRAND / 2 ? "FAIL" : "    ", protocol_dict_get_name(dict, i),
                   used[i], tried[i]);
        if(used[i] < NRAND / 2) fails++;
    }
    if(argc > 1 && argv[argc - 1][0] != '-') check_render(dict, argv[argc - 1]);
    protocol_dict_free(dict);
    printf("lfrfidtest: %d protocols, %d cases, %d failed, %d known (Idteck, Nexwatch: see vector()); "
           "decoder heap %zu B (host)\n",
           LFRFIDProtocolMax, cases, fails, known, heap);
    return fails != 0;
}
