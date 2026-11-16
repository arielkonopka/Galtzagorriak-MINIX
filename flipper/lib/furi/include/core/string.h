/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri (spec R5.1): FuriString (stock furi/core/string.h). The stock
 * one wraps M*LIB's string_t; this one is a plain heap string with the
 * same functions and the same type-generic macros (_Generic), so a call
 * with a C string or a FuriString picks the right function.
 * Indexes are bytes. FURI_STRING_FAILURE is "not found". */
#ifndef FURI_CORE_STRING_H
#define FURI_CORE_STRING_H

#include <stdarg.h>
#include "core/base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FURI_STRING_FAILURE ((size_t)-1)

typedef struct FuriString FuriString;

FuriString* furi_string_alloc(void);
FuriString* furi_string_alloc_set(const FuriString* source);
FuriString* furi_string_alloc_set_str(const char cstr_source[]);
FuriString* furi_string_alloc_printf(const char format[], ...)
    __attribute__((format(printf, 1, 2)));
FuriString* furi_string_alloc_vprintf(const char format[], va_list args);
FuriString* furi_string_alloc_move(FuriString* source);
void furi_string_free(FuriString* string);

void furi_string_reserve(FuriString* string, size_t size);
void furi_string_reset(FuriString* string);
void furi_string_swap(FuriString* string_1, FuriString* string_2);
void furi_string_move(FuriString* destination, FuriString* source);
size_t furi_string_hash(const FuriString* string);
size_t furi_string_size(const FuriString* string);
bool furi_string_empty(const FuriString* string);

char furi_string_get_char(const FuriString* string, size_t index);
const char* furi_string_get_cstr(const FuriString* string);

void furi_string_set(FuriString* string, FuriString* source);
void furi_string_set_str(FuriString* string, const char source[]);
void furi_string_set_strn(FuriString* string, const char source[], size_t length);
void furi_string_set_char(FuriString* string, size_t index, const char c);
void furi_string_set_n(FuriString* string, const FuriString* source, size_t offset, size_t length);
int furi_string_printf(FuriString* string, const char format[], ...)
    __attribute__((format(printf, 2, 3)));
int furi_string_vprintf(FuriString* string, const char format[], va_list args);

void furi_string_push_back(FuriString* string, char c);
void furi_string_cat(FuriString* string_1, const FuriString* string_2);
void furi_string_cat_str(FuriString* string_1, const char cstring_2[]);
int furi_string_cat_printf(FuriString* string, const char format[], ...)
    __attribute__((format(printf, 2, 3)));
int furi_string_cat_vprintf(FuriString* string, const char format[], va_list args);

int furi_string_cmp(const FuriString* string_1, const FuriString* string_2);
int furi_string_cmp_str(const FuriString* string_1, const char cstring_2[]);
int furi_string_cmpi(const FuriString* string_1, const FuriString* string_2);
int furi_string_cmpi_str(const FuriString* string_1, const char cstring_2[]);
bool furi_string_equal(const FuriString* string_1, const FuriString* string_2);
bool furi_string_equal_str(const FuriString* string_1, const char cstring_2[]);

size_t furi_string_search(const FuriString* string, const FuriString* needle, size_t start);
size_t furi_string_search_str(const FuriString* string, const char needle[], size_t start);
size_t furi_string_search_char(const FuriString* string, char c, size_t start);
size_t furi_string_search_rchar(const FuriString* string, char c, size_t start);

void furi_string_replace_at(
    FuriString* string,
    size_t pos,
    size_t len,
    const char replace[]);
size_t furi_string_replace(
    FuriString* string,
    FuriString* needle,
    FuriString* replace,
    size_t start);
size_t furi_string_replace_str(
    FuriString* string,
    const char needle[],
    const char replace[],
    size_t start);
void furi_string_replace_all(
    FuriString* string,
    const FuriString* needle,
    const FuriString* replace);
void furi_string_replace_all_str(FuriString* string, const char needle[], const char replace[]);

bool furi_string_start_with(const FuriString* string, const FuriString* start);
bool furi_string_start_with_str(const FuriString* string, const char start[]);
bool furi_string_end_with(const FuriString* string, const FuriString* end);
bool furi_string_end_with_str(const FuriString* string, const char end[]);

void furi_string_left(FuriString* string, size_t index);
void furi_string_right(FuriString* string, size_t index);
void furi_string_mid(FuriString* string, size_t index, size_t size);
void furi_string_trim(FuriString* string, const char chars[]);

