/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri port layer for the host tests (spec R5.1): ucontext threads,
 * CLOCK_MONOTONIC, nanosleep. Not built for MINIX. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "furi_i.h"

uint32_t fport_now(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U);
}

unsigned long fport_host_idles; /* the tests count the sleeps */

void fport_idle(uint32_t ms) {
    struct timespec ts;

    fport_host_idles++;
    ts.tv_sec = ms / 1000U;
    ts.tv_nsec = (long)(ms % 1000U) * 1000000L;
    nanosleep(&ts, NULL);
}

int fport_vformat(char* buf, size_t size, const char* fmt, va_list ap) {
    int r;

    if(size == 0) return -1;
    r = vsnprintf(buf, size, fmt, ap);
    return (r < 0 || (size_t)r >= size) ? -1 : r;
}

/* A slack under each stack: an overflow lands there, and the canary
 * check reports it, instead of damaging the host's heap first. */
#define SLACK 8192

void* fport_stack_alloc(size_t size) {
    void* p = NULL;

    if(posix_memalign(&p, 16, size + SLACK) != 0) return NULL;
    return (char*)p + SLACK;
}

void fport_stack_free(void* lo, size_t size) {
    (void)size;
    free((char*)lo - SLACK);
}

bool fport_main_stack_ok(void) {
    return true;
}

void fport_ctx_init(FPortCtx* ctx, void* lo, size_t size, void (*entry)(void)) {
    getcontext(&ctx->uc);
    ctx->uc.uc_stack.ss_sp = lo;
    ctx->uc.uc_stack.ss_size = size;
    ctx->uc.uc_link = NULL;
    makecontext(&ctx->uc, entry, 0);
}

void fport_ctx_swap(FPortCtx* from, FPortCtx* to) {
    swapcontext(&from->uc, &to->uc);
}
