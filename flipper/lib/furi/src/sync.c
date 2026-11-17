/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri (spec R5.1, D.1): message queues, event flags, mutexes and
 * semaphores for the cooperative threads of one process. API and return
 * codes as the stock furi/core/{message_queue,event_flag,mutex,
 * semaphore}.h: timeout 0 and nothing there -> ErrorResource, a wait that
 * ran out -> ErrorTimeout. Waiters are woken in FIFO order and check
 * again (a woken thread may find the slot taken). */
#include <stdlib.h>
#include <string.h>
#include "furi_i.h"

/* -------------------------------------------------------- message queue */

struct FuriMessageQueue {
    FuriWaitq not_empty;
    FuriWaitq not_full;
    uint32_t capacity;
    uint32_t size; /* bytes per message */
    uint32_t count;
    uint32_t head; /* next to get */
    uint8_t buf[]; /* capacity * size */
};

FuriMessageQueue* furi_message_queue_alloc(uint32_t msg_count, uint32_t msg_size) {
    FuriMessageQueue* q;

    furi_check(msg_count > 0 && msg_size > 0);
    furi_check(msg_count <= 0xFFFFU && msg_size <= 0xFFFFU, "message queue too big");
    q = calloc(1, sizeof(FuriMessageQueue) + (size_t)msg_count * msg_size);
    furi_check(q, "furi_message_queue_alloc: out of memory");
    q->capacity = msg_count;
    q->size = msg_size;
    return q;
}

void furi_message_queue_free(FuriMessageQueue* q) {
    furi_check(q);
    furi_check(!q->not_empty.head && !q->not_full.head, "message queue freed with waiters");
    free(q);
}

FuriStatus furi_message_queue_put(FuriMessageQueue* q, const void* msg, uint32_t timeout) {
    FuriWaitState w = {0, 0};
    FuriStatus st;
    uint32_t tail;

    furi_check(q && msg);
    while(q->count == q->capacity) {
        st = fsched_wait(&q->not_full, timeout, &w);
        if(st != FuriStatusOk) return st;
    }
    tail = q->head + q->count;
    if(tail >= q->capacity) tail -= q->capacity;
    memcpy(q->buf + (size_t)tail * q->size, msg, q->size);
    q->count++;
    fsched_wake_one(&q->not_empty);
    return FuriStatusOk;
}

FuriStatus furi_message_queue_get(FuriMessageQueue* q, void* msg, uint32_t timeout) {
    FuriWaitState w = {0, 0};
    FuriStatus st;

    furi_check(q && msg);
    while(q->count == 0) {
        st = fsched_wait(&q->not_empty, timeout, &w);
        if(st != FuriStatusOk) return st;
    }
    memcpy(msg, q->buf + (size_t)q->head * q->size, q->size);
    if(++q->head == q->capacity) q->head = 0;
    q->count--;
    fsched_wake_one(&q->not_full);
    return FuriStatusOk;
}

uint32_t furi_message_queue_get_capacity(FuriMessageQueue* q) {
    furi_check(q);
    return q->capacity;
}

uint32_t furi_message_queue_get_message_size(FuriMessageQueue* q) {
    furi_check(q);
    return q->size;
}

uint32_t furi_message_queue_get_count(FuriMessageQueue* q) {
    furi_check(q);
    return q->count;
}

uint32_t furi_message_queue_get_space(FuriMessageQueue* q) {
    furi_check(q);
    return q->capacity - q->count;
}

FuriStatus furi_message_queue_reset(FuriMessageQueue* q) {
    furi_check(q);
    q->count = 0;
    q->head = 0;
    fsched_wake_all(&q->not_full);
    return FuriStatusOk;
}

/* ---------------------------------------------------------- event flags */

struct FuriEventFlag {
    FuriWaitq wq;
    uint32_t flags;
};

FuriEventFlag* furi_event_flag_alloc(void) {
    FuriEventFlag* e = calloc(1, sizeof(FuriEventFlag));

    furi_check(e, "furi_event_flag_alloc: out of memory");
    return e;
}

void furi_event_flag_free(FuriEventFlag* e) {
    furi_check(e);
    furi_check(!e->wq.head, "event flag freed with waiters");
    free(e);
}

uint32_t furi_event_flag_set(FuriEventFlag* e, uint32_t flags) {
    furi_check(e);
    furi_check((flags & FURI_EVENT_FLAG_INVALID_BITS) == 0);
    e->flags |= flags;
    fsched_wake_all(&e->wq);
    return e->flags;
}

uint32_t furi_event_flag_clear(FuriEventFlag* e, uint32_t flags) {
    uint32_t old;

    furi_check(e);
    furi_check((flags & FURI_EVENT_FLAG_INVALID_BITS) == 0);
    old = e->flags;
    e->flags &= ~flags;
    return old;
}

