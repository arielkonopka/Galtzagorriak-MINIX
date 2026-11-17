/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri internals: the cooperative scheduler and the port layer.
 *
 * The scheduler: one thread runs at a time. A thread gives up the CPU
 * only in fsched_block() (a queue, flags, mutex, semaphore, delay or join
 * that has to wait) or furi_thread_yield(). Waiting threads sit on the
 * wait queue of their object and, with a timeout, on the timed list.
 * When nothing is ready, the scheduler runs due timers, then sleeps in
 * ffd_idle() (one poll() over the fds threads wait for) until the next
 * deadline or a ready fd. */
#ifndef FURI_I_H
#define FURI_I_H

#include <sys/types.h>
#include <poll.h>
#include <stdarg.h>
#include "furi.h"

/* ---- port layer (port_minix.c / port_host.c) ---- */
#ifdef FURI_HOST
#include <ucontext.h>
typedef struct {
    ucontext_t uc;
} FPortCtx;
#else
typedef struct {
    void* sp; /* saved r4-r11, lr on the thread's own stack */
} FPortCtx;
#endif

/* Milliseconds, monotonic, wraps at 2^32. */
uint32_t fport_now(void);
/* poll(fds, n, ms): wait at most ms (FuriWaitForever: no limit) for one
 * of the n fds (n may be 0: a plain sleep). May return early (a signal).
 * The number of ready entries, 0 on a timeout or a signal. */
int fport_poll(struct pollfd* fds, unsigned n, uint32_t ms);
/* One clock tick in ms, rounded up: how often a device whose "ready"
 * was only a hint is tried again (fdwait.c). */
#ifdef FURI_HOST
#define FPORT_TICK_MS 10U
#else
#define FPORT_TICK_MS 17U /* 60 Hz */
#endif
/* vsnprintf: the length, or -1 if it did not fit in size bytes. */
int fport_vformat(char* buf, size_t size, const char* fmt, va_list ap);
/* A stack for a thread: size bytes, 8-aligned; NULL if no room. */
void* fport_stack_alloc(size_t size);
void fport_stack_free(void* lo, size_t size);
/* Main stack overflow check: false if the main thread ran into the
 * thread stacks. */
bool fport_main_stack_ok(void);
void fport_ctx_init(FPortCtx* ctx, void* lo, size_t size, void (*entry)(void));
void fport_ctx_swap(FPortCtx* from, FPortCtx* to);

/* ---- scheduler (thread.c) ---- */
typedef struct {
    FuriThread* head;
    FuriThread* tail;
} FuriWaitq;

#define FURI_STACK_CANARY 0x57AC0DE5UL
#define FURI_STACK_GUARD 4 /* canary words at the bottom of a stack */
#define FURI_STACK_PAINT 0xA5U
#define FURI_STACK_MIN 512U

struct FuriThread {
    FuriThread* next; /* ready queue */
    FuriThread* wnext; /* wait queue */
    FuriThread* tnext; /* timed list */
    FuriWaitq* wq; /* the wait queue it is on */
    char* name;
    char* appid;
    FuriThreadCallback callback;
    void* context;
    FuriThreadStateCallback state_callback;
    void* state_context;
    void* stack; /* lowest address; NULL when not allocated */
    uint32_t stack_size;
    uint32_t wake_at;
    uint32_t flags; /* thread flags */
    int32_t ret;
    FuriWaitq join_wq;
    FuriWaitq flags_wq;
    FPortCtx ctx;
    uint8_t state; /* FuriThreadState */
    uint8_t priority;
    uint8_t woken; /* set by fsched_wake(), clear on timeout */
    uint8_t timed; /* on the timed list */
};

FuriThread* fsched_current(void);
/* Wait on wq (may be NULL) for at most timeout ms. true: woken by
 * fsched_wake*(); false: timed out (or timeout 0). */
bool fsched_block(FuriWaitq* wq, uint32_t timeout);
void fsched_wake_one(FuriWaitq* wq);
void fsched_wake_all(FuriWaitq* wq);
/* One step of a wait-and-check-again loop: the first call blocks for
 * timeout, later ones for what is left of it. Ok: woken or timed out,
 * check again; ErrorResource: timeout 0; ErrorTimeout: the time is up. */
typedef struct {
    uint32_t start;
    uint8_t started;
} FuriWaitState;
FuriStatus fsched_wait(FuriWaitq* wq, uint32_t timeout, FuriWaitState* w);
/* Timeout bookkeeping for retry loops: the ms left of timeout since
 * start, 0 if it has run out. FuriWaitForever stays forever. */
uint32_t fsched_left(uint32_t timeout, uint32_t start);
void fsched_check_not_in_callback(uint32_t timeout);

/* ---- timers (timer.c), called by the scheduler ---- */
/* Any timer running or pending callback queued? */
bool ftimer_armed(void);
/* Runs due timers and pending callbacks. Returns true if a timer is
 * still armed, with the ms from now to the earliest one in *next. */
bool ftimer_run(uint32_t now, uint32_t* next);
extern uint8_t ftimer_in_callback;

/* ---- fd waits (fdwait.c), internal (for R5.2/R5.4) ---- */
typedef struct FuriFdWait FuriFdWait;
#define FFD_MAX 8 /* fds waited for at once (poll set on the idle stack) */
#define FFD_PEEK 16 /* switches between two looks while threads run */
/* Block the calling thread (only it) until fd is ready for events
 * (POLLIN, POLLOUT) or timeout ms. The revents (POLLERR, POLLHUP and
 * POLLNVAL included), 0 on a timeout. retry: the last "ready" was only a
 * hint (a non-blocking read found nothing): leave fd out of poll() and
 * wake after one tick (FPORT_TICK_MS) with revents = events. */
int ffd_wait(int fd, int events, uint32_t timeout, bool retry);
/* read() that blocks only the calling thread, at most timeout ms (0: do
 * not wait). Reads with O_NONBLOCK; -1 with errno EAGAIN on a timeout. */
ssize_t ffd_read(int fd, void* buf, size_t n, uint32_t timeout);
/* Any thread waiting for an fd? */
bool ffd_waiting(void);
/* Nothing ready: poll the waited fds for at most ms (FuriWaitForever: no
 * limit), wake the threads whose fd is ready. */
void ffd_idle(uint32_t ms);
/* Threads ready: every FFD_PEEK calls, ffd_idle(0). */
void ffd_peek(void);
extern uint32_t ffd_reads; /* read() calls by ffd_read() */

/* ---- log (check.c) ---- */
void flog_write(const char* s, size_t n);

/* ---- sizes for the tests' RAM report ---- */
size_t fdebug_sizeof_thread(void);
size_t fdebug_sizeof_timer(void);
size_t fdebug_sizeof_string(void);
size_t fdebug_sizeof_fdwait(void);
size_t furi_debug_sizeof(int what);

#endif
