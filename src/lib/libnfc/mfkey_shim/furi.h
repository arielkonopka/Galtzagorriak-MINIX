/* SPDX-License-Identifier: GPL-3.0-or-later
 * libnfc mfkey shim (#387 part 3): mfkey_attack.c includes <furi.h>; the
 * engine uses only the checks and UNUSED.  A failed check ends the host test
 * with the place, as furi_crash would on the device. */
#ifndef LIBNFC_MFKEY_SHIM_FURI_H
#define LIBNFC_MFKEY_SHIM_FURI_H
#include <stdio.h>
#include <stdlib.h>
#include "furi_hal.h"
#define furi_crash(...)                                                    \
    do {                                                                   \
        fprintf(stderr, "furi_crash at %s:%d\n", __FILE__, __LINE__);      \
        abort();                                                           \
    } while(0)
#define furi_check(x, ...) ((x) ? (void)0 : furi_crash())
#define furi_assert(x, ...) furi_check(x)
#define FURI_LOG_E(...) ((void)0)
#define FURI_LOG_W(...) ((void)0)
#define FURI_LOG_I(...) ((void)0)
#define FURI_LOG_D(...) ((void)0)
#define FURI_LOG_T(...) ((void)0)
#endif