uint32_t furi_event_flag_get(FuriEventFlag* e) {
    furi_check(e);
    return e->flags;
}

uint32_t furi_event_flag_wait(FuriEventFlag* e, uint32_t flags, uint32_t options, uint32_t timeout) {
    FuriWaitState w = {0, 0};
    uint32_t got;

    furi_check(e);
    furi_check(flags != 0 && (flags & FURI_EVENT_FLAG_INVALID_BITS) == 0);
    for(;;) {
        got = e->flags;
        if((options & FuriFlagWaitAll) ? (got & flags) == flags : (got & flags) != 0) {
            if(!(options & FuriFlagNoClear)) e->flags &= ~flags;
            return got;
        }
        switch(fsched_wait(&e->wq, timeout, &w)) {
        case FuriStatusOk:
            break;
        case FuriStatusErrorResource:
            return FuriFlagErrorResource;
        default:
            return FuriFlagErrorTimeout;
        }
    }
}

/* ---------------------------------------------------------------- mutex */

struct FuriMutex {
    FuriWaitq wq;
    FuriThread* owner;
    uint32_t count;
    uint8_t type;
};

FuriMutex* furi_mutex_alloc(FuriMutexType type) {
    FuriMutex* m;

    furi_check(type == FuriMutexTypeNormal || type == FuriMutexTypeRecursive);
    m = calloc(1, sizeof(FuriMutex));
    furi_check(m, "furi_mutex_alloc: out of memory");
    m->type = (uint8_t)type;
    return m;
}

void furi_mutex_free(FuriMutex* m) {
    furi_check(m);
    furi_check(!m->wq.head, "mutex freed with waiters");
    free(m);
}

FuriStatus furi_mutex_acquire(FuriMutex* m, uint32_t timeout) {
    FuriThread* me = fsched_current();
    FuriWaitState w = {0, 0};
    FuriStatus st;

    furi_check(m);
    if(m->owner == me && m->type == FuriMutexTypeRecursive) {
        m->count++;
        return FuriStatusOk;
    }
    while(m->owner) {
        st = fsched_wait(&m->wq, timeout, &w);
        if(st != FuriStatusOk) return st;
    }
    m->owner = me;
    m->count = 1;
    return FuriStatusOk;
}

FuriStatus furi_mutex_release(FuriMutex* m) {
    furi_check(m);
    if(m->owner != fsched_current()) return FuriStatusErrorResource;
    if(--m->count == 0) {
        m->owner = NULL;
        fsched_wake_one(&m->wq);
    }
    return FuriStatusOk;
}

FuriThreadId furi_mutex_get_owner(FuriMutex* m) {
    furi_check(m);
    return m->owner;
}

/* ------------------------------------------------------------ semaphore */

struct FuriSemaphore {
    FuriWaitq wq;
    uint32_t count;
    uint32_t max;
};

FuriSemaphore* furi_semaphore_alloc(uint32_t max_count, uint32_t initial_count) {
    FuriSemaphore* s;

    furi_check(max_count > 0 && initial_count <= max_count);
    s = calloc(1, sizeof(FuriSemaphore));
    furi_check(s, "furi_semaphore_alloc: out of memory");
    s->max = max_count;
    s->count = initial_count;
    return s;
}

void furi_semaphore_free(FuriSemaphore* s) {
    furi_check(s);
    furi_check(!s->wq.head, "semaphore freed with waiters");
    free(s);
}

FuriStatus furi_semaphore_acquire(FuriSemaphore* s, uint32_t timeout) {
    FuriWaitState w = {0, 0};
    FuriStatus st;

    furi_check(s);
    while(s->count == 0) {
        st = fsched_wait(&s->wq, timeout, &w);
        if(st != FuriStatusOk) return st;
    }
    s->count--;
    return FuriStatusOk;
}

FuriStatus furi_semaphore_release(FuriSemaphore* s) {
    furi_check(s);
    if(s->count == s->max) return FuriStatusErrorResource;
    s->count++;
    fsched_wake_one(&s->wq);
    return FuriStatusOk;
}

uint32_t furi_semaphore_get_count(FuriSemaphore* s) {
    furi_check(s);
    return s->count;
}

/* For the tests' RAM report (test_furi.c). */
size_t furi_debug_sizeof(int what) {
    switch(what) {
    case 0:
        return fdebug_sizeof_thread();
    case 1:
        return sizeof(FuriMessageQueue);
    case 2:
        return sizeof(FuriEventFlag);
    case 3:
        return sizeof(FuriMutex);
    case 4:
        return sizeof(FuriSemaphore);
    case 5:
        return fdebug_sizeof_timer();
    case 6:
        return fdebug_sizeof_string();
    }
    return 0;
}
