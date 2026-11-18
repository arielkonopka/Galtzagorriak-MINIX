/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri (spec R5.1): FuriString (stock furi/core/string.h), a heap
 * string. An empty string has no buffer (12 B header only). Written for
 * Galtzagorriak MINIX (the stock one wraps M*LIB). */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "furi_i.h"

/* The header's type-generic macros would rewrite the definitions. */
#undef furi_string_alloc_set
#undef furi_string_set
#undef furi_string_cmp
#undef furi_string_cmpi
#undef furi_string_equal
#undef furi_string_replace_all
#undef furi_string_start_with
#undef furi_string_end_with
#undef furi_string_cat
#undef furi_string_search
#undef furi_string_search_str
#undef furi_string_search_char
#undef furi_string_search_rchar
#undef furi_string_replace
#undef furi_string_replace_str
#undef furi_string_trim

struct FuriString {
    char* ptr; /* NULL while cap == 0 */
    size_t size;
    size_t cap; /* bytes usable for characters (the NUL is extra) */
};

static char empty[1];

static char* str_of(const FuriString* s) {
    return s->ptr ? s->ptr : empty;
}

static void grow(FuriString* s, size_t need) {
    size_t cap;
    char* p;

    if(need <= s->cap) return;
    cap = s->cap ? s->cap : 15;
    while(cap < need)
        cap = cap * 2 + 1;
    p = realloc(s->ptr, cap + 1);
    furi_check(p, "furi_string: out of memory");
    if(!s->ptr) p[0] = '\0';
    s->ptr = p;
    s->cap = cap;
}

/* set the contents to n bytes at src (src may point into s) */
static void assign(FuriString* s, const char* src, size_t n) {
    if(n == 0) {
        if(s->ptr) s->ptr[0] = '\0';
        s->size = 0;
        return;
    }
    if(n > s->cap) {
        if(s->ptr && src >= s->ptr && src <= s->ptr + s->size) {
            size_t off = (size_t)(src - s->ptr);
            grow(s, n);
            src = s->ptr + off;
        } else {
            grow(s, n);
        }
    }
    memmove(s->ptr, src, n);
    s->ptr[n] = '\0';
    s->size = n;
}

static void append(FuriString* s, const char* src, size_t n) {
    if(n == 0) return;
    if(s->size + n > s->cap) {
        if(s->ptr && src >= s->ptr && src <= s->ptr + s->size) {
            size_t off = (size_t)(src - s->ptr);
            grow(s, s->size + n);
            src = s->ptr + off;
        } else {
            grow(s, s->size + n);
        }
    }
    memmove(s->ptr + s->size, src, n);
    s->size += n;
    s->ptr[s->size] = '\0';
}

FuriString* furi_string_alloc(void) {
    FuriString* s = calloc(1, sizeof(FuriString));
    furi_check(s, "furi_string: out of memory");
    return s;
}

FuriString* furi_string_alloc_set(const FuriString* source) {
    FuriString* s = furi_string_alloc();
    assign(s, str_of(source), source->size);
    return s;
}

FuriString* furi_string_alloc_set_str(const char cstr[]) {
    FuriString* s = furi_string_alloc();
    assign(s, cstr, strlen(cstr));
    return s;
}

FuriString* furi_string_alloc_printf(const char format[], ...) {
    va_list ap;
    FuriString* s = furi_string_alloc();

    va_start(ap, format);
    furi_string_vprintf(s, format, ap);
    va_end(ap);
    return s;
}

FuriString* furi_string_alloc_vprintf(const char format[], va_list args) {
    FuriString* s = furi_string_alloc();
    furi_string_vprintf(s, format, args);
    return s;
}

FuriString* furi_string_alloc_move(FuriString* source) {
    FuriString* s = furi_string_alloc();
    furi_string_move(s, source);
    return s;
}

void furi_string_free(FuriString* s) {
    if(!s) return;
    free(s->ptr);
    free(s);
}