/* UTF-8 */
typedef unsigned int FuriStringUnicodeValue;
typedef enum {
    FuriStringUTF8StateStarting,
    FuriStringUTF8StateDecoding1,
    FuriStringUTF8StateDecoding2,
    FuriStringUTF8StateDecoding3,
    FuriStringUTF8StateError,
} FuriStringUTF8State;
size_t furi_string_utf8_length(FuriString* string);
void furi_string_utf8_push(FuriString* string, FuriStringUnicodeValue unicode);
void furi_string_utf8_decode(char c, FuriStringUTF8State* state, FuriStringUnicodeValue* unicode);

/* Type-generic forms, as the stock header. */
#define FURI_STRING_SELECT(n, a, ...)                                       \
    _Generic((a),                                                           \
        char*: n##_str,                                                     \
        const char*: n##_str,                                               \
        FuriString*: n,                                                     \
        const FuriString*: n)(__VA_ARGS__)
#define __FURI_S_SEL3(_1, _2, _3, NAME, ...) NAME
#define __FURI_S_SEL4(_1, _2, _3, _4, NAME, ...) NAME

#define furi_string_alloc_set(a) FURI_STRING_SELECT(furi_string_alloc_set, a, a)
#define furi_string_set(a, b) FURI_STRING_SELECT(furi_string_set, b, a, b)
#define furi_string_cmp(a, b) FURI_STRING_SELECT(furi_string_cmp, b, a, b)
#define furi_string_cmpi(a, b) FURI_STRING_SELECT(furi_string_cmpi, b, a, b)
#define furi_string_equal(a, b) FURI_STRING_SELECT(furi_string_equal, b, a, b)
#define furi_string_replace_all(a, b, c) FURI_STRING_SELECT(furi_string_replace_all, b, a, b, c)
#define furi_string_start_with(a, b) FURI_STRING_SELECT(furi_string_start_with, b, a, b)
#define furi_string_end_with(a, b) FURI_STRING_SELECT(furi_string_end_with, b, a, b)
#define furi_string_cat(a, b) FURI_STRING_SELECT(furi_string_cat, b, a, b)

/* Optional last arguments: start = 0, chars = " \n\r\t". */
#define __furi_string_search2(v, n) FURI_STRING_SELECT(furi_string_search, n, v, n, 0)
#define __furi_string_search3(v, n, s) FURI_STRING_SELECT(furi_string_search, n, v, n, s)
#define furi_string_search(...) \
    __FURI_S_SEL3(__VA_ARGS__, __furi_string_search3, __furi_string_search2, 0)(__VA_ARGS__)
#define __furi_string_search_str2(v, n) furi_string_search_str(v, n, 0)
#define __furi_string_search_str3(v, n, s) furi_string_search_str(v, n, s)
#define furi_string_search_str(...)                                                         \
    __FURI_S_SEL3(__VA_ARGS__, __furi_string_search_str3, __furi_string_search_str2, 0)( \
        __VA_ARGS__)
#define __furi_string_search_char2(v, c) furi_string_search_char(v, c, 0)
#define __furi_string_search_char3(v, c, s) furi_string_search_char(v, c, s)
#define furi_string_search_char(...)                                                          \
    __FURI_S_SEL3(__VA_ARGS__, __furi_string_search_char3, __furi_string_search_char2, 0)( \
        __VA_ARGS__)
#define __furi_string_search_rchar2(v, c) furi_string_search_rchar(v, c, 0)
#define __furi_string_search_rchar3(v, c, s) furi_string_search_rchar(v, c, s)
#define furi_string_search_rchar(...)                                                           \
    __FURI_S_SEL3(__VA_ARGS__, __furi_string_search_rchar3, __furi_string_search_rchar2, 0)( \
        __VA_ARGS__)
#define __furi_string_replace3(v, n, r) FURI_STRING_SELECT(furi_string_replace, n, v, n, r, 0)
#define __furi_string_replace4(v, n, r, s) FURI_STRING_SELECT(furi_string_replace, n, v, n, r, s)
#define furi_string_replace(...) \
    __FURI_S_SEL4(__VA_ARGS__, __furi_string_replace4, __furi_string_replace3, 0, 0)(__VA_ARGS__)
#define __furi_string_replace_str3(v, n, r) furi_string_replace_str(v, n, r, 0)
#define __furi_string_replace_str4(v, n, r, s) furi_string_replace_str(v, n, r, s)
#define furi_string_replace_str(...)                                                          \
    __FURI_S_SEL4(__VA_ARGS__, __furi_string_replace_str4, __furi_string_replace_str3, 0, 0)( \
        __VA_ARGS__)
#define __furi_string_trim1(v) furi_string_trim(v, " \n\r\t")
#define __furi_string_trim2(v, c) furi_string_trim(v, c)
#define furi_string_trim(...) \
    __FURI_S_SEL3(__VA_ARGS__, 0, __furi_string_trim2, __furi_string_trim1, 0)(__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif
