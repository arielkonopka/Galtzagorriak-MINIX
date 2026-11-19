/* SPDX-License-Identifier: GPL-3.0-or-later
 * libsubghz: FlipperFormat as a bounded text buffer.  See
 * lib/flipper_format/flipper_format.h.  Own code; the file syntax is the
 * stock one (lib/flipper_format/flipper_format_stream.c): "Key: value"
 * lines, '#' comments, numbers in decimal, hex bytes as "AA BB", bools as
 * "true"/"false". */
#include <lib/flipper_format/flipper_format.h>
#include <toolbox/hex.h>

struct FlipperFormat {
    char* text; /* NUL-terminated, len bytes */
    size_t len;
    size_t cap;
    size_t pos;
    bool changed;
    char path[128];
};

FlipperFormat* flipper_format_string_alloc(void) {
    FlipperFormat* ff = malloc(sizeof(FlipperFormat));

    ff->cap = 128;
    ff->text = malloc(ff->cap);
    return ff;
}

FlipperFormat* flipper_format_file_alloc(Storage* storage) {
    UNUSED(storage);
    return flipper_format_string_alloc();
}

void flipper_format_free(FlipperFormat* ff) {
    if(ff == NULL) return;
    free(ff->text);
    free(ff);
}

static bool ff_reserve(FlipperFormat* ff, size_t n) {
    char* p;
    size_t cap = ff->cap;

    if(n + 1 <= cap) return true;
    if(n + 1 > FLIPPER_FORMAT_MAX) return false;
    while(cap < n + 1)
        cap *= 2;
    if(cap > FLIPPER_FORMAT_MAX) cap = FLIPPER_FORMAT_MAX;
    p = realloc(ff->text, cap);
    if(p == NULL) return false;
    ff->text = p;
    ff->cap = cap;
    return true;
}

bool flipper_format_set_text(FlipperFormat* ff, const char* text, size_t len) {
    if(!ff_reserve(ff, len)) return false;
    memcpy(ff->text, text, len);
    ff->text[len] = 0;
    ff->len = len;
    ff->pos = 0;
    ff->changed = false;
    return true;
}

const char* flipper_format_text(FlipperFormat* ff, size_t* len) {
    if(len) *len = ff->len;
    return ff->text;
}

bool flipper_format_changed(FlipperFormat* ff) {
    return ff->changed;
}