void furi_string_reserve(FuriString* s, size_t size) {
    grow(s, size);
}

void furi_string_reset(FuriString* s) {
    free(s->ptr);
    s->ptr = NULL;
    s->size = 0;
    s->cap = 0;
}

void furi_string_swap(FuriString* a, FuriString* b) {
    FuriString t = *a;
    *a = *b;
    *b = t;
}

void furi_string_move(FuriString* dst, FuriString* src) {
    free(dst->ptr);
    *dst = *src;
    free(src);
}

size_t furi_string_hash(const FuriString* s) {
    /* FNV-1a */
    uint32_t h = 2166136261UL;
    const unsigned char* p = (const unsigned char*)str_of(s);
    size_t i;

    for(i = 0; i < s->size; i++)
        h = (h ^ p[i]) * 16777619UL;
    return (size_t)h;
}

size_t furi_string_size(const FuriString* s) {
    return s->size;
}

bool furi_string_empty(const FuriString* s) {
    return s->size == 0;
}

char furi_string_get_char(const FuriString* s, size_t index) {
    furi_check(index < s->size, "furi_string_get_char: index out of range");
    return s->ptr[index];
}

const char* furi_string_get_cstr(const FuriString* s) {
    return str_of(s);
}

void furi_string_set(FuriString* s, FuriString* source) {
    if(s != source) assign(s, str_of(source), source->size);
}

void furi_string_set_str(FuriString* s, const char source[]) {
    assign(s, source, strlen(source));
}

void furi_string_set_strn(FuriString* s, const char source[], size_t length) {
    size_t n = 0;

    while(n < length && source[n])
        n++;
    assign(s, source, n);
}

void furi_string_set_char(FuriString* s, size_t index, const char c) {
    furi_check(index < s->size, "furi_string_set_char: index out of range");
    s->ptr[index] = c;
}

void furi_string_set_n(FuriString* s, const FuriString* source, size_t offset, size_t length) {
    furi_check(offset <= source->size, "furi_string_set_n: offset out of range");
    if(length > source->size - offset) length = source->size - offset;
    assign(s, str_of(source) + offset, length);
}

/* Format at s[at..]: grow and try again until it fits. */
static int format_at(FuriString* s, size_t at, const char* format, va_list args) {
    va_list ap;
    int r;

    for(;;) {
        if(s->cap < at + 15) grow(s, at + 15);
        va_copy(ap, args);
        r = fport_vformat(s->ptr + at, s->cap + 1 - at, format, ap);
        va_end(ap);
        if(r >= 0) break;
        grow(s, s->cap * 2 + 1);
    }
    s->size = at + (size_t)r;
    return r;
}

int furi_string_vprintf(FuriString* s, const char format[], va_list args) {
    return format_at(s, 0, format, args);
}

int furi_string_printf(FuriString* s, const char format[], ...) {
    va_list ap;
    int r;

    va_start(ap, format);
    r = format_at(s, 0, format, ap);
    va_end(ap);
    return r;
}

int furi_string_cat_vprintf(FuriString* s, const char format[], va_list args) {
    return format_at(s, s->size, format, args);
}

int furi_string_cat_printf(FuriString* s, const char format[], ...) {
    va_list ap;
    int r;

    va_start(ap, format);
    r = format_at(s, s->size, format, ap);
    va_end(ap);
    return r;
}

void furi_string_push_back(FuriString* s, char c) {
    append(s, &c, 1);
}

void furi_string_cat(FuriString* a, const FuriString* b) {
    append(a, str_of(b), b->size);
}

void furi_string_cat_str(FuriString* a, const char b[]) {
    append(a, b, strlen(b));
}

int furi_string_cmp(const FuriString* a, const FuriString* b) {
    return strcmp(str_of(a), str_of(b));
}

int furi_string_cmp_str(const FuriString* a, const char b[]) {
    return strcmp(str_of(a), b);
}

