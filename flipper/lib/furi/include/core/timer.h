/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri (spec R5.1, D.1): software timers (stock furi/core/timer.h).
 * Callbacks run from the scheduler when a thread blocks or yields, never
 * from a signal handler; a callback must not block (it may put to a queue
 * or set flags with timeout 0). */
#ifndef FURI_CORE_TIMER_H
#define FURI_CORE_TIMER_H

#include "core/base.h"
#include "core/thread.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*FuriTimerCallback)(void* context);
typedef void (*FuriTimerPendigCallback)(void* context, uint32_t arg);

typedef enum {
    FuriTimerTypeOnce = 0,
    FuriTimerTypePeriodic = 1,
} FuriTimerType;

typedef enum {
    FuriTimerThreadPriorityNormal,
    FuriTimerThreadPriorityElevated,
} FuriTimerThreadPriority;

typedef struct FuriTimer FuriTimer;

FuriTimer* furi_timer_alloc(FuriTimerCallback func, FuriTimerType type, void* context);
void furi_timer_free(FuriTimer* instance);
FuriStatus furi_timer_start(FuriTimer* instance, uint32_t ticks);
FuriStatus furi_timer_restart(FuriTimer* instance, uint32_t ticks);
FuriStatus furi_timer_stop(FuriTimer* instance);
uint32_t furi_timer_is_running(FuriTimer* instance);
uint32_t furi_timer_get_expire_time(FuriTimer* instance);
/* Runs callback(context, arg) from the scheduler, soon. */
void furi_timer_pending_callback(FuriTimerPendigCallback callback, void* context, uint32_t arg);
void furi_timer_set_thread_priority(FuriTimerThreadPriority priority);
/* Waits until no timer callback is running (always true here). */
void furi_timer_flush(void);

#ifdef __cplusplus
}
#endif

#endif
