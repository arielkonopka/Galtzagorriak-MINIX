/* SPDX-License-Identifier: GPL-3.0-or-later
 * libsubghz: the part of M*LIB's m-array.h (BSD-2) the Unleashed Sub-GHz
 * files use, for plain-old-data elements only: ARRAY_DEF(name, type, oplist)
 * makes name_t (an array of one struct, as M*LIB's) with init, clear,
 * push_raw, push_new, size, get; M_EACH walks it.  Own code, not M*LIB. */
#pragma once

#include <furi.h>

#define M_POD_OPLIST ()
#define ARRAY_OPLIST(name, oplist) ()

#define ARRAY_DEF(name, type, oplist)                                              \
    typedef struct name##_s {                                                      \
        size_t size, alloc;                                                        \
        type* ptr;                                                                 \
    } name##_t[1];                                                                 \
    typedef type name##_subtype_ct;                                                \
    static inline void name##_init(name##_t a) {                                   \
        a->size = a->alloc = 0;                                                    \
        a->ptr = NULL;                                                             \
    }                                                                              \
    static inline void name##_clear(name##_t a) {                                  \
        free(a->ptr);                                                              \
        name##_init(a);                                                            \
    }                                                                              \
    static inline void name##_reset(name##_t a) {                                  \
        a->size = 0;                                                               \
    }                                                                              \
    static inline type* name##_push_raw(name##_t a) {                              \
        if(a->size == a->alloc) {                                                  \
            size_t n = a->alloc ? a->alloc * 2 : 8;                                \
            type* p = (type*)realloc(a->ptr, n * sizeof(type));                    \
            if(p == NULL) subghz_crash("out of memory", __FILE__, __LINE__);       \
            a->ptr = p;                                                            \
            a->alloc = n;                                                          \
        }                                                                          \
        memset(&a->ptr[a->size], 0, sizeof(type));                                 \
        return &a->ptr[a->size++];                                                 \
    }                                                                              \
    static inline type* name##_push_new(name##_t a) {                              \
        return name##_push_raw(a);                                                 \
    }                                                                              \
    static inline size_t name##_size(const name##_t a) {                           \
        return a->size;                                                            \
    }                                                                              \
    static inline type* name##_get(const name##_t a, size_t i) {                   \
        return &a->ptr[i];                                                         \
    }

/* "for M_EACH(item, array, type) { ... }": item points at each element. */
#define M_EACH(item, a, type) \
    (__typeof__(&(a)->ptr[0]) item = (a)->ptr; item < (a)->ptr + (a)->size; item++)
