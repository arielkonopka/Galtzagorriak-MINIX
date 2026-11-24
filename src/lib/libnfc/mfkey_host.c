/* SPDX-License-Identifier: GPL-3.0-or-later
 * libnfc mfkey host driver (spec Phase 14, R14.*, AC14.2; issue #387 part 3).
 * See mfkey_host.h.  The recovery math is the untouched vendored engine in
 * src/lib/nfc/mfkey; this file is the non-GUI half of the stock mfkey.c --
 * the per-nonce orchestration and the `.mfkey32.log` parser -- plus the few
 * symbols the engine expects the app to provide (MSB_LIMIT, sync_state,
 * flush_key_buffer) and the two parity helpers crypto1.h uses inline. */

#include "mfkey_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Vendored engine (angle include: skips this dir, so it is the vendored
 * crypto1.h/mfkey.h, not libnfc's own crypto1.h). */
#include <crypto1.h>
#include <mfkey.h>
#include <mfkey_attack.h>
#include <mfkey_recovery.h>

/* ---- symbols the vendored engine expects from the app -------------------- */

/* Chunk size out of 256; the engine resets it per nonce but references the
 * global.  16 is the stock full-speed value. */
uint8_t MSB_LIMIT = 16;

/* The engine polls this between rounds to let the GUI stop the thread.  On
 * the host nothing interrupts; always keep going. */
int sync_state(ProgramState* program_state) {
    (void)program_state;
    return 0;
}

/* static_encrypted key buffering only; mfkey32 never calls it. */
void flush_key_buffer(ProgramState* program_state) {
    (void)program_state;
}

/* crypto1.h uses these inline (crypt_word_par); the stock build gets them
 * from nfc/helpers/nfc_util.c. */
uint8_t nfc_util_even_parity8(uint8_t data) {
    data ^= data >> 4;
    data ^= data >> 2;
    data ^= data >> 1;
    return data & 1;
}

uint8_t nfc_util_even_parity32(uint32_t data) {
    data ^= data >> 16;
    data ^= data >> 8;
    data ^= data >> 4;
    data ^= data >> 2;
    data ^= data >> 1;
    return (uint8_t)(data & 1);
}

/* ---- host stand-ins for the furi HAL the orchestration touches ----------- */

/* The stock recover() sizes its scratch blocks against the live heap; on the
 * host there is plenty, so report a large free block. */
size_t memmgr_heap_get_max_free_block(void) {
    return (size_t)1 << 24;
}

/* ---- Crypto-1 helpers ---------------------------------------------------- */

static uint64_t bytes_to_num_be(const uint8_t* b, int n) {
    uint64_t v = 0;
    for(int i = 0; i < n; i++) v = (v << 8) | b[i];
    return v;
}

/* The key-to-Crypto1State expansion stock uses in several places. */
static void state_from_key(const uint8_t key[6], struct Crypto1State* s) {
    uint64_t k = bytes_to_num_be(key, 6);
    s->odd = 0;
    s->even = 0;
    for(int i = 0; i < 24; i++) {
        s->odd |= (uint32_t)BIT(k, 2 * i + 1) << (i ^ 3);
        s->even |= (uint32_t)BIT(k, 2 * i) << (i ^ 3);
    }
}

/* ---- per-nonce recovery (the non-GUI core of stock mfkey.c:recover) ------ */

/* Run the vendored MSB-table attack for one mfkey32 nonce.  On success the
 * engine writes the key into n->key; returns true then. */
static bool recover_one(MfClassicNonce* n, int ks2, unsigned int in, ProgramState* ps) {
    /* Block sizes from stock mfkey.c, full speed (MSB_LIMIT == 16). */
    const size_t block_sizes[] = {36992, 36992, 5120, 5120, 4096};
    struct Msb* odd_msbs = malloc(block_sizes[0]);
    struct Msb* even_msbs = malloc(block_sizes[1]);
    unsigned int* temp_states_odd = malloc(block_sizes[2]);
    unsigned int* temp_states_even = malloc(block_sizes[3]);
    unsigned int* states_buffer = malloc(block_sizes[4]);
    if(!odd_msbs || !even_msbs || !temp_states_odd || !temp_states_even || !states_buffer) {
        free(odd_msbs);
        free(even_msbs);
        free(temp_states_odd);
        free(temp_states_even);
        free(states_buffer);
        return false;
    }

    MSB_LIMIT = 16;
    int oks = 0, eks = 0;
    for(int i = 31; i >= 0; i -= 2) oks = oks << 1 | BEBIT(ks2, i);
    for(int i = 30; i >= 0; i -= 2) eks = eks << 1 | BEBIT(ks2, i);

    bool found = false;
    for(int msb = 0; msb <= ((256 / MSB_LIMIT) - 1); msb++) {
        ps->search = msb;
        if(calculate_msb_tables_optimized(
               oks,
               eks,
               msb,
               n,
               states_buffer,
               odd_msbs,
               even_msbs,
               temp_states_odd,
               temp_states_even,
               in,
               ps)) {
            found = true;
            break;
        }
    }

    free(odd_msbs);
    free(even_msbs);
    free(temp_states_odd);
    free(temp_states_even);
    free(states_buffer);
    return found;
}

