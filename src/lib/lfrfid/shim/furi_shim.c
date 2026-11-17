/* SPDX-License-Identifier: GPL-3.0-or-later
 * liblfrfid (spec R9.0.1): the furi shim's code.  See furi.h.
 *
 * FuriString's printf is done here, not by the C library: MINIX 2.0's
 * doprnt has no %ll and no %hh, which the Unleashed renderers use (%llu
 * national codes, %hhu facility codes).  Doing it here also makes the text
 * the same on the PC (make lfrfidtest) and on the Flipper.  Supported:
 * flags -+ #0, width, precision (numbers or *), the lengths hh h l ll z j,
 * the conversions d i u x X o c s f %.  %l is 32 bits as on the Flipper
 * (Unleashed passes uint32_t to %lu; on a 64-bit PC the upper half of that
 * argument slot is undefined, so it is cut off). */
#include <furi.h>
#include <furi_hal_rtc.h>

#undef malloc
#undef furi_string_cat
#undef furi_string_set

size_t lfrfid_heap_bytes;

void* lfrfid_malloc(size_t size) {
    void* p = calloc(1, size ? size : 1);

    if(p == NULL) lfrfid_crash("out of memory", __FILE__, __LINE__);
    lfrfid_heap_bytes += size;
    return p;
}

void lfrfid_crash(const char* message, const char* file, int line) {
    fprintf(stderr, "lfrfid: %s (%s:%d)\n", message ? message : "crash", file, line);
    abort();
}

FuriHalRtcLocaleUnits furi_hal_rtc_get_locale_units(void) {
    return FuriHalRtcLocaleUnitsMetric;
}

void* furi_record_open(const char* name) {
    (void)name;
    return NULL;
}

void furi_record_close(const char* name) {
    (void)name;
}

/* ---- FuriString: a fixed buffer ---- */

FuriString* furi_string_alloc(void) {
    FuriString* s = lfrfid_malloc(sizeof(FuriString));
    return s;
}

void furi_string_free(FuriString* s) {
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

static void put(FuriString* s, char c) {
    if(s->len + 1 < LFRFID_STR_MAX) {
        s->buf[s->len++] = c;
        s->buf[s->len] = 0;
    }
}

void furi_string_cat_str(FuriString* s, const char* source) {
    while(*source)
        put(s, *source++);
}

void furi_string_set_str(FuriString* s, const char* source) {
    furi_string_reset(s);
    furi_string_cat_str(s, source);
}

void furi_string_set(FuriString* s, FuriString* source) {
    if(s != source) furi_string_set_str(s, source->buf);
}

void furi_string_cat(FuriString* s, const FuriString* source) {
    furi_string_cat_str(s, source->buf);
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
    FuriString t;
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

#ifdef LFRFID_LIBC_PRINTF
/* make lfrfid-golden: the C library's printf, as newlib's on the stock
 * firmware, to make the reference text the test compares with. */
int furi_string_cat_vprintf(FuriString* s, const char* f, va_list ap) {
    int n = vsnprintf(s->buf + s->len, LFRFID_STR_MAX - s->len, f, ap);

    s->len += n < 0 ? 0 : (size_t)n;
    if(s->len >= LFRFID_STR_MAX) s->len = LFRFID_STR_MAX - 1;
    return n;
}
#else
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
#endif

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
/* MINIX 2.0's libm is C89: no roundf (PAC/Stanley rounds a duration to
 * bits with it).  Halves away from zero, as C99's. */
float roundf(float x) {
    return x < 0 ? -(float)(long)(-x + 0.5f) : (float)(long)(x + 0.5f);
}
#endif
