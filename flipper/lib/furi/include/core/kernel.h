/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri (spec R5.1): ticks and delays (stock furi/core/kernel.h).
 * One tick is one millisecond, as in the stock firmware. The clock behind
 * it is the system's (times(), 60 Hz on MINIX today) until R4b.6. */
#ifndef FURI_CORE_KERNEL_H
#define FURI_CORE_KERNEL_H

#include "core/base.h"

#ifdef __cplusplus
extern "C" {
#endif

bool furi_kernel_is_irq_or_masked(void);
bool furi_kernel_is_running(void);
int32_t furi_kernel_lock(void);
int32_t furi_kernel_unlock(void);
int32_t furi_kernel_restore_lock(int32_t lock);
uint32_t furi_kernel_get_tick_frequency(void);

uint32_t furi_get_tick(void);
uint32_t furi_ms_to_ticks(uint32_t milliseconds);

/* Delays let the other cooperative threads run. */
void furi_delay_tick(uint32_t ticks);
FuriStatus furi_delay_until_tick(uint32_t tick);
void furi_delay_ms(uint32_t milliseconds);
/* No busy wait: rounded up to a millisecond and slept like furi_delay_ms. */
void furi_delay_us(uint32_t microseconds);

#ifdef __cplusplus
}
#endif

#endif