/* ---- the stock .mfkey32.log parser (init_plugin.c:load_mfkey32_nonces) ---- */

/* A "Sec..." line carries, by token index: 5 uid, 7 nt0, 9 nr0, 11 ar0,
 * 13 nt1, 15 nr1, 17 ar1 (all hex).  Fill a mfkey32 MfClassicNonce; returns
 * false if the line is not a nonce line. */
static bool parse_mfkey32_line(const char* line, MfClassicNonce* res) {
    if(strncmp(line, "Sec", 3) != 0) return false;
    memset(res, 0, sizeof(*res));
    res->attack = mfkey32;
    const char* p = line;
    char* endptr;
    for(int i = 0; i <= 17; i++) {
        if(i != 0) {
            p = strchr(p, ' ');
            if(p)
                p++;
            else
                return false;
        }
        unsigned long value = strtoul(p, &endptr, 16);
        switch(i) {
        case 5:
            res->uid = value;
            break;
        case 7:
            res->nt0 = value;
            break;
        case 9:
            res->nr0_enc = value;
            break;
        case 11:
            res->ar0_enc = value;
            break;
        case 13:
            res->nt1 = value;
            break;
        case 15:
            res->nr1_enc = value;
            break;
        case 17:
            res->ar1_enc = value;
            break;
        default:
            break;
        }
        p = endptr;
    }
    res->p64 = prng_successor(res->nt0, 64);
    res->p64b = prng_successor(res->nt1, 64);
    res->uid_xor_nt0 = res->uid ^ res->nt0;
    res->uid_xor_nt1 = res->uid ^ res->nt1;
    return true;
}

/* ---- the stock nested-nonce parser (init_plugin.c:load_nested_nonces) ---- */

/* "0101" -> bits, MSB first, as stock binaryStringToInt does. */
static uint8_t parity_bits_from_str(const char* s) {
    uint8_t v = 0;
    for(int i = 0; s[i] == '0' || s[i] == '1'; i++) v = (uint8_t)((v << 1) | (s[i] - '0'));
    return v;
}

/* Parse one static-nested line into a static_nested MfClassicNonce.  Returns
 * false unless both nonces are present (a single nonce is static_encrypted,
 * which this host path does not recover).  Mirrors load_nested_nonces. */
static bool parse_nested_line(const char* line, MfClassicNonce* res) {
    if(strncmp(line, "Sec", 3) != 0) return false;
    memset(res, 0, sizeof(*res));
    res->attack = static_encrypted;
    int sector_num = 0;
    char key_type = 'A';
    int parsed = sscanf(
        line,
        "Sec %d key %c cuid %" PRIx32 " nt0 %" PRIx32 " ks0 %" PRIx32 " par0 %4[01] nt1 %" PRIx32
        " ks1 %" PRIx32 " par1 %4[01]",
        &sector_num,
        &key_type,
        &res->uid,
        &res->nt0,
        &res->ks1_1_enc,
        res->par_1_str,
        &res->nt1,
        &res->ks1_2_enc,
        res->par_2_str);
    if(parsed != 9) return false; /* need both nonces for static_nested */
    res->attack = static_nested;
    res->key_idx = (uint8_t)(sector_num * 2 + (key_type == 'B' ? 1 : 0));
    res->par_1 = parity_bits_from_str(res->par_1_str);
    res->par_2 = parity_bits_from_str(res->par_2_str);
    res->uid_xor_nt0 = res->uid ^ res->nt0;
    res->uid_xor_nt1 = res->uid ^ res->nt1;
    return true;
}

