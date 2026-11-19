/* SPDX-License-Identifier: GPL-3.0-or-later
 * libsubghz: the furi shim's code.  See furi.h.
 *
 * FuriString's printf is done here, not by the C library: MINIX 2.0's
 * doprnt has no %ll and no %z, which the Unleashed renderers use (%08llX
 * keys, %zu).  Doing it here also makes the text the same on the PC (the
 * host test) and on the Flipper.  The formatter is liblfrfid's
 * (src/lib/lfrfid/shim/furi_shim.c, T19): flags -+ #0, width, precision
 * (numbers or *), the lengths hh h l ll z j, the conversions d i u x X o c
 * s f %.  %l is 32 bits as on the Flipper (Unleashed passes uint32_t to
 * %lX; on a 64-bit PC the upper half of that argument slot is undefined,
 * so it is cut off). */
#include <furi.h>
#include <furi_hal.h>
#include "../subghz/protocols/raw.h"

#undef malloc
#undef furi_string_cat
#undef furi_string_set
#undef furi_string_alloc_set
#undef furi_string_trim

size_t subghz_heap_bytes;
size_t subghz_heap_room = 64 * 1024;

size_t memmgr_heap_get_max_free_block(void) {
    return subghz_heap_room;
}

void* subghz_malloc(size_t size) {
    void* p = calloc(1, size ? size : 1);

    if(p == NULL) subghz_crash("out of memory", __FILE__, __LINE__);
    subghz_heap_bytes += size;
    return p;
}

void subghz_crash(const char* message, const char* file, int line) {
    fprintf(stderr, "subghz: %s (%s:%d)\n", message && *message ? message : "crash", file, line);
    abort();
}

void* furi_record_open(const char* name) {
    (void)name;
    return NULL;
}

void furi_record_close(const char* name) {
    (void)name;
}

void furi_delay_ms(uint32_t ms) {
    (void)ms;
}

uint32_t furi_get_tick(void) {
    return 0;
}

/* ---- furi_hal ---- */

static int32_t rolling_counter_mult = 1;

int32_t furi_hal_subghz_get_rolling_counter_mult(void) {
    return rolling_counter_mult;
}

void furi_hal_subghz_set_rolling_counter_mult(int32_t mult) {
    rolling_counter_mult = mult;
}

bool furi_hal_crypto_enclave_load_key(uint8_t slot, const uint8_t* iv) {
    (void)slot;
    (void)iv;
    return false;
}

bool furi_hal_crypto_enclave_unload_key(uint8_t slot) {
    (void)slot;
    return false;
}

bool furi_hal_crypto_encrypt(const uint8_t* input, uint8_t* output, size_t size) {
    (void)input;
    (void)output;
    (void)size;
    return false;
}

bool furi_hal_crypto_decrypt(const uint8_t* input, uint8_t* output, size_t size) {
    (void)input;
    (void)output;
    (void)size;
    return false;
}

/* ---- RAW: no decoder or encoder in the library (stock's raw.c writes the
 * SD card from a worker thread); `subghz` reads and plays RAW files itself,
 * a line at a time.  The registry entry stays, so the name is known. ---- */

const SubGhzProtocol subghz_protocol_raw = {
    .name = SUBGHZ_PROTOCOL_RAW_NAME,
    .type = SubGhzProtocolTypeRAW,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_868 | SubGhzProtocolFlag_315 |
            SubGhzProtocolFlag_AM | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_RAW |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = NULL,
    .encoder = NULL,
};

/* ---- FuriString: a heap string, at most SUBGHZ_STR_MAX bytes ---- */

FuriString* furi_string_alloc(void) {
    FuriString* s = subghz_malloc(sizeof(FuriString));

    s->cap = 16;
    s->buf = subghz_malloc(s->cap);
    return s;
}

FuriString* furi_string_alloc_set_str(const char* str) {
    FuriString* s = furi_string_alloc();

    furi_string_cat_str(s, str);
    return s;
}

void furi_string_free(FuriString* s) {
    if(s == NULL) return;
    free(s->buf);
    free(s);
}

void furi_string_reset(FuriString* s) {
    s->len = 0;
    s->buf[0] = 0;
}

const char* furi_string_get_cstr(const FuriString* s) {
    return s->buf;
}

size_t furi_string_size(const FuriString* s) {
    return s->len;
}

bool furi_string_empty(const FuriString* s) {
    return s->len == 0;
}