static int cmpi(const char* a, const char* b) {
    int ca, cb;

    do {
        ca = tolower((unsigned char)*a++);
        cb = tolower((unsigned char)*b++);
    } while(ca == cb && ca != 0);
    return ca - cb;
}

int furi_string_cmpi(const FuriString* a, const FuriString* b) {
    return cmpi(str_of(a), str_of(b));
}

int furi_string_cmpi_str(const FuriString* a, const char b[]) {
    return cmpi(str_of(a), b);
}

bool furi_string_equal(const FuriString* a, const FuriString* b) {
    return a->size == b->size && memcmp(str_of(a), str_of(b), a->size) == 0;
}

bool furi_string_equal_str(const FuriString* a, const char b[]) {
    return strcmp(str_of(a), b) == 0;
}

static size_t search(const FuriString* s, const char* needle, size_t start) {
    const char* p;

    if(start > s->size) return FURI_STRING_FAILURE;
    p = strstr(str_of(s) + start, needle);
    return p ? (size_t)(p - str_of(s)) : FURI_STRING_FAILURE;
}

size_t furi_string_search(const FuriString* s, const FuriString* needle, size_t start) {
    return search(s, str_of(needle), start);
}

size_t furi_string_search_str(const FuriString* s, const char needle[], size_t start) {
    return search(s, needle, start);
}

size_t furi_string_search_char(const FuriString* s, char c, size_t start) {
    const char* p;

    if(start >= s->size) return FURI_STRING_FAILURE;
    p = memchr(s->ptr + start, c, s->size - start);
    return p ? (size_t)(p - s->ptr) : FURI_STRING_FAILURE;
}

size_t furi_string_search_rchar(const FuriString* s, char c, size_t start) {
    size_t i = s->size;

    /* the last c at or after start */
    while(i > start) {
        i--;
        if(s->ptr[i] == c) return i;
    }
    return FURI_STRING_FAILURE;
}

void furi_string_replace_at(FuriString* s, size_t pos, size_t len, const char replace[]) {
    size_t rlen = strlen(replace), tail;

    furi_check(pos <= s->size, "furi_string_replace_at: position out of range");
    if(len > s->size - pos) len = s->size - pos;
    tail = s->size - pos - len;
    if(rlen > len) {
        if(s->ptr && replace >= s->ptr && replace <= s->ptr + s->size) {
            /* replace points into s: copy it first */
            FuriString* t = furi_string_alloc_set_str(replace);
            furi_string_replace_at(s, pos, len, str_of(t));
            furi_string_free(t);
            return;
        }
        grow(s, s->size + rlen - len);
    }
    if(s->ptr) {
        memmove(s->ptr + pos + rlen, s->ptr + pos + len, tail + 1);
        memcpy(s->ptr + pos, replace, rlen);
    }
    s->size = s->size - len + rlen;
}

size_t furi_string_replace_str(
    FuriString* s,
    const char needle[],
    const char replace[],
    size_t start) {
    size_t at = search(s, needle, start);

    if(at != FURI_STRING_FAILURE) furi_string_replace_at(s, at, strlen(needle), replace);
    return at;
}

size_t furi_string_replace(FuriString* s, FuriString* needle, FuriString* replace, size_t start) {
    return furi_string_replace_str(s, str_of(needle), str_of(replace), start);
}

void furi_string_replace_all_str(FuriString* s, const char needle[], const char replace[]) {
    size_t at = 0, nlen = strlen(needle), rlen = strlen(replace);

    if(nlen == 0) return;
    while((at = search(s, needle, at)) != FURI_STRING_FAILURE) {
        furi_string_replace_at(s, at, nlen, replace);
        at += rlen;
    }
}

void furi_string_replace_all(FuriString* s, const FuriString* needle, const FuriString* replace) {
    furi_string_replace_all_str(s, str_of(needle), str_of(replace));
}

bool furi_string_start_with_str(const FuriString* s, const char start[]) {
    size_t n = strlen(start);
    return n <= s->size && memcmp(str_of(s), start, n) == 0;
}

