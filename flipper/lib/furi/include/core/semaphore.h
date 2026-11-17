/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri (spec R5.1, D.1): counting semaphores (stock
 * furi/core/semaphore.h). */
#ifndef FURI_CORE_SEMAPHORE_H
#define FURI_CORE_SEMAPHORE_H

#include "core/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FuriSemaphore FuriSemaphore;

FuriSemaphore* furi_semaphore_alloc(uint32_t max_count, uint32_t initial_count);
void furi_semaphore_free(FuriSemaphore* instance);
FuriStatus furi_semaphore_acquire(FuriSemaphore* instance, uint32_t timeout);
FuriStatus furi_semaphore_release(FuriSemaphore* instance);
uint32_t furi_semaphore_get_count(FuriSemaphore* instance);

#ifdef __cplusplus
}
#endif

#endif
