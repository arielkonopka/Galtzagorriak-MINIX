/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri GUI (spec R5.2): the part of M*LIB's ARRAY_DEF that the stock
 * gui code uses, on one shared untyped core (farray.c) so each array
 * type costs a few inline wrappers, not a copy of M*LIB's generated
 * code. Elements are plain data: the oplist argument is ignored, a new
 * element is zeroed, clear/reset/remove do not run element destructors
 * (the ported modules free what their elements own themselves).
 * Iterators are indices; previous() from 0 wraps to "end". */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    void* ptr;
    uint16_t size;
    uint16_t alloc;
} FArray;

void farray_init(FArray* a);
void farray_clear(FArray* a);
/* A zeroed slot at index i (i <= size), the ones after it moved up. */
void* farray_insert(FArray* a, size_t i, size_t es);
void farray_remove(FArray* a, size_t i, size_t es);

#define M_POD_OPLIST     ()
#define M_PTR_OPLIST     ()
#define M_DEFAULT_OPLIST ()

#define ARRAY_DEF(name, ...) ARRAY_DEF_(name, __VA_ARGS__, )
#define ARRAY_DEF_(name, type, ...)                                                    \
    typedef FArray name##_t[1];                                                        \
    typedef struct {                                                                   \
        FArray* a;                                                                     \
        size_t i;                                                                      \
    } name##_it_t[1];                                                                  \
    static inline void name##_init(name##_t a) {                                       \
        farray_init(a);                                                                \
    }                                                                                  \
    static inline void name##_clear(name##_t a) {                                      \
        farray_clear(a);                                                               \
    }                                                                                  \
    static inline void name##_reset(name##_t a) {                                      \
        a->size = 0;                                                                   \
    }                                                                                  \
    static inline size_t name##_size(const name##_t a) {                               \
        return a->size;                                                                \
    }                                                                                  \
    static inline type* name##_get(const name##_t a, size_t i) {                       \
        return (type*)a->ptr + i;                                                      \
    }                                                                                  \
    static inline const type* name##_cget(const name##_t a, size_t i) {                \
        return (const type*)a->ptr + i;                                                \
    }                                                                                  \
    static inline type* name##_push_new(name##_t a) {                                  \
        return (type*)farray_insert(a, a->size, sizeof(type));                         \
    }                                                                                  \
    static inline void name##_push_back(name##_t a, type x) {                          \
        *(type*)farray_insert(a, a->size, sizeof(type)) = x;                           \
    }                                                                                  \
    static inline void name##_push_at(name##_t a, size_t i, type x) {                  \
        *(type*)farray_insert(a, i, sizeof(type)) = x;                                 \
    }                                                                                  \
    static inline type* name##_back(const name##_t a) {                                \
        return (type*)a->ptr + a->size - 1;                                            \
    }                                                                                  \
    static inline void name##_pop_back(type* x, name##_t a) {                          \
        a->size--;                                                                     \
        if(x) *x = ((type*)a->ptr)[a->size];                                           \
    }                                                                                  \
    static inline void name##_it(name##_it_t it, name##_t a) {                         \
        it->a = a;                                                                     \
        it->i = 0;                                                                     \
    }                                                                                  \
    static inline void name##_it_last(name##_it_t it, name##_t a) {                    \
        it->a = a;                                                                     \
        it->i = (size_t)a->size - 1;                                                   \
    }                                                                                  \
    static inline bool name##_end_p(const name##_it_t it) {                            \
        return it->i >= it->a->size;                                                   \
    }                                                                                  \
    static inline void name##_next(name##_it_t it) {                                   \
        it->i++;                                                                       \
    }                                                                                  \
    static inline void name##_previous(name##_it_t it) {                               \
        it->i--;                                                                       \
    }                                                                                  \
    static inline type* name##_ref(const name##_it_t it) {                             \
        return (type*)it->a->ptr + it->i;                                              \
    }                                                                                  \
    static inline const type* name##_cref(const name##_it_t it) {                      \
        return (const type*)it->a->ptr + it->i;                                        \
    }                                                                                  \
    /* drop the element at it and those after it */                                    \
    static inline void name##_pop_until(name##_t a, const name##_it_t it) {            \
        a->size = (uint16_t)it->i;                                                     \
    }                                                                                  \
    static inline void name##_remove(name##_t a, name##_it_t it) {                     \
        farray_remove(a, it->i, sizeof(type));                                         \
    }                                                                                  \
    typedef int name##_unused_
