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
 * Two attack formats are wired here, both host- and command-usable:
 *   - mfkey32: a stock `.mfkey32.log`, two reader authentications on one
 *     sector (mfkey_recover_file);
 *   - static_nested: the stock nested-nonce log (one tag-static sector, two
 *     tag nonces with their first keystream words), mfkey_recover_nested_file.
 * static_encrypted additionally needs a per-UID candidate-key dictionary
 * (`mf_classic_dict_<uid>.nfc`) and key buffering; it is not wired here yet.
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

/* Recover the keys for every well-formed static-nested line in a stock
 * nested-nonce log (format: "Sec <s> key <A|B> cuid <uid> nt0 <nt0> ks0
 * <ks> par0 <bits> nt1 <nt1> ks1 <ks> par1 <bits>").  Only lines carrying
 * both nonces (a full static_nested recovery) are used; single-nonce
 * static_encrypted lines, which need the per-UID dictionary, are skipped.
 * Same return/elapsed contract as mfkey_recover_file. */
int mfkey_recover_nested_file(
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

/* Test helper: write a self-consistent static-nested nonce file for `key`
 * and `uid`, with `count` sector lines.  Each line carries two distinct tag
 * nonces nt0/nt1 and their first keystream words ks0/ks1 as the vendored
 * Crypto-1 produces them, so mfkey_recover_nested_file recovers `key`.
 * Returns true on success. */
bool mfkey_write_test_nested_nonces(
    const char* path,
    const uint8_t key[6],
    uint32_t uid,
    int count);

/* The user key dictionary recovered keys are added to (D.3: no keys are
 * shipped; the user's own file).  Matches mf_dict.h's MF_DICT_USER; a
 * define here keeps mfkey_host free of the mf_dict/mf_classic headers. */
#define MFKEY_DICT_USER "/home/flipper/nfc/assets/mf_classic_dict_user.nfc"

/* Append each of `keys` (big-endian six bytes) to the dictionary at `path`
 * (12 uppercase hex digits a line, as stock writes), skipping any key the
 * file already holds.  The existing file is read one line at a time (the
 * R14 memory rule: never the whole file), so a large dictionary costs no
 * RAM.  A missing file is created.  Returns the number of keys actually
 * written (0 if all were duplicates), or -1 on an I/O error.  `found_from`
 * may hold the sector/uid records from recovery; only the six key bytes
 * are used, through MfkeyFoundKey. */
int mfkey_append_keys_to_dict(const char* path, const MfkeyFoundKey* keys, int n);

#endif
