/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri (spec R5.1, D.1): mutexes (stock furi/core/mutex.h). With
 * cooperative threads a mutex only matters across a yield or a block;
 * it is still a real one (owner, recursion count, waiters). */
#ifndef FURI_CORE_MUTEX_H
#define FURI_CORE_MUTEX_H

#include "core/base.h"
#include "core/thread.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FuriMutexTypeNormal,
    FuriMutexTypeRecursive,
} FuriMutexType;

typedef struct FuriMutex FuriMutex;

FuriMutex* furi_mutex_alloc(FuriMutexType type);
void furi_mutex_free(FuriMutex* instance);
FuriStatus furi_mutex_acquire(FuriMutex* instance, uint32_t timeout);
FuriStatus furi_mutex_release(FuriMutex* instance);
FuriThreadId furi_mutex_get_owner(FuriMutex* instance);

#ifdef __cplusplus
}
#endif

#endif
