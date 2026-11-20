/* SPDX-License-Identifier: GPL-3.0-or-later
 * libir (spec Phase 7; issue #389): the thin furi shim the Unleashed
 * infrared encoders and decoders (encoder_decoder/, copied byte for byte)
 * are compiled against.  Only what they use: furi_assert, furi_check,
 * furi_crash, COUNT_OF, MIN, MAX.  malloc() is the stock firmware's:
 * zeroed memory, never NULL (the program ends with a message when the heap
 * is out).  No threads, no records: the same idea as src/lib/subghz/shim.
 */
#ifndef IR_SHIM_H
#define IR_SHIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void ir_crash(const char* message, const char* file, int line) __attribute__((noreturn));
void* ir_zalloc(size_t n);

#define furi_crash(...)  ir_crash("" __VA_ARGS__, __FILE__, __LINE__)
#define furi_check(x, ...) \
    ((x) ? (void)0 : ir_crash("furi_check failed: " #x, __FILE__, __LINE__))
#define furi_assert(x, ...) furi_check(x)

#ifndef COUNT_OF
#define COUNT_OF(x) (sizeof(x) / sizeof(x[0]))
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/* Stock's malloc zeroes and never fails. */
#define malloc(n) ir_zalloc(n)

#endif /* IR_SHIM_H */
