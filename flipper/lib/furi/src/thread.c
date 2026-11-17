/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri (spec R5.1, D.1): cooperative threads, the scheduler, thread
 * flags, ticks and delays. API: stock furi/core/thread.h, kernel.h.
 * Written for Galtzagorriak MINIX; no stock code (the stock one wraps
 * FreeRTOS tasks). */
#include <stdlib.h>
#include <string.h>
#include "furi_i.h"

static FuriThread main_thread = {
    .name = "main",
    .state = FuriThreadStateRunning,
    .priority = FuriThreadPriorityNormal,
    .stack_size = 0,
};
static FuriThread* current = &main_thread;
static FuriThread* ready_head;
static FuriThread* ready_tail;
static FuriThread* timed_head; /* threads waiting with a timeout */

FuriThread* fsched_current(void) {
    return current;
}

/* ---------------------------------------------------------------- lists */

static void ready_put(FuriThread* t) {
    t->next = NULL;
    if(ready_tail)
        ready_tail->next = t;
    else
        ready_head = t;
    ready_tail = t;
}

static FuriThread* ready_get(void) {
    FuriThread* t = ready_head;
    if(t) {
        ready_head = t->next;
        if(!ready_head) ready_tail = NULL;
        t->next = NULL;
    }
    return t;
}

static void wq_put(FuriWaitq* wq, FuriThread* t) {
    t->wnext = NULL;
    t->wq = wq;
    if(wq->tail)
        wq->tail->wnext = t;
    else
        wq->head = t;
    wq->tail = t;
}

static void wq_remove(FuriThread* t) {
    FuriWaitq* wq = t->wq;
    FuriThread *p, *prev = NULL;

    if(!wq) return;
    for(p = wq->head; p; prev = p, p = p->wnext) {
        if(p != t) continue;
        if(prev)
            prev->wnext = p->wnext;
        else
            wq->head = p->wnext;
        if(wq->tail == p) wq->tail = prev;
        break;
    }
    t->wq = NULL;
    t->wnext = NULL;
}

static void timed_remove(FuriThread* t) {
    FuriThread** pp;

    if(!t->timed) return;
    for(pp = &timed_head; *pp; pp = &(*pp)->tnext) {
        if(*pp == t) {
            *pp = t->tnext;
            break;
        }
    }
    t->tnext = NULL;
    t->timed = 0;
}

