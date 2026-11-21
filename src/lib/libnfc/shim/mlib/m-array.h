/* SPDX-License-Identifier: GPL-3.0-or-later
 * libnfc shim (#412): the part of M*LIB's m-array.h the vendored FeliCa
 * poller uses, for plain-old-data elements only: ARRAY_DEF(name, type,
 * oplist) gives name_t with init, clear, push_raw, size, back.  Own code,
 * not M*LIB (as src/lib/subghz/shim/m-array.h). */
#pragma once

#include <furi.h>

#define M_POD_OPLIST ()

#define ARRAY_DEF(name, type, oplist)                                      \
    typedef struct name##_s {                                              \
        size_t size, alloc;                                                \
        type* ptr;                                                         \
    } name##_t[1];                                                         \
    static inline void name##_init(name##_t a) {                           \
        a->size = a->alloc = 0;                                            \
        a->ptr = NULL;                                                     \
    }                                                                      \
    static inline void name##_clear(name##_t a) {                          \
        free(a->ptr);                                                      \
        name##_init(a);                                                    \
    }                                                                      \
    static inline type* name##_push_raw(name##_t a) {                      \
        if(a->size == a->alloc) {                                          \
            size_t n = a->alloc ? a->alloc * 2 : 8;                        \
            type* p = (type*)realloc(a->ptr, n * sizeof(type));            \
            if(p == NULL) furi_crash("out of memory");                     \
            a->ptr = p;                                                    \
            a->alloc = n;                                                  \
        }                                                                  \
        memset(&a->ptr[a->size], 0, sizeof(type));                         \
        return &a->ptr[a->size++];                                         \
    }                                                                      \
    static inline size_t name##_size(const name##_t a) {                   \
        return a->size;                                                    \
    }                                                                      \
    static inline type* name##_back(const name##_t a) {                    \
        furi_check(a->size);                                               \
        return &a->ptr[a->size - 1];                                       \
    }
