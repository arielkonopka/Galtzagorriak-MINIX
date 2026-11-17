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

/* malloc that never returns NULL (stock: crashes when out of memory). */
void* furi_malloc_checked(size_t size);

#ifdef __cplusplus
}
#endif

#endif
