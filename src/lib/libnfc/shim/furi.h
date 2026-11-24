/* SPDX-License-Identifier: GPL-3.0-or-later
 * libnfc poller shim (issue #404): the small part of furi the vendored
 * Unleashed NFC protocol files (src/lib/nfc) use, so they compile
 * unchanged.  No threads, no RTOS objects, no records: checks end the
 * program with the place, logs are dropped, delays are no-ops (timing is
 * the transceiver's), FuriString is a capped heap string.  malloc() is the
 * stock firmware's: zeroed, never NULL (the vendored code relies on both).
 * The same idea as src/lib/subghz/shim (#302), kept separate so the two
 * libraries can be built alone. */
#ifndef LIBNFC_SHIM_FURI_H
#define LIBNFC_SHIM_FURI_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <ctype.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NFCSHIM_STR_MAX 1024	/* longer text is cut, never overrun */

typedef struct FuriString {
    char* buf;
    size_t len;
} FuriString;

FuriString* furi_string_alloc(void);
FuriString* furi_string_alloc_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
FuriString* furi_string_alloc_set_str(const char* s);
void furi_string_free(FuriString* s);
void furi_string_reset(FuriString* s);
const char* furi_string_get_cstr(const FuriString* s);
size_t furi_string_size(const FuriString* s);
bool furi_string_empty(const FuriString* s);
void furi_string_set_str(FuriString* s, const char* str);
void furi_string_set_s(FuriString* s, const FuriString* src);
void furi_string_cat_str(FuriString* s, const char* str);
void furi_string_cat_s(FuriString* s, const FuriString* src);
void furi_string_push_back(FuriString* s, char c);
int furi_string_cmp_str(const FuriString* s, const char* str);
bool furi_string_equal_str(const FuriString* s, const char* str);
bool furi_string_equal_s(const FuriString* s, const FuriString* t);
int furi_string_printf(FuriString* s, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
int furi_string_cat_printf(FuriString* s, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
int furi_string_vprintf(FuriString* s, const char* fmt, va_list ap);
int furi_string_cat_vprintf(FuriString* s, const char* fmt, va_list ap);
char furi_string_get_char(const FuriString* s, size_t i);
void furi_string_left(FuriString* s, size_t len);

#define furi_string_cat(s, x) \
    _Generic((x), char*: furi_string_cat_str, const char*: furi_string_cat_str, default: furi_string_cat_s)(s, x)
#define furi_string_set(s, x) \
    _Generic((x), char*: furi_string_set_str, const char*: furi_string_set_str, default: furi_string_set_s)(s, x)
#define furi_string_equal(s, x) \
    _Generic((x), char*: furi_string_equal_str, const char*: furi_string_equal_str, default: furi_string_equal_s)(s, x)
#define furi_string_alloc_set(x) furi_string_alloc_set_str(x)

void nfcshim_crash(const char* msg, const char* file, int line) __attribute__((noreturn));
#define furi_crash(...)    nfcshim_crash("" __VA_ARGS__, __FILE__, __LINE__)
#define furi_check(x, ...) ((x) ? (void)0 : nfcshim_crash("furi_check failed: " #x, __FILE__, __LINE__))
#define furi_assert(x, ...) furi_check(x)

#define FURI_LOG_E(tag, ...) ((void)(tag))
#define FURI_LOG_W(tag, ...) ((void)(tag))
#define FURI_LOG_I(tag, ...) ((void)(tag))
#define FURI_LOG_D(tag, ...) ((void)(tag))
#define FURI_LOG_T(tag, ...) ((void)(tag))
/* Log level: never Trace, so the EMV poller's APDU trace (card data) is
 * never printed (#412). */
typedef enum {
    FuriLogLevelDefault, FuriLogLevelNone, FuriLogLevelError, FuriLogLevelWarn,
    FuriLogLevelInfo, FuriLogLevelDebug, FuriLogLevelTrace,
} FuriLogLevel;
static inline FuriLogLevel furi_log_get_level(void) { return FuriLogLevelNone; }
#define FURI_LOG_RAW_D(...)  ((void)0)
#define FURI_LOG_RAW_T(...)  ((void)0)
#define FURI_LOG_RAW_I(...)  ((void)0)

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
#define EXT_PATH(p) "/ext/" p
#define REVERSE_BYTES_U32(x) __builtin_bswap32(x)
#define FURI_PACKED __attribute__((packed))
#define FURI_BIT(x, n) (((x) >> (n)) & 1)
#define FURI_BIT_SET(x, n) ((x) |= (1UL << (n)))
#define FURI_BIT_CLEAR(x, n) ((x) &= ~(1UL << (n)))
#define FURI_SWAP(x, y)        \
    do {                       \
        __typeof__(x) _t = x;  \
        x = y;                 \
        y = _t;                \
    } while(0)
#define FURI_WARN_UNUSED __attribute__((warn_unused_result))

typedef enum {
    FuriWaitForever = 0xFFFFFFFFU,
} FuriWait;

/* Thread flags without threads (#404): nfc_start() runs the poller chain
 * in the caller and returns, so by the time a vendored sync wrapper waits,
 * the callback has already set its flag.  One word for the one caller;
 * waiting on a flag nobody set returns FuriFlagErrorTimeout at once
 * instead of blocking. */
typedef void* FuriThreadId;
#define FuriFlagWaitAny 0x0U
#define FuriFlagWaitAll 0x1U
#define FuriFlagNoClear 0x2U
#define FuriFlagErrorTimeout 0xFFFFFFFEU
FuriThreadId furi_thread_get_current_id(void);
uint32_t furi_thread_flags_set(FuriThreadId id, uint32_t flags);
uint32_t furi_thread_flags_clear(uint32_t flags);
uint32_t furi_thread_flags_wait(uint32_t flags, uint32_t options, uint32_t timeout);
void furi_string_trim_chars(FuriString* s, const char* chars);
/* FeliCa's file loader (#412). */
#define FURI_STRING_FAILURE ((size_t)-1)
size_t nfcshim_str_search(const FuriString* s, const char* needle, size_t start);
#define furi_string_search_str(s, n, ...) nfcshim_str_search(s, n, (0, ##__VA_ARGS__))
void furi_string_mid(FuriString* s, size_t start, size_t len);
void furi_string_replace_all(FuriString* s, const char* from, const char* to);
/* The first `from` at or after start becomes `to` (NTAG4xx's short name,
 * #451); returns its index or FURI_STRING_FAILURE. */
size_t nfcshim_str_replace(FuriString* s, const char* from, const char* to, size_t start);
#define furi_string_replace(s, f, t, ...) nfcshim_str_replace(s, f, t, (0, ##__VA_ARGS__))
/* Last c in s, or FURI_STRING_FAILURE (stock's optional start is not used). */
size_t furi_string_search_rchar(const FuriString* s, char c, ...);
/* Code points in the (UTF-8) string. */
size_t furi_string_utf8_length(FuriString* s);
/* The buffer is fixed (NFCSHIM_STR_MAX): nothing to reserve. */
void furi_string_reserve(FuriString* s, size_t size);
#define furi_string_trim_1(s) furi_string_trim_chars(s, " \n\r\t")
#define furi_string_trim_2(s, c) furi_string_trim_chars(s, c)
#define furi_string_trim_x(a, b, f, ...) f
#define furi_string_trim(...) \
    furi_string_trim_x(__VA_ARGS__, furi_string_trim_2, furi_string_trim_1, )(__VA_ARGS__)

void furi_delay_ms(uint32_t ms);
void furi_delay_us(uint32_t us);
uint32_t furi_get_tick(void);

/* The heap this library took so far (bytes asked for). */
extern size_t nfcshim_heap_bytes;
void* nfcshim_malloc(size_t size);
#define malloc(size) nfcshim_malloc(size)

#ifdef __cplusplus
}
#endif
#endif
