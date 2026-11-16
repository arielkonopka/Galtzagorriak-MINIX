/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri port layer for Galtzagorriak MINIX (spec R5.1, D.1).
 *
 * Thread stacks: MM (adjust() in mm/break.c) only accepts a stack pointer
 * inside the stack segment: a signal or a brk() while sp points into the
 * heap kills the process or fails. So the stacks are not malloc'ed: they
 * are cut from the gap below the main stack (the program's chmem), top
 * down, after FURI_MAIN_STACK bytes left to the main thread. A new piece
 * of the gap is claimed with a brk() made with sp at its bottom: MM then
 * grows the stack segment over it (or says ENOMEM if the heap is in the
 * way), and the heap can never be given that memory later. A canary word
 * at the boundary catches a main stack that runs into the thread stacks.
 *
 * Time: times() (60 Hz) gives the ms clock. Idle waits: alarm() and
 * sigsuspend(), whole seconds, until a millisecond user alarm or poll()
 * (R4b.6) exists; only fport_idle() changes then. libfuri owns SIGALRM
 * while it sleeps (the old handler comes back after). */
#include <sys/types.h>
#include <sys/times.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "furi_i.h"

#ifndef FURI_MAIN_STACK
#define FURI_MAIN_STACK 3072 /* printf alone takes ~1.2 KB */
#endif
#define PROBE_STACK 256 /* the claim's brk() call chain runs here */
#define CTX_FRAME 9 /* r4-r11, lr */

void fport_ctx_swap_sp(void** save_sp, void* new_sp);
void fport_ctx_start(void);
void* fport_pic_base(void);
void* fport_sp(void);
int _doprnt(const char* format, va_list ap, FILE* stream);

/* ------------------------------------------------------------------ time */

uint32_t fport_now(void) {
    static uint32_t last;
    static uint64_t ticks; /* since the first call; no jump at a wrap */
    static int started;
    struct tms tb;
    uint32_t t = (uint32_t)times(&tb);

    if(!started) {
        started = 1;
        last = t;
    }
    ticks += (uint32_t)(t - last);
    last = t;
    return (uint32_t)(ticks * 1000U / CLOCKS_PER_SEC);
}

static void on_alarm(int sig) {
    (void)sig;
}

void fport_idle(uint32_t ms) {
    struct sigaction sa, old;
    sigset_t block, prev, wait;
    unsigned secs = (unsigned)((ms + 999U) / 1000U);

    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigemptyset(&block);
    sigaddset(&block, SIGALRM);
    /* SIGALRM blocked from alarm() until sigsuspend(): no lost wake-up */
    sigprocmask(SIG_BLOCK, &block, &prev);
    sigaction(SIGALRM, &sa, &old);
    alarm(secs);
    wait = prev;
    sigdelset(&wait, SIGALRM);
    sigsuspend(&wait);
    alarm(0);
    sigaction(SIGALRM, &old, NULL);
    sigprocmask(SIG_SETMASK, &prev, NULL);
}

/* ---------------------------------------------------------------- format */

int fport_vformat(char* buf, size_t size, const char* fmt, va_list ap) {
    FILE f;
    int r;

    if(size == 0) return -1;
    /* the vsprintf() stream with a limit: the C library's __flushbuf
     * refuses a stream without a file (fd -1), so _doprnt stops at the
     * end of buf and returns a negative count */
    f._fd = -1;
    f._flags = _IOWRITE | _IONBF | _IOWRITING;
    f._buf = (unsigned char*)buf;
    f._ptr = (unsigned char*)buf;
    f._count = (int)size - 1; /* room for the NUL */
    f._bufsiz = 0;
    r = _doprnt(fmt, ap, &f);
    if(r < 0 || (size_t)r >= size) return -1;
    buf[r] = '\0';
    return r;
}

/* ---------------------------------------------------------------- stacks */

typedef struct Hole {
    struct Hole* next; /* by address */
    size_t size;
} Hole;

static char* gap_top; /* the canary under the main stack; stacks below */
static char* gap_low; /* lowest claimed address */
static Hole* holes; /* freed stacks in [gap_low, gap_top) */

static FPortCtx probe_ret, probe_ctx;
static int probe_ok;

static void probe(void) {
    /* sp is near the bottom of the new piece: MM's adjust() grows the
     * stack segment down to it, or refuses */
    probe_ok = brk(sbrk(0)) == 0;
    fport_ctx_swap(&probe_ctx, &probe_ret);
}

static bool claim(char* lo, size_t size) {
    fport_ctx_init(&probe_ctx, lo, size < PROBE_STACK ? size : PROBE_STACK, probe);
    probe_ok = 0;
    fport_ctx_swap(&probe_ret, &probe_ctx);
    return probe_ok;
}

bool fport_main_stack_ok(void) {
    return gap_low == gap_top || *(volatile uint32_t*)gap_top == FURI_STACK_CANARY;
}

void* fport_stack_alloc(size_t size) {
    Hole **pp, *h;
    char* lo;

    size = (size + 7U) & ~(size_t)7U;
    if(!gap_top) {
        /* 8 bytes for the canary, written with the first claim */
        gap_top = (char*)(((uintptr_t)fport_sp() - FURI_MAIN_STACK - 8U) & ~(uintptr_t)7U);
        gap_low = gap_top;
    }
    /* first fit in the freed stacks */
    for(pp = &holes; (h = *pp) != NULL; pp = &h->next) {
        if(h->size < size) continue;
        if(h->size - size >= FURI_STACK_MIN) {
            /* keep the low part as a hole, hand out the top */
            h->size -= size;
            return (char*)h + h->size;
        }
        *pp = h->next;
        return h;
    }
    lo = gap_low - size;
    if(lo > gap_low || !claim(lo, size)) return NULL;
    if(gap_low == gap_top) *(uint32_t*)gap_top = FURI_STACK_CANARY;
    gap_low = lo;
    return lo;
}

void fport_stack_free(void* p, size_t size) {
    Hole **pp, *h = p, *n;

    size = (size + 7U) & ~(size_t)7U;
    h->size = size;
    for(pp = &holes; *pp && (char*)*pp < (char*)h; pp = &(*pp)->next)
        ;
    h->next = *pp;
    *pp = h;
    /* merge with the next hole, then with the one before */
    n = h->next;
    if(n && (char*)h + h->size == (char*)n) {
        h->size += n->size;
        h->next = n->next;
    }
    if(pp != &holes) {
        Hole* prev = (Hole*)((char*)pp - offsetof(Hole, next));
        if((char*)prev + prev->size == (char*)h) {
            prev->size += h->size;
            prev->next = h->next;
        }
    }
}

/* ------------------------------------------------------------- contexts */

void fport_ctx_init(FPortCtx* ctx, void* lo, size_t size, void (*entry)(void)) {
    uint32_t* sp = (uint32_t*)(((uintptr_t)lo + size) & ~(uintptr_t)7U);

    sp -= CTX_FRAME + 2; /* + 2: sp 8-byte aligned after the pop */
    sp[0] = 0; /* r4 */
    sp[1] = (uint32_t)(uintptr_t)entry; /* r5 */
    sp[2] = 0; /* r6 */
    sp[3] = 0; /* r7 */
    sp[4] = 0; /* r8 */
    sp[5] = (uint32_t)(uintptr_t)fport_pic_base(); /* r9 */
    sp[6] = 0; /* r10 */
    sp[7] = 0; /* r11 */
    sp[8] = (uint32_t)(uintptr_t)fport_ctx_start; /* lr -> pc */
    sp[9] = 0;
    sp[10] = 0;
    ctx->sp = sp;
}

void fport_ctx_swap(FPortCtx* from, FPortCtx* to) {
    fport_ctx_swap_sp(&from->sp, to->sp);
}
