/* SPDX-License-Identifier: GPL-3.0-or-later
 * mfkeytest (spec Phase 14, AC14.2; issue #387 part 3): the vendored MIFARE
 * Classic key-recovery engine (src/lib/nfc/mfkey) driven on the host through
 * src/lib/libnfc/mfkey_host.  Generate a stock-format `.mfkey32.log` from a
 * known key, recover it, and report the time per key.  Built and run by
 * flipper/tests/nfc (run-nfc, the `build` CI job), under ASan/UBSan. */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "mfkey_host.h"

static int fails;

static void check(int cond, const char* msg) {
    printf("%s: %s\n", cond ? "ok  " : "FAIL", msg);
    if(!cond) fails++;
}

int main(void) {
    const char* nonce_path = "build/mfkey32.log";

    /* A known key planted into a two-authentication mfkey32 trace. */
    static const uint8_t key[6] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
    const uint32_t uid = 0x12345678u;

    check(mfkey_write_test_nonces(nonce_path, key, uid, 1), "write test nonces");

    MfkeyFoundKey found[8];
    uint64_t elapsed_us = 0;
    int n = mfkey_recover_file(nonce_path, found, 8, &elapsed_us);
    check(n == 1, "recovered exactly one key");
    check(n >= 1 && memcmp(found[0].key, key, 6) == 0, "recovered key matches planted key");
    check(n >= 1 && found[0].uid == uid, "recovered key carries the right uid");

    if(n >= 1) {
        printf(
            "     key %02X%02X%02X%02X%02X%02X recovered in %" PRIu64 " us (%.3f s/key)\n",
            found[0].key[0],
            found[0].key[1],
            found[0].key[2],
            found[0].key[3],
            found[0].key[4],
            found[0].key[5],
            elapsed_us,
            elapsed_us / 1e6);
    }

    /* A second, different key, to show the recovery is not returning a
     * constant and the time is per key, not per file. */
    static const uint8_t key2[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    check(mfkey_write_test_nonces(nonce_path, key2, 0xDEADBEEFu, 1), "write second nonces");
    uint64_t e2 = 0;
    int n2 = mfkey_recover_file(nonce_path, found, 8, &e2);
    check(n2 == 1 && memcmp(found[0].key, key2, 6) == 0, "recovered the default transport key");

    /* static_nested: one tag-static sector, two tag nonces with their first
     * keystream words.  Plant a key, recover it from the nested-nonce log. */
    const char* nested_path = "build/mf_classic_nested.log";
    static const uint8_t nkey[6] = {0x13, 0x37, 0xC0, 0xDE, 0xBA, 0xBE};
    const uint32_t nuid = 0xCAFEF00Du;
    check(
        mfkey_write_test_nested_nonces(nested_path, nkey, nuid, 1),
        "write static_nested test nonces");
    uint64_t ne = 0;
    int nn = mfkey_recover_nested_file(nested_path, found, 8, &ne);
    check(nn == 1, "recovered exactly one nested key");
    check(nn >= 1 && memcmp(found[0].key, nkey, 6) == 0, "recovered nested key matches planted key");
    check(nn >= 1 && found[0].uid == nuid, "recovered nested key carries the right uid");
    if(nn >= 1)
        printf(
            "     static_nested key %02X%02X%02X%02X%02X%02X recovered in %" PRIu64
            " us (%.3f s/key)\n",
            found[0].key[0],
            found[0].key[1],
            found[0].key[2],
            found[0].key[3],
            found[0].key[4],
            found[0].key[5],
            ne,
            ne / 1e6);

    /* AC14.2 report: time per key, for the on-device comparison later. */
    if(n >= 1)
        printf(
            "AC14.2 host mfkey32: ~%.3f s/key (device target: within 2x of stock on a real 1K)\n",
            elapsed_us / 1e6);

    printf(fails ? "\nmfkeytest: %d FAILED\n" : "\nmfkeytest: all passed\n", fails);
    return fails ? 1 : 0;
}