bool flipper_format_file_open_existing(FlipperFormat* ff, const char* path) {
    FILE* f = fopen(path, "r");
    char buf[256];
    size_t n;

    if(f == NULL) return false;
    ff->len = ff->pos = 0;
    ff->text[0] = 0;
    while((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if(!ff_reserve(ff, ff->len + n)) {
            fclose(f);
            ff->len = 0;
            ff->text[0] = 0;
            return false; /* too big for a key file */
        }
        memcpy(ff->text + ff->len, buf, n);
        ff->len += n;
        ff->text[ff->len] = 0;
    }
    fclose(f);
    if(memchr(ff->text, 0, ff->len) != NULL) return false; /* not text */
    snprintf(ff->path, sizeof(ff->path), "%s", path);
    ff->changed = false;
    return true;
}

bool flipper_format_file_open_always(FlipperFormat* ff, const char* path) {
    ff->len = ff->pos = 0;
    ff->text[0] = 0;
    snprintf(ff->path, sizeof(ff->path), "%s", path);
    ff->changed = true;
    return true;
}

bool flipper_format_save(FlipperFormat* ff, const char* path) {
    FILE* f = fopen(path, "w");
    bool ok;

    if(f == NULL) return false;
    ok = fwrite(ff->text, 1, ff->len, f) == ff->len;
    if(fclose(f) != 0) ok = false;
    if(ok) ff->changed = false;
    return ok;
}

bool flipper_format_file_close(FlipperFormat* ff) {
    bool ok = true;

    if(ff->changed && ff->path[0]) ok = flipper_format_save(ff, ff->path);
    ff->path[0] = 0;
    return ok;
}

bool flipper_format_rewind(FlipperFormat* ff) {
    ff->pos = 0;
    return true;
}

bool flipper_format_seek_to_end(FlipperFormat* ff) {
    ff->pos = ff->len;
    return true;
}

Stream* flipper_format_get_raw_stream(FlipperFormat* ff) {
    return ff;
}

bool stream_clean(Stream* ff) {
    ff->len = ff->pos = 0;
    ff->text[0] = 0;
    ff->changed = true;
    return true;
}

/* From pos on: the line whose key is `key`.  *line = its start, *val = the
 * first byte of the value (after ": "), *end = after its '\n' (or len). */
static bool ff_find(FlipperFormat* ff, size_t from, const char* key, size_t* line, size_t* val, size_t* end) {
    size_t klen = strlen(key);
    size_t p = from;

    /* a position inside a line: go to the next line start */
    if(p > 0 && p <= ff->len && ff->text[p - 1] != '\n') {
        while(p < ff->len && ff->text[p] != '\n')
            p++;
        if(p < ff->len) p++;
    }
    while(p < ff->len) {
        size_t e = p;
        while(e < ff->len && ff->text[e] != '\n')
            e++;
        if(ff->text[p] != '#' && e - p >= klen + 1 && memcmp(ff->text + p, key, klen) == 0 &&
           ff->text[p + klen] == ':') {
            size_t v = p + klen + 1;
            while(v < e && ff->text[v] == ' ')
                v++;
            *line = p;
            *val = v;
            *end = e < ff->len ? e + 1 : e;
            return true;
        }
        p = e < ff->len ? e + 1 : e;
    }
    return false;
}

/* The value of `key` (from pos on) into buf, without the line end; the
 * position moves after the line, as stock. */
static bool ff_value(FlipperFormat* ff, const char* key, char* buf, size_t size) {
    size_t line, val, end, n;

    if(!ff_find(ff, ff->pos, key, &line, &val, &end)) return false;
    n = end - val;
    while(n > 0 && (ff->text[val + n - 1] == '\n' || ff->text[val + n - 1] == '\r' ||
                    ff->text[val + n - 1] == ' '))
        n--;
    if(n >= size) return false;
    memcpy(buf, ff->text + val, n);
    buf[n] = 0;
    ff->pos = end;
    return true;
}

bool flipper_format_key_exist(FlipperFormat* ff, const char* key) {
    size_t a, b, c;
    return ff_find(ff, 0, key, &a, &b, &c);
}

bool flipper_format_read_string(FlipperFormat* ff, const char* key, FuriString* data) {
    char buf[256];

    if(!ff_value(ff, key, buf, sizeof(buf))) return false;
    furi_string_set_str(data, buf);
    return true;
}

bool flipper_format_read_header(FlipperFormat* ff, FuriString* filetype, uint32_t* version) {
    return flipper_format_read_string(ff, "Filetype", filetype) &&
           flipper_format_read_uint32(ff, "Version", version, 1);
}

/* n values separated by spaces; each through conv() */
static bool ff_values(
    FlipperFormat* ff,
    const char* key,
    uint16_t n,
    bool (*conv)(const char* tok, void* out, uint16_t i),
    void* out) {
    char buf[512];
    char* p = buf;
    uint16_t i;

    if(!ff_value(ff, key, buf, sizeof(buf))) return false;
    for(i = 0; i < n; i++) {
        char* t;
        while(*p == ' ')
            p++;
        if(*p == 0) return false;
        t = p;
        while(*p && *p != ' ')
            p++;
        if(*p) *p++ = 0;
        if(!conv(t, out, i)) return false;
    }
    return true;
}

static bool conv_u32(const char* t, void* out, uint16_t i) {
    unsigned long long v = 0;
    if(*t == 0) return false;
    for(; *t; t++) {
        if(*t < '0' || *t > '9') return false;
        v = v * 10 + (unsigned)(*t - '0');
        if(v > 0xFFFFFFFFull) return false;
    }
    ((uint32_t*)out)[i] = (uint32_t)v;
    return true;
}

static bool conv_i32(const char* t, void* out, uint16_t i) {
    bool neg = *t == '-';
    uint32_t v;
    if(neg) t++;
    if(!conv_u32(t, &v, 0) || v > 0x80000000u || (!neg && v == 0x80000000u)) return false;
    ((int32_t*)out)[i] = neg ? (int32_t)(0u - v) : (int32_t)v;
    return true;
}

static bool conv_hex(const char* t, void* out, uint16_t i) {
    uint8_t v;
    if(strlen(t) != 2 || !hex_char_to_uint8(t[0], t[1], &v)) return false;
    ((uint8_t*)out)[i] = v;
    return true;
}

static bool conv_bool(const char* t, void* out, uint16_t i) {
    if(strcmp(t, "true") == 0)
        ((bool*)out)[i] = true;
    else if(strcmp(t, "false") == 0)
        ((bool*)out)[i] = false;
    else
        return false;
    return true;
}

bool flipper_format_read_uint32(FlipperFormat* ff, const char* key, uint32_t* data, uint16_t n) {
    return ff_values(ff, key, n, conv_u32, data);
}

bool flipper_format_read_int32(FlipperFormat* ff, const char* key, int32_t* data, uint16_t n) {
    return ff_values(ff, key, n, conv_i32, data);
}

bool flipper_format_read_hex(FlipperFormat* ff, const char* key, uint8_t* data, uint16_t n) {
    return ff_values(ff, key, n, conv_hex, data);
}

bool flipper_format_read_bool(FlipperFormat* ff, const char* key, bool* data, uint16_t n) {
    return ff_values(ff, key, n, conv_bool, data);
}

bool flipper_format_get_value_count(FlipperFormat* ff, const char* key, uint32_t* count) {
    size_t line, val, end, p;
    uint32_t c = 0;
    bool in = false;

    if(!ff_find(ff, 0, key, &line, &val, &end)) return false;
    for(p = val; p < end && ff->text[p] != '\n' && ff->text[p] != '\r'; p++) {
        if(ff->text[p] != ' ' && !in) c++;
        in = ff->text[p] != ' ';
    }
    *count = c;
    return true;
}

/* Insert text at pos (pos moves after it). */
static bool ff_insert(FlipperFormat* ff, const char* s, size_t n) {
    if(!ff_reserve(ff, ff->len + n)) return false;
    memmove(ff->text + ff->pos + n, ff->text + ff->pos, ff->len - ff->pos + 1);
    memcpy(ff->text + ff->pos, s, n);
    ff->len += n;
    ff->pos += n;
    ff->changed = true;
    return true;
}

typedef void (*ff_fmt)(FuriString* line, const void* data, uint16_t i);

static void fmt_u32(FuriString* l, const void* d, uint16_t i) {
    furi_string_cat_printf(l, "%lu", (unsigned long)((const uint32_t*)d)[i]);
}
static void fmt_i32(FuriString* l, const void* d, uint16_t i) {
    furi_string_cat_printf(l, "%ld", (long)((const int32_t*)d)[i]);
}
static void fmt_hex(FuriString* l, const void* d, uint16_t i) {
    furi_string_cat_printf(l, "%02X", ((const uint8_t*)d)[i]);
}
static void fmt_bool(FuriString* l, const void* d, uint16_t i) {
    furi_string_cat_str(l, ((const bool*)d)[i] ? "true" : "false");
}
static void fmt_str(FuriString* l, const void* d, uint16_t i) {
    UNUSED(i);
    furi_string_cat_str(l, (const char*)d);
}

static FuriString* ff_line(const char* key, ff_fmt fmt, const void* data, uint16_t n) {
    FuriString* l = furi_string_alloc();
    uint16_t i;

    furi_string_cat_printf(l, "%s: ", key);
    for(i = 0; i < n; i++) {
        if(i) furi_string_push_back(l, ' ');
        fmt(l, data, i);
    }
    furi_string_push_back(l, '\n');
    return l;
}

static bool ff_write(FlipperFormat* ff, const char* key, ff_fmt fmt, const void* data, uint16_t n) {
    FuriString* l = ff_line(key, fmt, data, n);
    bool ok;

    /* a line starts after a newline */
    if(ff->pos > 0 && ff->text[ff->pos - 1] != '\n' && !ff_insert(ff, "\n", 1)) {
        furi_string_free(l);
        return false;
    }
    ok = ff_insert(ff, furi_string_get_cstr(l), furi_string_size(l));
    furi_string_free(l);
    return ok;
}

/* stock: rewind, find the key, replace its line; the position after it */
static bool ff_update(FlipperFormat* ff, const char* key, ff_fmt fmt, const void* data, uint16_t n) {
    size_t line, val, end;
    FuriString* l;
    bool ok;

    if(!ff_find(ff, 0, key, &line, &val, &end)) return false;
    l = ff_line(key, fmt, data, n);
    if(end == ff->len && (end == 0 || ff->text[end - 1] != '\n')) {
        /* the last line had no newline: neither has the new one */
        furi_string_trim(l, "\n");
    }
    memmove(ff->text + line, ff->text + end, ff->len - end + 1);
    ff->len -= end - line;
    ff->pos = line;
    ok = ff_insert(ff, furi_string_get_cstr(l), furi_string_size(l));
    furi_string_free(l);
    return ok;
}

bool flipper_format_write_header_cstr(FlipperFormat* ff, const char* filetype, uint32_t version) {
    return flipper_format_write_string_cstr(ff, "Filetype", filetype) &&
           flipper_format_write_uint32(ff, "Version", &version, 1);
}

bool flipper_format_write_string_cstr(FlipperFormat* ff, const char* key, const char* data) {
    return ff_write(ff, key, fmt_str, data, 1);
}

bool flipper_format_write_string(FlipperFormat* ff, const char* key, FuriString* data) {
    return ff_write(ff, key, fmt_str, furi_string_get_cstr(data), 1);
}

bool flipper_format_write_uint32(FlipperFormat* ff, const char* key, const uint32_t* data, uint16_t n) {
    return ff_write(ff, key, fmt_u32, data, n);
}

bool flipper_format_write_int32(FlipperFormat* ff, const char* key, const int32_t* data, uint16_t n) {
    return ff_write(ff, key, fmt_i32, data, n);
}

bool flipper_format_write_hex(FlipperFormat* ff, const char* key, const uint8_t* data, uint16_t n) {
    return ff_write(ff, key, fmt_hex, data, n);
}

bool flipper_format_write_bool(FlipperFormat* ff, const char* key, const bool* data, uint16_t n) {
    return ff_write(ff, key, fmt_bool, data, n);
}

bool flipper_format_write_comment_cstr(FlipperFormat* ff, const char* data) {
    FuriString* l = furi_string_alloc();
    bool ok;

    furi_string_cat_printf(l, "# %s\n", data);
    ok = ff_insert(ff, furi_string_get_cstr(l), furi_string_size(l));
    furi_string_free(l);
    return ok;
}

bool flipper_format_update_string_cstr(FlipperFormat* ff, const char* key, const char* data) {
    return ff_update(ff, key, fmt_str, data, 1);
}

bool flipper_format_update_uint32(FlipperFormat* ff, const char* key, const uint32_t* data, uint16_t n) {
    return ff_update(ff, key, fmt_u32, data, n);
}

bool flipper_format_update_hex(FlipperFormat* ff, const char* key, const uint8_t* data, uint16_t n) {
    return ff_update(ff, key, fmt_hex, data, n);
}

bool flipper_format_insert_or_update_uint32(
    FlipperFormat* ff,
    const char* key,
    const uint32_t* data,
    uint16_t n) {
    if(flipper_format_key_exist(ff, key)) return ff_update(ff, key, fmt_u32, data, n);
    ff->pos = ff->len;
    return ff_write(ff, key, fmt_u32, data, n);
}

bool flipper_format_insert_or_update_hex(
    FlipperFormat* ff,
    const char* key,
    const uint8_t* data,
    uint16_t n) {
    if(flipper_format_key_exist(ff, key)) return ff_update(ff, key, fmt_hex, data, n);
    ff->pos = ff->len;
    return ff_write(ff, key, fmt_hex, data, n);
}
