/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri (spec R5.1): status codes and wait options.
 * Names and values as the stock firmware's furi/core/base.h
 * (flipperdevices/flipperzero-firmware, GPL-3.0), so ported code compiles. */
#ifndef FURI_CORE_BASE_H
#define FURI_CORE_BASE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FuriWaitForever = 0xFFFFFFFFU,
} FuriWait;

typedef enum {
    FuriFlagWaitAny = 0x00000000U, /* wait for any flag (default) */
    FuriFlagWaitAll = 0x00000001U, /* wait for all flags */
    FuriFlagNoClear = 0x00000002U, /* do not clear the flags waited for */

    FuriFlagError = 0x80000000U, /* error indicator */
    FuriFlagErrorUnknown = 0xFFFFFFFFU,
    FuriFlagErrorTimeout = 0xFFFFFFFEU,
    FuriFlagErrorResource = 0xFFFFFFFDU,
    FuriFlagErrorParameter = 0xFFFFFFFCU,
    FuriFlagErrorISR = 0xFFFFFFFAU,
} FuriFlag;

typedef enum {
    FuriStatusOk = 0,
    FuriStatusError = -1,
    FuriStatusErrorTimeout = -2,
    FuriStatusErrorResource = -3,
    FuriStatusErrorParameter = -4,
    FuriStatusErrorNoMemory = -5,
    FuriStatusErrorISR = -6,
    FuriStatusReserved = 0x7FFFFFFF,
} FuriStatus;

typedef enum {
    FuriSignalExit,
    FuriSignalCustom = 100,
} FuriSignal;

#ifdef __cplusplus
}
#endif

#endif
