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

void furi_string_reserve(FuriString* s, size_t size) {
    (void)s;
    (void)size;
}

bool flipper_format_write_empty_line(FlipperFormat* ff) {
    (void)ff;
    return false;
}
