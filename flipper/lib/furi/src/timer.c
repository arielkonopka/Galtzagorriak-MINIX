/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri (spec R5.1, D.1): software timers (stock furi/core/timer.h).
 * The stock firmware runs callbacks in the FreeRTOS timer task; here the
 * scheduler runs them whenever a thread blocks or yields. Until poll()
 * (R4b.6) an idle process sleeps with alarm() (port_minix.c). */
#include <stdlib.h>
#include "furi_i.h"

struct FuriTimer {
    FuriTimer* next; /* armed list */
    FuriTimerCallback cb;
    void* context;
    uint32_t period;
    uint32_t expire;
    uint8_t type;
    uint8_t running;
};

typedef struct Pending {
    struct Pending* next;
    FuriTimerPendigCallback cb;
    void* context;
    uint32_t arg;
} Pending;

static FuriTimer* armed;
static Pending *pending_head, *pending_tail;
uint8_t ftimer_in_callback;

static void unlink_timer(FuriTimer* t) {
    FuriTimer** pp;

    for(pp = &armed; *pp; pp = &(*pp)->next) {
        if(*pp == t) {
            *pp = t->next;
            break;
        }
    }
    t->next = NULL;
    t->running = 0;
}

FuriTimer* furi_timer_alloc(FuriTimerCallback func, FuriTimerType type, void* context) {
    FuriTimer* t;

    furi_check(func);
    t = calloc(1, sizeof(FuriTimer));
    furi_check(t, "furi_timer_alloc: out of memory");
    t->cb = func;
    t->context = context;
    t->type = (uint8_t)type;
    return t;
}

void furi_timer_free(FuriTimer* t) {
    furi_check(t);
    if(t->running) unlink_timer(t);
    free(t);
}

FuriStatus furi_timer_start(FuriTimer* t, uint32_t ticks) {
    furi_check(t);
    furi_check(ticks > 0 && ticks < 0x80000000UL, "furi_timer_start: bad period");
    if(t->running) unlink_timer(t);
    t->period = ticks;
    t->expire = fport_now() + ticks;
    t->running = 1;
    t->next = armed;
    armed = t;
    return FuriStatusOk;
}

FuriStatus furi_timer_restart(FuriTimer* t, uint32_t ticks) {
    return furi_timer_start(t, ticks);
}

FuriStatus furi_timer_stop(FuriTimer* t) {
    furi_check(t);
    if(t->running) unlink_timer(t);
    return FuriStatusOk;
}

uint32_t furi_timer_is_running(FuriTimer* t) {
    furi_check(t);
    return t->running;
}

uint32_t furi_timer_get_expire_time(FuriTimer* t) {
    furi_check(t);
    return t->expire;
}

void furi_timer_pending_callback(FuriTimerPendigCallback callback, void* context, uint32_t arg) {
    Pending* p;

    furi_check(callback);
    p = malloc(sizeof(Pending));
    furi_check(p, "furi_timer_pending_callback: out of memory");
    p->next = NULL;
    p->cb = callback;
    p->context = context;
    p->arg = arg;
    if(pending_tail)
        pending_tail->next = p;
    else
        pending_head = p;
    pending_tail = p;
}

void furi_timer_set_thread_priority(FuriTimerThreadPriority priority) {
    UNUSED(priority);
}

void furi_timer_flush(void) {
}

bool ftimer_armed(void) {
    return armed != NULL || pending_head != NULL;
}

bool ftimer_run(uint32_t now, uint32_t* next) {
    FuriTimer* t;
    Pending* p;
    bool any = false;
    uint32_t d;

    /* Pending callbacks first, then due timers. A callback may start,
     * stop or free timers: rescan after each one. */
    while((p = pending_head) != NULL) {
        pending_head = p->next;
        if(!pending_head) pending_tail = NULL;
        ftimer_in_callback = 1;
        p->cb(p->context, p->arg);
        ftimer_in_callback = 0;
        free(p);
    }
again:
    for(t = armed; t; t = t->next) {
        if((int32_t)(t->expire - now) > 0) continue;
        if(t->type == FuriTimerTypePeriodic) {
            t->expire += t->period;
            /* fell behind by more than a period: no burst of calls */
            if((int32_t)(t->expire - now) <= 0) t->expire = now + t->period;
        } else {
            unlink_timer(t);
        }
        ftimer_in_callback = 1;
        t->cb(t->context);
        ftimer_in_callback = 0;
        goto again;
    }
    for(t = armed; t; t = t->next) {
        d = (uint32_t)(t->expire - now);
        if(!any || d < *next) *next = d;
        any = true;
    }
    if(pending_head) { /* queued by a timer callback */
        *next = 0;
        any = true;
    }
    return any;
}

size_t fdebug_sizeof_timer(void) {
    return sizeof(FuriTimer);
}
