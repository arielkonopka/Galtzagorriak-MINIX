/* SPDX-License-Identifier: GPL-3.0-or-later
 * libsubghz (spec Phase 9, R8.2; issue #302): the thin furi shim the
 * Unleashed Sub-GHz protocol files are compiled against.  Only what they
 * use: furi_check/furi_assert/furi_crash, FURI_LOG_*, FuriString as a heap
 * string with a hard cap (SUBGHZ_STR_MAX bytes; longer text is cut, never
 * overrun) and its own printf (MINIX 2.0's doprnt has no %ll/%z), records
 * and delays as no-ops.  malloc() is the stock firmware's: zeroed memory,
 * never NULL (the process ends with a message when the heap is out).
 * Not libfuri itself: `subghz` is a plain command, no threads, no records.
 * The same idea as src/lib/lfrfid/shim (T19), with growing strings: the
 * keystore holds one name per manufacturer key, a fixed 256 B each would
 * not fit. */
#ifndef SUBGHZ_SHIM_FURI_H
#define SUBGHZ_SHIM_FURI_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SUBGHZ_STR_MAX
#define SUBGHZ_STR_MAX 1024
#endif

typedef struct FuriString {
    char* buf;
    size_t len;
    size_t cap;
} FuriString;

FuriString* furi_string_alloc(void);
FuriString* furi_string_alloc_set_str(const char* s);
void furi_string_free(FuriString* s);
void furi_string_reset(FuriString* s);
const char* furi_string_get_cstr(const FuriString* s);
size_t furi_string_size(const FuriString* s);
bool furi_string_empty(const FuriString* s);
void furi_string_set_s(FuriString* s, const FuriString* source);
void furi_string_set_str(FuriString* s, const char* source);
void furi_string_set_n(FuriString* s, const FuriString* source, size_t start, size_t len);
void furi_string_cat_s(FuriString* s, const FuriString* source);
void furi_string_cat_str(FuriString* s, const char* source);
void furi_string_push_back(FuriString* s, char c);
void furi_string_trim(FuriString* s, const char* chars);
int furi_string_cmp_str(const FuriString* s, const char* str);
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
        default: furi_string_cat_s)(s, x)
#define furi_string_set(s, x)                    \
    _Generic((x),                                \
        char*: furi_string_set_str,              \
        const char*: furi_string_set_str,        \
        default: furi_string_set_s)(s, x)
#define furi_string_alloc_set(x) furi_string_alloc_set_str(x)
#define furi_string_trim(s, ...) furi_string_trim(s, " \n\r\t")

/* Checks: as stock, a failed one ends the program (with the place). */
void subghz_crash(const char* message, const char* file, int line) __attribute__((noreturn));
#define furi_crash(...)  subghz_crash("" __VA_ARGS__, __FILE__, __LINE__)
#define furi_check(x, ...) \
    ((x) ? (void)0 : subghz_crash("furi_check failed: " #x, __FILE__, __LINE__))
#define furi_assert(x, ...) furi_check(x)

/* Logging: the decoders' debug lines are not wanted in a command's output. */
#define FURI_LOG_E(tag, ...)  ((void)(tag))
#define FURI_LOG_W(tag, ...)  ((void)(tag))
#define FURI_LOG_I(tag, ...)  ((void)(tag))
#define FURI_LOG_D(tag, ...)  ((void)(tag))
#define FURI_LOG_T(tag, ...)  ((void)(tag))
#define FURI_LOG_RAW_D(...)   ((void)0)

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
#ifndef FURI_PACKED
#define FURI_PACKED __attribute__((packed))
#endif

/* The stock SD card root is /ext; MINIX's is the user's home (spec 9). */
#define EXT_PATH(path) "/home/flipper/" path
#define RECORD_STORAGE "storage"
typedef struct Storage Storage;
void* furi_record_open(const char* name);
void furi_record_close(const char* name);
void furi_delay_ms(uint32_t ms);
uint32_t furi_get_tick(void);

/* The heap the library took so far (bytes asked for; the budget). */
extern size_t subghz_heap_bytes;
void* subghz_malloc(size_t size);
#define malloc(size) subghz_malloc(size)

/* Stock's largest free heap block; Bin RAW takes its 10 KB capture buffers
 * only when this says they fit.  The shim answers subghz_heap_room (set by
 * the program; 0 turns Bin RAW's capture off). */
extern size_t subghz_heap_room;
size_t memmgr_heap_get_max_free_block(void);

/* MINIX 2.0's <math.h> is C89: roundf is the shim's.  Declared here (every
 * protocol includes furi.h; Princeton uses it without <math.h>). */
float roundf(float x);

#ifdef __cplusplus
}
#endif

#endif