static void put(FuriString* s, char c) {
    if(s->len + 1 >= s->cap) {
        size_t n = s->cap * 2;
        char* p;

        if(n > SUBGHZ_STR_MAX) n = SUBGHZ_STR_MAX;
        if(s->len + 1 >= n) return; /* full: cut */
        p = realloc(s->buf, n);
        if(p == NULL) subghz_crash("out of memory", __FILE__, __LINE__);
        s->buf = p;
        s->cap = n;
    }
    s->buf[s->len++] = c;
    s->buf[s->len] = 0;
}

void furi_string_push_back(FuriString* s, char c) {
    put(s, c);
}

void furi_string_cat_str(FuriString* s, const char* source) {
    while(*source)
        put(s, *source++);
}

void furi_string_set_str(FuriString* s, const char* source) {
    furi_string_reset(s);
    furi_string_cat_str(s, source);
}

void furi_string_set_s(FuriString* s, const FuriString* source) {
    if(s != source) furi_string_set_str(s, source->buf);
}

void furi_string_set_n(FuriString* s, const FuriString* source, size_t start, size_t len) {
    size_t i;

    if(s == source) return;
    furi_string_reset(s);
    for(i = start; i < source->len && i < start + len; i++)
        put(s, source->buf[i]);
}

void furi_string_cat_s(FuriString* s, const FuriString* source) {
    furi_string_cat_str(s, source->buf);
}

void furi_string_trim(FuriString* s, const char* chars) {
    size_t a = 0, b = s->len;

    while(a < b && strchr(chars, s->buf[a]))
        a++;
    while(b > a && strchr(chars, s->buf[b - 1]))
        b--;
    memmove(s->buf, s->buf + a, b - a);
    s->len = b - a;
    s->buf[s->len] = 0;
}

int furi_string_cmp_str(const FuriString* s, const char* str) {
    return strcmp(s->buf, str);
}

/* One number: digits of v in base, then sign, prefix, padding. */
static void put_num(
    FuriString* s,
    unsigned long long v,
    int neg,
    int base,
    int upper,
    int width,
    int prec,
    int flags) {
    char tmp[24], *p = tmp + sizeof(tmp);
    const char* dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char sign = neg ? '-' : (flags & 2) ? '+' : (flags & 4) ? ' ' : 0;
    const char* pre = "";
    int n, pad, zero = (flags & 8) && !(flags & 1) && prec < 0;

    if(prec < 0) prec = 1;
    while(v) {
        *--p = dig[v % base];
        v /= base;
    }
    n = (int)(tmp + sizeof(tmp) - p);
    if((flags & 16) && base == 16 && n) pre = upper ? "0X" : "0x";
    if((flags & 16) && base == 8 && prec <= n) prec = n + 1;
    if(prec < n) prec = n;
    pad = width - prec - (sign != 0) - (int)strlen(pre);
    if(!(flags & 1) && !zero)
        while(pad-- > 0)
            put(s, ' ');
    if(sign) put(s, sign);
    furi_string_cat_str(s, pre);
    if(zero)
        while(pad-- > 0)
            put(s, '0');
    while(prec-- > n)
        put(s, '0');
    while(p < tmp + sizeof(tmp))
        put(s, *p++);
    if(flags & 1)
        while(pad-- > 0)
            put(s, ' ');
}

static void put_float(FuriString* s, double d, int width, int prec, int flags) {
    char tbuf[48];
    FuriString t = {tbuf, 0, sizeof(tbuf)};
    unsigned long long ip, fp, scale = 1;
    int i, neg = d < 0;

    if(prec < 0) prec = 6;
    if(prec > 9) prec = 9;
    if(neg) d = -d;
    for(i = 0; i < prec; i++)
        scale *= 10;
    d = d * (double)scale + 0.5;
    if(d > 1e18) d = 1e18;
    ip = (unsigned long long)d / scale;
    fp = (unsigned long long)d % scale;
    furi_string_reset(&t);
    put_num(&t, ip, neg, 10, 0, 0, -1, flags & (2 | 4));
    if(prec) {
        put(&t, '.');
        put_num(&t, fp, 0, 10, 0, 0, prec, 0);
    }
    for(i = (int)t.len; !(flags & 1) && i < width; i++)
        put(s, (flags & 8) ? '0' : ' ');
    furi_string_cat_str(s, t.buf);
    for(i = (int)t.len; (flags & 1) && i < width; i++)
        put(s, ' ');
}

