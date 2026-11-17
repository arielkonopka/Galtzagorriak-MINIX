/* SPDX-License-Identifier: GPL-3.0-or-later
 * liblfrfid (spec R9.0.1): the thin furi shim the Unleashed protocol files
 * are compiled against.  Only what they use: furi_check/furi_assert/
 * furi_crash, FURI_LOG_*, and FuriString as a fixed char buffer
 * (LFRFID_STR_MAX bytes; longer text is cut, never overrun).  malloc() is
 * the stock firmware's: zeroed memory, never NULL (the process ends with a
 * message when the heap is out), as libfuri's furi.h.
 * Not libfuri itself: `rfid` is a plain command, no threads, no records. */
#ifndef LFRFID_SHIM_FURI_H
#define LFRFID_SHIM_FURI_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <core/check.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LFRFID_STR_MAX
#define LFRFID_STR_MAX 256
#endif

typedef struct FuriString {
    size_t len;
    char buf[LFRFID_STR_MAX];
} FuriString;

FuriString* furi_string_alloc(void);
void furi_string_free(FuriString* s);
void furi_string_reset(FuriString* s);
const char* furi_string_get_cstr(const FuriString* s);
size_t furi_string_size(const FuriString* s);
void furi_string_set(FuriString* s, FuriString* source);
void furi_string_set_str(FuriString* s, const char* source);
void furi_string_cat(FuriString* s, const FuriString* source);
void furi_string_cat_str(FuriString* s, const char* source);
int furi_string_printf(FuriString* s, const char* format, ...)
    __attribute__((format(printf, 2, 3)));
int furi_string_cat_printf(FuriString* s, const char* format, ...)
    __attribute__((format(printf, 2, 3)));
int furi_string_cat_vprintf(FuriString* s, const char* format, va_list ap);

/* As stock: a C string or a FuriString may be given (stock uses _Generic). */
#define furi_string_cat(s, x)                    \
    _Generic((x),                                \
        char*: furi_string_cat_str,              \
        const char*: furi_string_cat_str,        \
        default: furi_string_cat)(s, x)
#define furi_string_set(s, x)                    \
    _Generic((x),                                \
        char*: furi_string_set_str,              \
        const char*: furi_string_set_str,        \
        default: furi_string_set)(s, x)

/* Logging: the decoders' debug lines are not wanted in a command's output. */
#define FURI_LOG_E(tag, ...) ((void)(tag))
#define FURI_LOG_W(tag, ...) ((void)(tag))
#define FURI_LOG_I(tag, ...) ((void)(tag))
#define FURI_LOG_D(tag, ...) ((void)(tag))
#define FURI_LOG_T(tag, ...) ((void)(tag))

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif
#ifndef COUNT_OF
#define COUNT_OF(x) (sizeof(x) / sizeof(x[0]))
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/* The heap the library took so far (bytes asked for; the R9.0.1 budget). */
extern size_t lfrfid_heap_bytes;
void* lfrfid_malloc(size_t size);
#define malloc(size) lfrfid_malloc(size)

#ifdef __cplusplus
}
#endif

#endif
