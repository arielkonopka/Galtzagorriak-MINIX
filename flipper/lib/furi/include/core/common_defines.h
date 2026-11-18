/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri (spec R5.1): the helper macros of the stock
 * furi/core/common_defines.h that ported apps use. */
#ifndef FURI_CORE_COMMON_DEFINES_H
#define FURI_CORE_COMMON_DEFINES_H

#include <stdbool.h>
#include <stdint.h>

#include "core/check.h"

#ifndef FURI_WARN_UNUSED
#define FURI_WARN_UNUSED __attribute__((warn_unused_result))
#endif
#ifndef FURI_WEAK
#define FURI_WEAK __attribute__((weak))
#endif
#ifndef FURI_PACKED
#define FURI_PACKED __attribute__((packed))
#endif
#ifndef FURI_ALWAYS_INLINE
#define FURI_ALWAYS_INLINE __attribute__((always_inline)) inline
#endif
#ifndef FURI_IS_IRQ_MASKED
#define FURI_IS_IRQ_MASKED() (0)	/* no interrupts in a MINIX process */
#endif
#ifndef FURI_IS_ISR
#define FURI_IS_ISR() (0)
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef ABS
#define ABS(a) ((a) < 0 ? -(a) : (a))
#endif
#ifndef ROUND_UP_TO
#define ROUND_UP_TO(a, b) (((a) + (b) - 1) / (b) * (b))
#endif
#ifndef CLAMP
#define CLAMP(x, upper, lower) (MIN(upper, MAX(x, lower)))
#endif
#ifndef COUNT_OF
#define COUNT_OF(x) (sizeof(x) / sizeof(x[0]))
#endif
#ifndef FURI_SWAP
#define FURI_SWAP(x, y)        \
    do {                       \
        __typeof__(x) _t = x;  \
        x = y;                 \
        y = _t;                \
    } while(0)
#endif
#ifndef PLACE_IN_SECTION
#define PLACE_IN_SECTION(x) __attribute__((section(x)))
#endif
#ifndef ALIGN
#define ALIGN(n) __attribute__((aligned(n)))
#endif
#ifndef FURI_DEPRECATED
#define FURI_DEPRECATED __attribute__((deprecated))
#endif
#ifndef UNUSED
#define UNUSED(X) (void)(X)
#endif
#ifndef STRINGIFY
#define STRINGIFY(x) #x
#endif
#ifndef TOSTRING
#define TOSTRING(x) STRINGIFY(x)
#endif
#ifndef FURI_BIT
#define FURI_BIT(x, n) (((x) >> (n)) & 1)
#endif
#ifndef FURI_BIT_SET
#define FURI_BIT_SET(x, n) ((x) |= (1UL << (n)))
#endif
#ifndef FURI_BIT_CLEAR
#define FURI_BIT_CLEAR(x, n) ((x) &= ~(1UL << (n)))
#endif
#ifndef FURI_SW_MEMBARRIER
#define FURI_SW_MEMBARRIER() __asm__ volatile("" : : : "memory")
#endif

/* Critical sections: the threads are cooperative and nothing else runs in
 * the process, so there is nothing to mask. */
typedef struct {
    uint32_t isrm;
    bool from_isr;
    bool kernel_running;
} __FuriCriticalInfo;
#define FURI_CRITICAL_ENTER() __FuriCriticalInfo __furi_critical_info = {0, false, true}
#define FURI_CRITICAL_EXIT() (void)__furi_critical_info

#endif
