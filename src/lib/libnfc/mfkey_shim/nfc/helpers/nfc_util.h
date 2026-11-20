/* SPDX-License-Identifier: GPL-3.0-or-later
 * libnfc mfkey shim (#387 part 3): the two parity helpers crypto1.h uses
 * inline (crypt_word_par).  Defined by the host driver (mfkey_host.c). */
#ifndef LIBNFC_MFKEY_SHIM_NFC_UTIL_H
#define LIBNFC_MFKEY_SHIM_NFC_UTIL_H
#include <stdint.h>
uint8_t nfc_util_even_parity8(uint8_t data);
uint8_t nfc_util_even_parity32(uint32_t data);
#endif
