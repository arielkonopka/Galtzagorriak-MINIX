/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri (spec R5.1, D.1): event flags (stock furi/core/event_flag.h).
 * 24 usable bits as in the stock firmware (FreeRTOS keeps the top byte). */
#ifndef FURI_CORE_EVENT_FLAG_H
#define FURI_CORE_EVENT_FLAG_H

#include "core/base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FURI_EVENT_FLAG_MAX_BITS_EVENT_GROUPS 24U
#define FURI_EVENT_FLAG_INVALID_BITS (~((1UL << FURI_EVENT_FLAG_MAX_BITS_EVENT_GROUPS) - 1U))

typedef struct FuriEventFlag FuriEventFlag;

FuriEventFlag* furi_event_flag_alloc(void);
void furi_event_flag_free(FuriEventFlag* instance);
uint32_t furi_event_flag_set(FuriEventFlag* instance, uint32_t flags);
uint32_t furi_event_flag_clear(FuriEventFlag* instance, uint32_t flags);
uint32_t furi_event_flag_get(FuriEventFlag* instance);
uint32_t furi_event_flag_wait(
    FuriEventFlag* instance,
    uint32_t flags,
    uint32_t options,
    uint32_t timeout);

#ifdef __cplusplus
}
#endif

#endif