/* Signed distance: wraps at 2^32 ms (49 days) like the stock tick. */
static int32_t tdiff(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

/* ------------------------------------------------------------ scheduler */

static void stack_check(FuriThread* t) {
    if(t == &main_thread) {
        if(!fport_main_stack_ok()) furi_crash("main thread: stack overflow");
        return;
    }
    if(t->stack) {
        const volatile uint32_t* g = t->stack;
        int i;

        for(i = 0; i < FURI_STACK_GUARD; i++)
            if(g[i] != FURI_STACK_CANARY) furi_crash("thread stack overflow");
    }
}

/* Make due timed waits ready; run due timers. The earliest deadline
 * from now into *wait; false if nothing is armed. */
static bool run_due(uint32_t* wait) {
    uint32_t now, next = 0;
    bool any;
    FuriThread *t, *tn;

    if(!timed_head && !ftimer_armed()) return false;
    now = fport_now();
    any = ftimer_run(now, &next);
    for(t = timed_head; t; t = tn) {
        tn = t->tnext;
        if(tdiff(t->wake_at, now) <= 0) {
            timed_remove(t);
            wq_remove(t);
            t->woken = 0;
            ready_put(t);
        } else if(!any || (uint32_t)tdiff(t->wake_at, now) < next) {
            next = (uint32_t)tdiff(t->wake_at, now);
            any = true;
        }
    }
    *wait = next;
    return any;
}

/* The current thread is blocked, yielding (already on the ready queue)
 * or finished: run the next one. Returns when current runs again.
 * due_done: run_due() was just called. */
static void schedule(bool due_done) {
    FuriThread *prev = current, *next;
    uint32_t wait;
    bool armed;

    stack_check(prev);
    for(;;) {
        armed = due_done ? true : run_due(&wait);
        if(ready_head) break;
        if(due_done) {
            due_done = false;
            continue;
        }
        if(!armed) {
            if(!ffd_waiting()) furi_crash("deadlock: every thread waits forever");
            wait = FuriWaitForever; /* until an fd is ready */
        }
        if(wait) ffd_idle(wait);
    }
    ffd_peek(); /* fd waiters get a look while others run */
    next = ready_get();
    if(next == prev) return;
    current = next;
    fport_ctx_swap(&prev->ctx, &next->ctx);
    /* back on prev's stack */
}

bool fsched_block(FuriWaitq* wq, uint32_t timeout) {
    FuriThread* t = current;

    if(timeout == 0) return false;
    fsched_check_not_in_callback(timeout);
    t->woken = 0;
    if(wq) wq_put(wq, t);
    if(timeout != FuriWaitForever) {
        t->wake_at = fport_now() + timeout;
        t->tnext = timed_head;
        timed_head = t;
        t->timed = 1;
    }
    schedule(false);
    return t->woken != 0;
}

static void wake(FuriThread* t) {
    wq_remove(t);
    timed_remove(t);
    t->woken = 1;
    ready_put(t);
}

void fsched_wake_one(FuriWaitq* wq) {
    if(wq->head) wake(wq->head);
}

void fsched_wake_all(FuriWaitq* wq) {
    while(wq->head)
        wake(wq->head);
}

FuriStatus fsched_wait(FuriWaitq* wq, uint32_t timeout, FuriWaitState* w) {
    uint32_t left = timeout;

    if(timeout == 0) return FuriStatusErrorResource;
    if(!w->started) {
        w->started = 1;
        if(timeout != FuriWaitForever) w->start = fport_now();
    } else {
        left = fsched_left(timeout, w->start);
        if(left == 0) return FuriStatusErrorTimeout;
    }
    fsched_block(wq, left);
    return FuriStatusOk;
}

uint32_t fsched_left(uint32_t timeout, uint32_t start) {
    uint32_t used;

    if(timeout == FuriWaitForever) return timeout;
    used = fport_now() - start;
    return used >= timeout ? 0 : timeout - used;
}

void fsched_check_not_in_callback(uint32_t timeout) {
    if(timeout != 0 && ftimer_in_callback) furi_crash("blocking call in a timer callback");
}

void furi_thread_yield(void) {
    uint32_t wait;

    fsched_check_not_in_callback(1);
    run_due(&wait);
    ffd_peek(); /* a thread that only yields must not starve fd waiters */
    if(!ready_head) return; /* nobody else to run */
    ready_put(current);
    schedule(true);
}

/* --------------------------------------------------------------- thread */

static void set_state(FuriThread* t, FuriThreadState state) {
    t->state = (uint8_t)state;
    if(t->state_callback) t->state_callback(t, state, t->state_context);
}

static void thread_body(void) {
    FuriThread* t = current;

    set_state(t, FuriThreadStateRunning);
    t->ret = t->callback(t->context);
    set_state(t, FuriThreadStateStopping);
    t->flags = 0;
    set_state(t, FuriThreadStateStopped);
    fsched_wake_all(&t->join_wq);
    schedule(false); /* t is on no queue: never comes back */
    furi_crash("stopped thread resumed");
}

FuriThread* furi_thread_alloc(void) {
    FuriThread* t = calloc(1, sizeof(FuriThread));

    furi_check(t, "furi_thread_alloc: out of memory");
    t->state = FuriThreadStateStopped;
    t->priority = FuriThreadPriorityNormal;
    return t;
}

FuriThread* furi_thread_alloc_ex(
    const char* name,
    uint32_t stack_size,
    FuriThreadCallback callback,
    void* context) {
    FuriThread* t = furi_thread_alloc();

    furi_thread_set_name(t, name);
    furi_thread_set_stack_size(t, stack_size);
    furi_thread_set_callback(t, callback);
    furi_thread_set_context(t, context);
    return t;
}

static void stack_release(FuriThread* t) {
    if(!t->stack) return;
    fport_stack_free(t->stack, t->stack_size);
    t->stack = NULL;
}

void furi_thread_free(FuriThread* t) {
    furi_check(t);
    furi_check(t != current, "furi_thread_free: own thread");
    furi_check(t->state == FuriThreadStateStopped, "furi_thread_free: thread running");
    stack_release(t);
    free(t->name);
    free(t->appid);
    free(t);
}

static char* dup_or_null(const char* s) {
    char* d;

    if(!s) return NULL;
    d = malloc(strlen(s) + 1);
    furi_check(d, "out of memory");
    strcpy(d, s);
    return d;
}

void furi_thread_set_name(FuriThread* t, const char* name) {
    furi_check(t && t != &main_thread);
    furi_check(t->state == FuriThreadStateStopped);
    free(t->name);
    t->name = dup_or_null(name);
}

void furi_thread_set_appid(FuriThread* t, const char* appid) {
    furi_check(t && t != &main_thread);
    free(t->appid);
    t->appid = dup_or_null(appid);
}

void furi_thread_set_stack_size(FuriThread* t, size_t stack_size) {
    furi_check(t && t != &main_thread);
    furi_check(t->state == FuriThreadStateStopped);
    stack_release(t);
    if(stack_size < FURI_STACK_MIN) stack_size = FURI_STACK_MIN;
    t->stack_size = (uint32_t)((stack_size + 7U) & ~(size_t)7U);
}

uint32_t furi_thread_get_stack_size(FuriThread* t) {
    furi_check(t);
    return t->stack_size;
}

void furi_thread_set_callback(FuriThread* t, FuriThreadCallback callback) {
    furi_check(t && t->state == FuriThreadStateStopped);
    t->callback = callback;
}

void furi_thread_set_context(FuriThread* t, void* context) {
    furi_check(t && t->state == FuriThreadStateStopped);
    t->context = context;
}

void furi_thread_set_priority(FuriThread* t, FuriThreadPriority priority) {
    furi_check(t);
    furi_check(priority <= FuriThreadPriorityIsr);
    t->priority = (uint8_t)priority;
}

FuriThreadPriority furi_thread_get_priority(FuriThread* t) {
    furi_check(t);
    return (FuriThreadPriority)t->priority;
}

void furi_thread_set_current_priority(FuriThreadPriority priority) {
    furi_thread_set_priority(current, priority);
}

FuriThreadPriority furi_thread_get_current_priority(void) {
    return (FuriThreadPriority)current->priority;
}

void furi_thread_set_state_callback(FuriThread* t, FuriThreadStateCallback callback) {
    furi_check(t && t->state == FuriThreadStateStopped);
    t->state_callback = callback;
}

void furi_thread_set_state_context(FuriThread* t, void* context) {
    furi_check(t && t->state == FuriThreadStateStopped);
    t->state_context = context;
}

FuriThreadState furi_thread_get_state(FuriThread* t) {
    furi_check(t);
    return (FuriThreadState)t->state;
}

void furi_thread_start(FuriThread* t) {
    int i;

    furi_check(t && t != &main_thread);
    furi_check(t->callback, "furi_thread_start: no callback");
    furi_check(t->state == FuriThreadStateStopped, "furi_thread_start: already running");
    if(t->stack_size == 0) furi_thread_set_stack_size(t, 1024);
    if(!t->stack) {
        t->stack = fport_stack_alloc(t->stack_size);
        furi_check(t->stack, "furi_thread_start: no room for the stack (chmem)");
    }
    memset(t->stack, FURI_STACK_PAINT, t->stack_size);
    for(i = 0; i < FURI_STACK_GUARD; i++)
        ((uint32_t*)t->stack)[i] = FURI_STACK_CANARY;
    fport_ctx_init(&t->ctx, t->stack, t->stack_size, thread_body);
    t->flags = 0;
    t->ret = 0;
    set_state(t, FuriThreadStateStarting);
    ready_put(t);
}

bool furi_thread_join(FuriThread* t) {
    furi_check(t);
    furi_check(t != current, "furi_thread_join: own thread");
    while(t->state != FuriThreadStateStopped)
        fsched_block(&t->join_wq, FuriWaitForever);
    stack_release(t);
    return true;
}

int32_t furi_thread_get_return_code(FuriThread* t) {
    furi_check(t && t->state == FuriThreadStateStopped);
    return t->ret;
}

FuriThreadId furi_thread_get_id(FuriThread* t) {
    furi_check(t);
    return t;
}

FuriThreadId furi_thread_get_current_id(void) {
    return current;
}

FuriThread* furi_thread_get_current(void) {
    return current;
}

const char* furi_thread_get_name(FuriThreadId id) {
    return id ? ((FuriThread*)id)->name : NULL;
}

const char* furi_thread_get_appid(FuriThreadId id) {
    return id ? ((FuriThread*)id)->appid : NULL;
}

uint32_t furi_thread_get_stack_space(FuriThreadId id) {
    FuriThread* t = id;
    const uint8_t* p;
    uint32_t n = 0;

    if(!t || !t->stack) return 0;
    p = (const uint8_t*)t->stack + FURI_STACK_GUARD * sizeof(uint32_t);
    while(n + FURI_STACK_GUARD * sizeof(uint32_t) < t->stack_size && p[n] == FURI_STACK_PAINT)
        n++;
    return n;
}

void furi_thread_enable_heap_trace(FuriThread* t) {
    UNUSED(t);
}

void furi_thread_disable_heap_trace(FuriThread* t) {
    UNUSED(t);
}

size_t furi_thread_get_heap_size(FuriThread* t) {
    UNUSED(t);
    return 0;
}

/* --------------------------------------------------------- thread flags */

uint32_t furi_thread_flags_set(FuriThreadId id, uint32_t flags) {
    FuriThread* t = id;

    if(!t || (flags & FuriFlagError)) return FuriFlagErrorParameter;
    t->flags |= flags;
    fsched_wake_all(&t->flags_wq);
    return t->flags;
}

uint32_t furi_thread_flags_clear(uint32_t flags) {
    uint32_t old = current->flags;

    if(flags & FuriFlagError) return FuriFlagErrorParameter;
    current->flags &= ~flags;
    return old;
}

uint32_t furi_thread_flags_get(void) {
    return current->flags;
}

uint32_t furi_thread_flags_wait(uint32_t flags, uint32_t options, uint32_t timeout) {
    FuriThread* t = current;
    FuriWaitState w = {0, 0};
    uint32_t got;

    if(flags & FuriFlagError) return FuriFlagErrorParameter;
    for(;;) {
        got = t->flags;
        if((options & FuriFlagWaitAll) ? (got & flags) == flags : (got & flags) != 0) {
            if(!(options & FuriFlagNoClear)) t->flags &= ~flags;
            return got;
        }
        switch(fsched_wait(&t->flags_wq, timeout, &w)) {
        case FuriStatusOk:
            break;
        case FuriStatusErrorResource:
            return FuriFlagErrorResource;
        default:
            return FuriFlagErrorTimeout;
        }
    }
}

/* --------------------------------------------------------------- kernel */

bool furi_kernel_is_irq_or_masked(void) {
    return false;
}

bool furi_kernel_is_running(void) {
    return true;
}

/* Nothing preempts a cooperative thread: the lock is only a counter. */
static int32_t kernel_lock;

int32_t furi_kernel_lock(void) {
    int32_t old = kernel_lock;
    kernel_lock = 1;
    return old;
}

int32_t furi_kernel_unlock(void) {
    int32_t old = kernel_lock;
    kernel_lock = 0;
    return old;
}

int32_t furi_kernel_restore_lock(int32_t lock) {
    kernel_lock = lock ? 1 : 0;
    return kernel_lock;
}

uint32_t furi_kernel_get_tick_frequency(void) {
    return 1000;
}

uint32_t furi_get_tick(void) {
    return fport_now();
}

uint32_t furi_ms_to_ticks(uint32_t ms) {
    return ms;
}

void furi_delay_tick(uint32_t ticks) {
    uint32_t start = fport_now(), left = ticks;

    if(ticks == 0) {
        furi_thread_yield();
        return;
    }
    while(left) {
        fsched_block(NULL, left);
        left = fsched_left(ticks, start);
    }
}

FuriStatus furi_delay_until_tick(uint32_t tick) {
    int32_t d = tdiff(tick, fport_now());

    if(d <= 0) return FuriStatusErrorParameter;
    furi_delay_tick((uint32_t)d);
    return FuriStatusOk;
}

void furi_delay_ms(uint32_t ms) {
    furi_delay_tick(ms);
}

void furi_delay_us(uint32_t us) {
    furi_delay_tick((us + 999U) / 1000U);
}

void furi_init(void) {
}

void furi_run(void) {
}

void* furi_malloc_checked(size_t size) {
    void* p = malloc(size ? size : 1);
    furi_check(p, "out of memory");
    return p;
}

size_t fdebug_sizeof_thread(void) {
    return sizeof(FuriThread);
}
