/* SPDX-License-Identifier: GPL-3.0-or-later
 * libnfc mfkey host driver (spec Phase 14, R14.*, AC14.2; issue #387 part 3).
 *
 * The vendored MIFARE Classic key-recovery engine of src/lib/nfc/mfkey
 * (crypto1.c, mfkey_attack.c, mfkey_recovery.c, mfkey_bs_verify.c,
 * mfkey_state_expansion.c, mfkey_batch_prelude.c) is written against the
 * stock GUI app (mfkey.c), which drives it through the storage/stream/GUI
 * records that MINIX does not have.  This driver gives the same engine a
 * plain, host- and command-usable face: a nonce file (the stock
 * `.mfkey32.log` format) in, recovered keys out, no furi records.  It owns
 * the parts of the stock mfkey.c that are not GUI -- the per-nonce `recover`
 * orchestration and the file parsing -- while the recovery math itself stays
 * the untouched vendored code.
 *
 * mfkey32 only (two reader authentications on one sector).  static_nested
 * and static_encrypted need the nested-nonce logs and per-UID dictionaries;
 * they are not wired here yet.
 */
#ifndef LIBNFC_MFKEY_HOST_H
#define LIBNFC_MFKEY_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One recovered key, big-endian six bytes, with the sector it came from. */
typedef struct {
    uint8_t key[6];
    uint32_t uid;
} MfkeyFoundKey;

/* Recover the keys for every well-formed "Sec..." line in a stock
 * `.mfkey32.log` file.  `out`/`out_cap` receive up to out_cap unique keys;
 * the number found is returned, or -1 on a file error.  If `elapsed_us` is
 * non-NULL it is set to the wall-clock microseconds spent in the recovery
 * loop (for the keys/s and time-per-key report, AC14.2). */
int mfkey_recover_file(
    const char* path,
    MfkeyFoundKey* out,
    int out_cap,
    uint64_t* elapsed_us);

/* Test helper: write a self-consistent mfkey32 nonce file for `key`
 * (big-endian six bytes) and `uid`, with `count` sector lines, each a pair
 * of reader authentications with distinct tag nonces.  Returns true on
 * success.  Uses the vendored Crypto-1 so the nonces match what the engine
 * recovers. */
bool mfkey_write_test_nonces(
    const char* path,
    const uint8_t key[6],
    uint32_t uid,
    int count);

#endif
