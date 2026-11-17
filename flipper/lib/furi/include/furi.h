/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri (spec R5.1, section D): source compatibility with the stock
 * firmware's furi core for apps rebuilt for Galtzagorriak MINIX.
 * The layout follows the stock furi/furi.h. */
#ifndef FURI_H
#define FURI_H

#include <stdlib.h>
#include <string.h>

#include "core/base.h"
#include "core/check.h"
#include "core/common_defines.h"
#include "core/event_flag.h"
#include "core/kernel.h"
#include "core/log.h"
#include "core/message_queue.h"
#include "core/mutex.h"
#include "core/record.h"
#include "core/semaphore.h"
#include "core/string.h"
#include "core/thread.h"
#include "core/timer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Stock entry points: there is nothing to start here (the process is
 * the "kernel"); kept so ported code links. */
void furi_init(void);
void furi_run(void);

/* malloc that never returns NULL (stock: crashes when out of memory)
 * and, as the stock heap, returns zeroed memory. */
void* furi_malloc_checked(size_t size);

/* Ported stock code (and the stock gui modules, R5.2) relies on the
 * stock malloc(): zeroed memory, never NULL. Define FURI_LIBC_MALLOC
 * before including furi.h to keep the C library's. */
#ifndef FURI_LIBC_MALLOC
#define malloc(size) furi_malloc_checked(size)
#endif

#ifdef __cplusplus
}
#endif

#endif
