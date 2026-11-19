/* SPDX-License-Identifier: GPL-3.0-or-later
 * libsubghz: the few furi_hal calls the Unleashed protocols make.
 * - furi_hal_subghz_get_rolling_counter_mult(): Unleashed's "counter
 *   increase" setting; the default (1) here, settable by `subghz`.
 * - furi_hal_crypto_*: the CPU2 key enclave decrypts stock's encrypted
 *   keystores (D.3: never shipped; MINIX has no CPU2 crypto client yet):
 *   always false, so only plain keystores load.
 * - the preset names a key file may carry. */
#ifndef SUBGHZ_SHIM_FURI_HAL_H
#define SUBGHZ_SHIM_FURI_HAL_H

#include <furi.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t furi_hal_subghz_get_rolling_counter_mult(void);
void furi_hal_subghz_set_rolling_counter_mult(int32_t mult);

bool furi_hal_crypto_enclave_load_key(uint8_t slot, const uint8_t* iv);
bool furi_hal_crypto_enclave_unload_key(uint8_t slot);
bool furi_hal_crypto_encrypt(const uint8_t* input, uint8_t* output, size_t size);
bool furi_hal_crypto_decrypt(const uint8_t* input, uint8_t* output, size_t size);

typedef enum {
    FuriHalSubGhzPresetIDLE,
    FuriHalSubGhzPresetOok270Async,
    FuriHalSubGhzPresetOok650Async,
    FuriHalSubGhzPreset2FSKDev238Async,
    FuriHalSubGhzPreset2FSKDev476Async,
    FuriHalSubGhzPresetMSK99_97KbAsync,
    FuriHalSubGhzPresetGFSK9_99KbAsync,
    FuriHalSubGhzPresetCustom,
} FuriHalSubGhzPreset;

#ifdef __cplusplus
}
#endif

#endif