bool furi_string_start_with(const FuriString* s, const FuriString* start) {
    return furi_string_start_with_str(s, str_of(start));
}

bool furi_string_end_with_str(const FuriString* s, const char end[]) {
    size_t n = strlen(end);
    return n <= s->size && memcmp(str_of(s) + s->size - n, end, n) == 0;
}

bool furi_string_end_with(const FuriString* s, const FuriString* end) {
    return furi_string_end_with_str(s, str_of(end));
}

void furi_string_left(FuriString* s, size_t index) {
    if(index < s->size) {
        s->size = index;
        s->ptr[index] = '\0';
    }
}

void furi_string_right(FuriString* s, size_t index) {
    if(index >= s->size) {
        assign(s, "", 0);
        return;
    }
    assign(s, s->ptr + index, s->size - index);
}

void furi_string_mid(FuriString* s, size_t index, size_t size) {
    if(index >= s->size) {
        assign(s, "", 0);
        return;
    }
    if(size > s->size - index) size = s->size - index;
    assign(s, s->ptr + index, size);
}

void furi_string_trim(FuriString* s, const char chars[]) {
    size_t a = 0, b = s->size;

    while(a < b && strchr(chars, s->ptr[a]))
        a++;
    while(b > a && strchr(chars, s->ptr[b - 1]))
        b--;
    if(a == 0) {
        furi_string_left(s, b);
        return;
    }
    assign(s, s->ptr + a, b - a);
}

/* ---------------------------------------------------------------- UTF-8 */

size_t furi_string_utf8_length(FuriString* s) {
    size_t i, n = 0;

    for(i = 0; i < s->size; i++)
        if(((unsigned char)s->ptr[i] & 0xC0) != 0x80) n++;
    return n;
}

void furi_string_utf8_push(FuriString* s, FuriStringUnicodeValue u) {
    char b[4];
    size_t n;

    if(u < 0x80) {
        b[0] = (char)u;
        n = 1;
    } else if(u < 0x800) {
        b[0] = (char)(0xC0 | (u >> 6));
        b[1] = (char)(0x80 | (u & 0x3F));
        n = 2;
    } else if(u < 0x10000) {
        b[0] = (char)(0xE0 | (u >> 12));
        b[1] = (char)(0x80 | ((u >> 6) & 0x3F));
        b[2] = (char)(0x80 | (u & 0x3F));
        n = 3;
    } else {
        b[0] = (char)(0xF0 | ((u >> 18) & 0x07));
        b[1] = (char)(0x80 | ((u >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((u >> 6) & 0x3F));
        b[3] = (char)(0x80 | (u & 0x3F));
        n = 4;
    }
    append(s, b, n);
}

void furi_string_utf8_decode(char c, FuriStringUTF8State* state, FuriStringUnicodeValue* unicode) {
    unsigned char b = (unsigned char)c;

    switch(*state) {
    case FuriStringUTF8StateStarting:
    case FuriStringUTF8StateError:
        if(b < 0x80) {
            *unicode = b;
            *state = FuriStringUTF8StateStarting;
        } else if((b & 0xE0) == 0xC0) {
            *unicode = b & 0x1F;
            *state = FuriStringUTF8StateDecoding1;
        } else if((b & 0xF0) == 0xE0) {
            *unicode = b & 0x0F;
            *state = FuriStringUTF8StateDecoding2;
        } else if((b & 0xF8) == 0xF0) {
            *unicode = b & 0x07;
            *state = FuriStringUTF8StateDecoding3;
        } else {
            *state = FuriStringUTF8StateError;
        }
        break;
    default:
        if((b & 0xC0) != 0x80) {
            *state = FuriStringUTF8StateError;
            break;
        }
        *unicode = (*unicode << 6) | (b & 0x3F);
        *state = (FuriStringUTF8State)(*state - 1);
        /* Decoding1 - 1 == Starting: a whole character */
        break;
    }
}

size_t fdebug_sizeof_string(void) {
    return sizeof(FuriString);
}
