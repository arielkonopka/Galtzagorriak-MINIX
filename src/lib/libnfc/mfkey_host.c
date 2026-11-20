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
