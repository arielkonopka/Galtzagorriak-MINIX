/* SPDX-License-Identifier: GPL-3.0-or-later
 * libir shim (see irshim.h): the stock firmware's malloc and crash. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ir_crash(const char* message, const char* file, int line) __attribute__((noreturn));
void* ir_zalloc(size_t n);

/* Bytes the codecs hold now (the decoder and encoder handlers): the
 * apps print it with their RAM report. */
size_t ir_heap_bytes;

void ir_crash(const char* message, const char* file, int line) {
    fprintf(stderr, "libir: %s (%s:%d)\n", message && *message ? message : "crash", file, line);
    exit(70);
}

void* ir_zalloc(size_t n) {
    void* p = calloc(1, n ? n : 1);

    if(p == NULL) ir_crash("out of memory", __FILE__, __LINE__);
    ir_heap_bytes += n;
    return p;
}