int furi_string_cat_vprintf(FuriString* s, const char* f, va_list ap) {
    size_t start = s->len;

    for(; *f; f++) {
        int flags = 0, width = 0, prec = -1, len = 0, base = 10, upper = 0;
        unsigned long long v;
        int neg = 0;

        if(*f != '%') {
            put(s, *f);
            continue;
        }
        for(f++;; f++) {
            if(*f == '-')
                flags |= 1;
            else if(*f == '+')
                flags |= 2;
            else if(*f == ' ')
                flags |= 4;
            else if(*f == '0')
                flags |= 8;
            else if(*f == '#')
                flags |= 16;
            else
                break;
        }
        if(*f == '*') {
            width = va_arg(ap, int);
            if(width < 0) {
                flags |= 1;
                width = -width;
            }
            f++;
        } else
            while(*f >= '0' && *f <= '9')
                width = width * 10 + (*f++ - '0');
        if(*f == '.') {
            prec = 0;
            f++;
            if(*f == '*') {
                prec = va_arg(ap, int);
                f++;
            } else
                while(*f >= '0' && *f <= '9')
                    prec = prec * 10 + (*f++ - '0');
        }
        /* len: -2 hh, -1 h, 0 int, 1 l (32 bits), 2 ll, 3 size_t */
        if(*f == 'h') {
            len = -1;
            if(*++f == 'h') {
                len = -2;
                f++;
            }
        } else if(*f == 'l') {
            len = 1;
            if(*++f == 'l') {
                len = 2;
                f++;
            }
        } else if(*f == 'z' || *f == 'j') {
            len = *f == 'z' ? 3 : 2;
            f++;
        }
        switch(*f) {
        case 0:
            f--;
            continue;
        case '%':
            put(s, '%');
            continue;
        case 'c':
            put(s, (char)va_arg(ap, int));
            continue;
        case 's': {
            const char* str = va_arg(ap, const char*);
            int n = 0, i;

            if(str == NULL) str = "(null)";
            while(str[n] && (prec < 0 || n < prec))
                n++;
            for(i = n; !(flags & 1) && i < width; i++)
                put(s, ' ');
            for(i = 0; i < n; i++)
                put(s, str[i]);
            for(i = n; (flags & 1) && i < width; i++)
                put(s, ' ');
            continue;
        }
        case 'f':
        case 'F':
            put_float(s, va_arg(ap, double), width, prec, flags);
            continue;
        case 'd':
        case 'i': {
            long long sv;

            if(len == 2)
                sv = va_arg(ap, long long);
            else if(len == 3)
                sv = (long long)va_arg(ap, ptrdiff_t);
            else if(len == 1)
                sv = (int32_t)va_arg(ap, long);
            else
                sv = va_arg(ap, int);
            if(len == -1) sv = (short)sv;
            if(len == -2) sv = (signed char)sv;
            neg = sv < 0;
            v = neg ? (unsigned long long)(-(sv + 1)) + 1 : (unsigned long long)sv;
            put_num(s, v, neg, 10, 0, width, prec, flags);
            continue;
        }
        case 'X':
            upper = 1;
            /* fall through */
        case 'x':
            base = 16;
            break;
        case 'o':
            base = 8;
            break;
        case 'u':
            break;
        default:
            put(s, '%');
            put(s, *f);
            continue;
        }
        if(len == 2)
            v = va_arg(ap, unsigned long long);
        else if(len == 3)
            v = va_arg(ap, size_t);
        else if(len == 1)
            v = (uint32_t)va_arg(ap, unsigned long);
        else
            v = va_arg(ap, unsigned int);
        if(len == -1) v = (unsigned short)v;
        if(len == -2) v = (unsigned char)v;
        put_num(s, v, 0, base, upper, width, prec, flags);
    }
    return (int)(s->len - start);
}

int furi_string_cat_printf(FuriString* s, const char* format, ...) {
    va_list ap;
    int n;

    va_start(ap, format);
    n = furi_string_cat_vprintf(s, format, ap);
    va_end(ap);
    return n;
}

int furi_string_printf(FuriString* s, const char* format, ...) {
    va_list ap;
    int n;

    furi_string_reset(s);
    va_start(ap, format);
    n = furi_string_cat_vprintf(s, format, ap);
    va_end(ap);
    return n;
}


#ifdef __minix
float roundf(float x) {
    return x < 0 ? -(float)(long)(-x + 0.5f) : (float)(long)(x + 0.5f);
}
#endif
