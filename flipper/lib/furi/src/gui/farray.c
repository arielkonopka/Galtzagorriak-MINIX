/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri GUI (spec R5.2): the untyped core of m-array.h. Grows by 4
 * elements at a time: GUI arrays are short (menu items, views). */
#include <furi.h>
#include "m-array.h"

void farray_init(FArray* a) {
    a->ptr = NULL;
    a->size = a->alloc = 0;
}

void farray_clear(FArray* a) {
    free(a->ptr);
    farray_init(a);
}

void* farray_insert(FArray* a, size_t i, size_t es) {
    char* p;

    furi_check(i <= a->size && a->size < UINT16_MAX);
    if(a->size == a->alloc) {
        p = realloc(a->ptr, (a->alloc + 4U) * es);
        furi_check(p, "out of memory");
        a->ptr = p;
        a->alloc += 4;
    }
    p = (char*)a->ptr + i * es;
    memmove(p + es, p, (a->size - i) * es);
    memset(p, 0, es);
    a->size++;
    return p;
}

void farray_remove(FArray* a, size_t i, size_t es) {
    char* p = (char*)a->ptr + i * es;

    furi_check(i < a->size);
    a->size--;
    memmove(p, p + es, (a->size - i) * es);
}
