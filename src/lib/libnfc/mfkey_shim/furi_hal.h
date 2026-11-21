/* SPDX-License-Identifier: GPL-3.0-or-later
 * libnfc mfkey shim (spec Phase 14, D.3; issue #387 part 3): the minimal
 * furi types the vendored src/lib/nfc/mfkey engine is compiled against on
 * the host.  The engine (crypto1.c, mfkey_attack.c, mfkey_recovery.c,
 * mfkey_bs_verify.c, mfkey_state_expansion.c, mfkey_batch_prelude.c) is the
 * MIFARE Classic key recovery; it touches none of the GUI.  mfkey.h, which
 * every engine file reaches through crypto1.h, pulls these headers in for a
 * few opaque pointer types only (FuriMutex*, FuriThread*), so the shim is
 * forward declarations, no bodies.  The vendored files are untouched; this
 * dir is placed first on the include path so the stock header names resolve
 * here. */
#ifndef LIBNFC_MFKEY_SHIM_FURI_HAL_H
#define LIBNFC_MFKEY_SHIM_FURI_HAL_H

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct FuriMutex FuriMutex;
typedef struct FuriThread FuriThread;
typedef struct FuriString FuriString;

#ifndef UNUSED
#define UNUSED(x) ((void)(x))
#endif

#endif
