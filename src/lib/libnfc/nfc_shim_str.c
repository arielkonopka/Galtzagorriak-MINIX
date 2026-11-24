/* nfc_shim_str.c - the FuriString and FlipperFormat calls the part-5
 * protocols (FeliCa, ST25TB, EMV) use past nfc_shim.c (#412).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #412.
 */
#include <furi.h>
#include <flipper_format/flipper_format.h>

size_t nfcshim_str_search(const FuriString* s, const char* needle, size_t start) {
    if(start > s->len) return FURI_STRING_FAILURE;
    const char* p = strstr(s->buf + start, needle);
    return p ? (size_t)(p - s->buf) : FURI_STRING_FAILURE;
}

void furi_string_mid(FuriString* s, size_t start, size_t len) {
    if(start >= s->len) {
        furi_string_reset(s);
        return;
    }
    if(len > s->len - start) len = s->len - start;
    memmove(s->buf, s->buf + start, len);
    s->len = len;
    s->buf[len] = 0;
}

size_t nfcshim_str_replace(FuriString* s, const char* from, const char* to, size_t start) {
    size_t fl = strlen(from), at;
    const char* q;
    if(!fl || start > s->len || (q = strstr(s->buf + start, from)) == NULL)
        return FURI_STRING_FAILURE;
    at = (size_t)(q - s->buf);
    FuriString* out = furi_string_alloc();
    furi_string_set_str(out, s->buf);
    furi_string_left(out, at);
    furi_string_cat_str(out, to);
    furi_string_cat_str(out, s->buf + at + fl);
    furi_string_set_s(s, out);
    furi_string_free(out);
    return at;
}

void furi_string_replace_all(FuriString* s, const char* from, const char* to) {
    size_t fl = strlen(from), tl = strlen(to);
    if(!fl) return;
    FuriString* out = furi_string_alloc();
    const char* p = s->buf;
    const char* q;
    while((q = strstr(p, from)) != NULL) {
        size_t n = (size_t)(q - p);
        if(n > NFCSHIM_STR_MAX - out->len) n = NFCSHIM_STR_MAX - out->len;
        memcpy(out->buf + out->len, p, n);
        out->len += n;
        out->buf[out->len] = 0;
        if(tl) furi_string_cat_str(out, to);
        p = q + fl;
    }
    furi_string_cat_str(out, p);
    furi_string_set_s(s, out);
    furi_string_free(out);
}

size_t furi_string_search_rchar(const FuriString* s, char c, ...) {
    const char* p = strrchr(s->buf, c);
    return p ? (size_t)(p - s->buf) : FURI_STRING_FAILURE;
}

size_t furi_string_utf8_length(FuriString* s) {
    size_t n = 0;
    for(size_t i = 0; i < s->len; i++)
        if(((unsigned char)s->buf[i] & 0xC0) != 0x80) n++;
    return n;
}

void furi_string_reserve(FuriString* s, size_t size) {
    (void)s;
    (void)size;
}

bool flipper_format_write_empty_line(FlipperFormat* ff) {
    (void)ff;
    return false;
}
