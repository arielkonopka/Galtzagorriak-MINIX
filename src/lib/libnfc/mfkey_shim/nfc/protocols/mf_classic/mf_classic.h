/* SPDX-License-Identifier: GPL-3.0-or-later
 * libnfc mfkey shim (#387 part 3): only the MIFARE Classic key type the
 * recovery engine needs.  The real stock header carries the whole protocol;
 * crypto1.h and mfkey.h use just MfClassicKey (a six-byte key). */
#ifndef LIBNFC_MFKEY_SHIM_MF_CLASSIC_H
#define LIBNFC_MFKEY_SHIM_MF_CLASSIC_H
#include <stdint.h>
#define MF_CLASSIC_KEY_SIZE (6)
typedef struct {
    uint8_t data[MF_CLASSIC_KEY_SIZE];
} MfClassicKey;
#endif