static bool key_is_new(const MfkeyFoundKey* out, int n, const uint8_t key[6]) {
    for(int i = 0; i < n; i++)
        if(memcmp(out[i].key, key, 6) == 0) return false;
    return true;
}

int mfkey_recover_file(const char* path, MfkeyFoundKey* out, int out_cap, uint64_t* elapsed_us) {
    FILE* f = fopen(path, "r");
    if(!f) return -1;

    ProgramState ps;
    memset(&ps, 0, sizeof(ps));

    int found = 0;
    char line[256];
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while(fgets(line, sizeof(line), f)) {
        MfClassicNonce n;
        if(!parse_mfkey32_line(line, &n)) continue;
        /* mfkey32: ks2 from the first authentication, in = 0. */
        int ks2 = (int)(n.ar0_enc ^ n.p64);
        if(recover_one(&n, ks2, 0, &ps)) {
            if(found < out_cap && key_is_new(out, found, n.key.data)) {
                memcpy(out[found].key, n.key.data, 6);
                out[found].uid = n.uid;
                found++;
            }
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    if(elapsed_us)
        *elapsed_us =
            (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000ull + (t1.tv_nsec - t0.tv_nsec) / 1000;

    fclose(f);
    return found;
}

int mfkey_recover_nested_file(
    const char* path,
    MfkeyFoundKey* out,
    int out_cap,
    uint64_t* elapsed_us) {
    FILE* f = fopen(path, "r");
    if(!f) return -1;

    ProgramState ps;
    memset(&ps, 0, sizeof(ps));

    int found = 0;
    char line[256];
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while(fgets(line, sizeof(line), f)) {
        MfClassicNonce n;
        if(!parse_nested_line(line, &n)) continue;
        /* static_nested: ks2 from the second keystream word, state fed with
         * uid^nt1 (mfkey.c: ks_enc = ks1_2_enc, nt_xor_uid = uid_xor_nt1). */
        if(recover_one(&n, (int)n.ks1_2_enc, n.uid_xor_nt1, &ps)) {
            if(found < out_cap && key_is_new(out, found, n.key.data)) {
                memcpy(out[found].key, n.key.data, 6);
                out[found].uid = n.uid;
                found++;
            }
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    if(elapsed_us)
        *elapsed_us =
            (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000ull + (t1.tv_nsec - t0.tv_nsec) / 1000;

    fclose(f);
    return found;
}

/* ---- the user dictionary (D.3: the user's own file, no keys shipped) ----- */

static int hexval(int c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse the leading 12 hex digits of a dictionary line into six bytes.
 * Returns true on a well-formed key line (same rule as mf_dict_next:
 * comments and blanks are not keys); false otherwise. */
static bool parse_dict_key(const char* line, uint8_t key[6]) {
    const char* p = line;
    while(*p == ' ' || *p == '\t') p++;
    if(*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') return false;
    for(int i = 0; i < 6; i++) {
        int hi = hexval((unsigned char)p[0]);
        int lo = (hi < 0) ? -1 : hexval((unsigned char)p[1]);
        if(hi < 0 || lo < 0) return false;
        key[i] = (uint8_t)(hi << 4 | lo);
        p += 2;
    }
    return true;
}

int mfkey_append_keys_to_dict(const char* path, const MfkeyFoundKey* keys, int n) {
    if(n <= 0) return 0;

    /* A per-candidate "write me" flag: start true, clear a duplicate. */
    bool want[64];
    if(n > (int)(sizeof want / sizeof want[0])) n = (int)(sizeof want / sizeof want[0]);
    for(int i = 0; i < n; i++) want[i] = true;

    /* Drop keys that repeat inside this batch (keep the first). */
    for(int i = 0; i < n; i++)
        for(int j = 0; j < i; j++)
            if(want[j] && memcmp(keys[i].key, keys[j].key, 6) == 0) {
                want[i] = false;
                break;
            }

    /* Stream the existing file (if any) a line at a time; clear the flag of
     * any candidate already present.  A missing file is simply empty. */
    FILE* f = fopen(path, "r");
    if(f) {
        char line[128];
        while(fgets(line, sizeof line, f)) {
            uint8_t k[6];
            if(!parse_dict_key(line, k)) continue;
            for(int i = 0; i < n; i++)
                if(want[i] && memcmp(keys[i].key, k, 6) == 0) want[i] = false;
        }
        fclose(f);
    }

    int to_write = 0;
    for(int i = 0; i < n; i++)
        if(want[i]) to_write++;
    if(to_write == 0) return 0;

    FILE* out = fopen(path, "a");
    if(!out) return -1;
    int written = 0;
    for(int i = 0; i < n; i++) {
        if(!want[i]) continue;
        const uint8_t* k = keys[i].key;
        if(fprintf(
               out,
               "%02X%02X%02X%02X%02X%02X\n",
               k[0],
               k[1],
               k[2],
               k[3],
               k[4],
               k[5]) < 0) {
            fclose(out);
            return written ? written : -1;
        }
        written++;
    }
    if(fclose(out) != 0) return written ? written : -1;
    return written;
}

/* ---- test nonce generator ------------------------------------------------ */

/* Produce one self-consistent encrypted authentication: feed uid^nt (plain)
 * then a chosen encrypted reader nonce, and read the keystream word so that
 * ar_enc = ks ^ suc64(nt) -- exactly the relation the engine verifies. */
static uint32_t make_auth(const uint8_t key[6], uint32_t uid, uint32_t nt, uint32_t nr_enc) {
    struct Crypto1State s;
    state_from_key(key, &s);
    crypt_word_noret(&s, uid ^ nt, 0);
    crypt_word_noret(&s, nr_enc, 1);
    return crypt_word(&s) ^ prng_successor(nt, 64);
}

bool mfkey_write_test_nonces(const char* path, const uint8_t key[6], uint32_t uid, int count) {
    FILE* f = fopen(path, "w");
    if(!f) return false;
    /* A simple deterministic nonce/reader-nonce sequence; the values need no
     * meaning beyond being distinct, which is all mfkey32 requires. */
    uint32_t seed = 0x1234567u ^ uid;
    for(int i = 0; i < count; i++) {
        uint32_t nt0 = seed = seed * 1664525u + 1013904223u;
        uint32_t nr0 = seed = seed * 1664525u + 1013904223u;
        uint32_t nt1 = seed = seed * 1664525u + 1013904223u;
        uint32_t nr1 = seed = seed * 1664525u + 1013904223u;
        uint32_t ar0 = make_auth(key, uid, nt0, nr0);
        uint32_t ar1 = make_auth(key, uid, nt1, nr1);
        fprintf(
            f,
            "Sector %d key A cuid %08lx nt0 %08lx nr0 %08lx ar0 %08lx nt1 %08lx nr1 %08lx ar1 "
            "%08lx\n",
            i,
            (unsigned long)uid,
            (unsigned long)nt0,
            (unsigned long)nr0,
            (unsigned long)ar0,
            (unsigned long)nt1,
            (unsigned long)nr1,
            (unsigned long)ar1);
    }
    fclose(f);
    return true;
}

/* First keystream word of an auth with a static tag nonce: start at the key
 * state and feed uid^nt (plain); the returned word is exactly the ks0/ks1
 * the nested engine verifies and rebuilds the key from. */
static uint32_t make_nested_ks(const uint8_t key[6], uint32_t uid, uint32_t nt) {
    struct Crypto1State s;
    state_from_key(key, &s);
    return crypt_word_ret(&s, uid ^ nt, 0);
}

bool mfkey_write_test_nested_nonces(
    const char* path,
    const uint8_t key[6],
    uint32_t uid,
    int count) {
    FILE* f = fopen(path, "w");
    if(!f) return false;
    uint32_t seed = 0x89abcdefu ^ uid;
    for(int i = 0; i < count; i++) {
        uint32_t nt0 = seed = seed * 1664525u + 1013904223u;
        uint32_t nt1 = seed = seed * 1664525u + 1013904223u;
        uint32_t ks0 = make_nested_ks(key, uid, nt0);
        uint32_t ks1 = make_nested_ks(key, uid, nt1);
        /* dist 0 (static) and a parity placeholder; the static_nested engine
         * verifies on the keystream words, not the parity bits. */
        fprintf(
            f,
            "Sec %d key A cuid %08lx nt0 %08lx ks0 %08lx par0 0000 nt1 %08lx ks1 %08lx par1 0000 "
            "dist 0\n",
            i,
            (unsigned long)uid,
            (unsigned long)nt0,
            (unsigned long)ks0,
            (unsigned long)nt1,
            (unsigned long)ks1);
    }
    fclose(f);
    return true;
}
