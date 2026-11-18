/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri (spec R5.1, D.1): FuriThread as cooperative threads inside the
 * process. API as the stock furi/core/thread.h.
 *
 * Differences from the stock firmware:
 * - A thread runs until it blocks (queue, flags, mutex, semaphore, delay,
 *   join) or calls furi_thread_yield(); priorities are kept but not used.
 * - Stacks come out of the process's stack gap (its chmem), not the heap;
 *   the app's chmem must cover the main stack, every thread stack and the
 *   heap. The C library's printf family needs about 1.2 KB of stack.
 * - Not from signal handlers. A blocking read() blocks every thread of
 *   the process until poll() (R4b.6). */
#ifndef FURI_CORE_THREAD_H
#define FURI_CORE_THREAD_H

#include "core/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FuriThreadStateStopped,
    FuriThreadStateStopping,
    FuriThreadStateStarting,
    FuriThreadStateRunning,
} FuriThreadState;

typedef enum {
    FuriThreadPriorityNone = 0,
    FuriThreadPriorityIdle = 1,
    FuriThreadPriorityLowest = 14,
    FuriThreadPriorityLow = 15,
    FuriThreadPriorityNormal = 16,
    FuriThreadPriorityHigh = 17,
    FuriThreadPriorityHighest = 18,
    FuriThreadPriorityIsr = 31,
} FuriThreadPriority;

typedef struct FuriThread FuriThread;
typedef void* FuriThreadId;

typedef int32_t (*FuriThreadCallback)(void* context);
typedef void (*FuriThreadStateCallback)(
    FuriThread* thread,
    FuriThreadState state,
    void* context);

FuriThread* furi_thread_alloc(void);
FuriThread* furi_thread_alloc_ex(
    const char* name,
    uint32_t stack_size,
    FuriThreadCallback callback,
    void* context);
void furi_thread_free(FuriThread* thread);

void furi_thread_set_name(FuriThread* thread, const char* name);
void furi_thread_set_appid(FuriThread* thread, const char* appid);
void furi_thread_set_stack_size(FuriThread* thread, size_t stack_size);
uint32_t furi_thread_get_stack_size(FuriThread* thread);
void furi_thread_set_callback(FuriThread* thread, FuriThreadCallback callback);
void furi_thread_set_context(FuriThread* thread, void* context);
void furi_thread_set_priority(FuriThread* thread, FuriThreadPriority priority);
FuriThreadPriority furi_thread_get_priority(FuriThread* thread);
void furi_thread_set_current_priority(FuriThreadPriority priority);
FuriThreadPriority furi_thread_get_current_priority(void);
void furi_thread_set_state_callback(FuriThread* thread, FuriThreadStateCallback callback);
void furi_thread_set_state_context(FuriThread* thread, void* context);
FuriThreadState furi_thread_get_state(FuriThread* thread);

void furi_thread_start(FuriThread* thread);
bool furi_thread_join(FuriThread* thread);
int32_t furi_thread_get_return_code(FuriThread* thread);

FuriThreadId furi_thread_get_id(FuriThread* thread);
FuriThreadId furi_thread_get_current_id(void);
FuriThread* furi_thread_get_current(void);
void furi_thread_yield(void);

uint32_t furi_thread_flags_set(FuriThreadId thread_id, uint32_t flags);
uint32_t furi_thread_flags_clear(uint32_t flags);
uint32_t furi_thread_flags_get(void);
uint32_t furi_thread_flags_wait(uint32_t flags, uint32_t options, uint32_t timeout);

const char* furi_thread_get_name(FuriThreadId thread_id);
const char* furi_thread_get_appid(FuriThreadId thread_id);
/* Bytes of the thread's stack never used so far (painted at start). */
uint32_t furi_thread_get_stack_space(FuriThreadId thread_id);

/* Heap tracing is not kept per thread: these do nothing / return 0. */
void furi_thread_enable_heap_trace(FuriThread* thread);
void furi_thread_disable_heap_trace(FuriThread* thread);
size_t furi_thread_get_heap_size(FuriThread* thread);

#ifdef __cplusplus
}
#endif

#endif
