/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri core tests (spec R5.1): every primitive, on the host
 * (make -C flipper/lib/furi test) and on MINIX in QEMU (/bin/furitest,
 * run by m2test). Exit status 0 when all pass.
 * Times differ: the host sleeps in ms, MINIX in whole seconds until
 * poll() (R4b.6), so the timing checks are loose there. */
#include <sys/types.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <furi.h>

static int fails, checks;

#define CHECK(c, what)                                       \
    do {                                                     \
        checks++;                                            \
        if(!(c)) {                                           \
            fails++;                                         \
            printf("  FAIL %s (line %d)\n", what, __LINE__); \
        }                                                    \
    } while(0)

/* ------------------------------------------------------------- threads */

static char order[64];
static int order_n;

static void mark(char c) {
    if(order_n < (int)sizeof(order) - 1) order[order_n++] = c;
}

static int32_t worker(void* ctx) {
    char c = *(char*)ctx;
    int i;

    for(i = 0; i < 3; i++) {
        mark(c);
        furi_thread_yield();
    }
    return c;
}

static int state_log[8], state_n;

static void on_state(FuriThread* t, FuriThreadState s, void* ctx) {
    (void)t;
    (void)ctx;
    if(state_n < 8) state_log[state_n++] = s;
}

static FuriThreadId seen_id;

static int32_t who_am_i(void* ctx) {
    (void)ctx;
    seen_id = furi_thread_get_current_id();
    return 7;
}

static void test_threads(void) {
    static char names[3] = {'a', 'b', 'c'};
    FuriThread* t[3];
    FuriThread* w;
    int i;

    printf("threads\n");
    order_n = 0;
    for(i = 0; i < 3; i++) {
        t[i] = furi_thread_alloc_ex("w", 1024, worker, &names[i]);
        furi_thread_start(t[i]);
    }
    for(i = 0; i < 3; i++)
        furi_thread_join(t[i]);
    order[order_n] = '\0';
    CHECK(strcmp(order, "abcabcabc") == 0, "round robin on yield");
    for(i = 0; i < 3; i++) {
        CHECK(furi_thread_get_return_code(t[i]) == names[i], "return code");
        CHECK(furi_thread_get_state(t[i]) == FuriThreadStateStopped, "stopped");
        furi_thread_free(t[i]);
    }

    w = furi_thread_alloc();
    furi_thread_set_name(w, "whoami");
    furi_thread_set_stack_size(w, 1024);
    furi_thread_set_callback(w, who_am_i);
    furi_thread_set_state_callback(w, on_state);
    state_n = 0;
    furi_thread_start(w);
    CHECK(furi_thread_get_state(w) == FuriThreadStateStarting, "starting until it runs");
    furi_thread_join(w);
    CHECK(seen_id == furi_thread_get_id(w), "current id inside the thread");
    CHECK(strcmp(furi_thread_get_name(seen_id), "whoami") == 0, "name");
    CHECK(furi_thread_get_return_code(w) == 7, "return code 7");
    CHECK(state_n == 4 && state_log[0] == FuriThreadStateStarting &&
              state_log[1] == FuriThreadStateRunning &&
              state_log[2] == FuriThreadStateStopping &&
              state_log[3] == FuriThreadStateStopped,
          "state callbacks");
    /* restart the same thread: new stack, runs again */
    seen_id = NULL;
    furi_thread_start(w);
    furi_thread_join(w);
    CHECK(seen_id == furi_thread_get_id(w), "restart after join");
    furi_thread_free(w);
    CHECK(furi_thread_get_current_id() != NULL, "main has an id");
}

/* -------------------------------------------------------- message queue */

static int32_t producer(void* ctx) {
    FuriMessageQueue* q = ctx;
    uint32_t i;

    for(i = 0; i < 20; i++)
        furi_check(furi_message_queue_put(q, &i, FuriWaitForever) == FuriStatusOk);
    return 0;
}

static void test_queue(void) {
    FuriMessageQueue* q = furi_message_queue_alloc(2, sizeof(uint32_t));
    FuriThread* p;
    uint32_t v, i, start;
    bool in_order = true;

    printf("message queue\n");
    CHECK(furi_message_queue_get_capacity(q) == 2, "capacity");
    CHECK(furi_message_queue_get_message_size(q) == 4, "message size");
    CHECK(furi_message_queue_get(q, &v, 0) == FuriStatusErrorResource, "empty, timeout 0");
    p = furi_thread_alloc_ex("producer", 1024, producer, q);
    furi_thread_start(p);
    for(i = 0; i < 20; i++) {
        if(furi_message_queue_get(q, &v, FuriWaitForever) != FuriStatusOk || v != i)
            in_order = false;
    }
    CHECK(in_order, "20 messages through a 2-slot queue, in order");
    furi_thread_join(p);
    furi_thread_free(p);

    v = 1;
    CHECK(furi_message_queue_put(q, &v, 0) == FuriStatusOk, "put");
    CHECK(furi_message_queue_put(q, &v, 0) == FuriStatusOk, "put 2");
    CHECK(furi_message_queue_put(q, &v, 0) == FuriStatusErrorResource, "full, timeout 0");
    CHECK(furi_message_queue_get_count(q) == 2 && furi_message_queue_get_space(q) == 0, "count");
    CHECK(furi_message_queue_reset(q) == FuriStatusOk && furi_message_queue_get_count(q) == 0,
          "reset");
    start = furi_get_tick();
    CHECK(furi_message_queue_get(q, &v, 30) == FuriStatusErrorTimeout, "get times out");
    CHECK(furi_get_tick() - start >= 30, "waited the timeout");
    furi_message_queue_free(q);
}

/* ---------------------------------------------------------- event flags */

static uint32_t ef_got;

static int32_t ef_waiter(void* ctx) {
    ef_got = furi_event_flag_wait(ctx, 0x3, FuriFlagWaitAll, FuriWaitForever);
    return 0;
}

static void test_event_flag(void) {
    FuriEventFlag* e = furi_event_flag_alloc();
    FuriThread* w = furi_thread_alloc_ex("efw", 1024, ef_waiter, e);

    printf("event flags\n");
    furi_thread_start(w);
    furi_thread_yield();
    furi_event_flag_set(e, 0x1);
    furi_thread_yield();
    CHECK(ef_got == 0, "waits for all");
    furi_event_flag_set(e, 0x6);
    furi_thread_join(w);
    CHECK((ef_got & 0x3) == 0x3, "woken with both");
    CHECK(furi_event_flag_get(e) == 0x4, "waited flags cleared, others kept");
    CHECK(furi_event_flag_wait(e, 0x5, FuriFlagWaitAny | FuriFlagNoClear, 0) == 0x4,
          "any + no clear");
    CHECK(furi_event_flag_get(e) == 0x4, "no clear kept it");
    CHECK(furi_event_flag_clear(e, 0x4) == 0x4 && furi_event_flag_get(e) == 0, "clear");
    CHECK(furi_event_flag_wait(e, 0x1, FuriFlagWaitAny, 0) == FuriFlagErrorResource,
          "timeout 0");
    CHECK(furi_event_flag_wait(e, 0x1, FuriFlagWaitAny, 20) == FuriFlagErrorTimeout,
          "timeout");
    furi_thread_free(w);
    furi_event_flag_free(e);
}

/* --------------------------------------------------------- thread flags */

static uint32_t tf_got;

static int32_t tf_waiter(void* ctx) {
    (void)ctx;
    tf_got = furi_thread_flags_wait(0x10 | 0x20, FuriFlagWaitAny, FuriWaitForever);
    return (int32_t)furi_thread_flags_get();
}

static void test_thread_flags(void) {
    FuriThread* w = furi_thread_alloc_ex("tfw", 1024, tf_waiter, NULL);

    printf("thread flags\n");
    furi_thread_start(w);
    furi_thread_yield();
    CHECK(furi_thread_flags_set(furi_thread_get_id(w), 0x21) == 0x21, "set returns flags");
    furi_thread_join(w);
    CHECK(tf_got == 0x21, "woken with the flags");
    CHECK(furi_thread_get_return_code(w) == 0x1, "only waited flags cleared");
    CHECK(furi_thread_flags_wait(0x1, FuriFlagWaitAny, 0) == FuriFlagErrorResource,
          "timeout 0");
    furi_thread_free(w);
}

/* ---------------------------------------------------------------- mutex */

static int in_section, overlap;

static int32_t locker(void* ctx) {
    FuriMutex* m = ctx;
    int i;

    for(i = 0; i < 5; i++) {
        furi_check(furi_mutex_acquire(m, FuriWaitForever) == FuriStatusOk);
        if(in_section) overlap = 1;
        in_section = 1;
        furi_thread_yield(); /* the other one must not get in */
        in_section = 0;
        furi_mutex_release(m);
        furi_thread_yield();
    }
    return 0;
}

static int32_t try_release(void* ctx) {
    return furi_mutex_release(ctx);
}

static void test_mutex(void) {
    FuriMutex* m = furi_mutex_alloc(FuriMutexTypeNormal);
    FuriMutex* r = furi_mutex_alloc(FuriMutexTypeRecursive);
    FuriThread *a, *b;

    printf("mutex\n");
    a = furi_thread_alloc_ex("la", 1024, locker, m);
    b = furi_thread_alloc_ex("lb", 1024, locker, m);
    furi_thread_start(a);
    furi_thread_start(b);
    furi_thread_join(a);
    furi_thread_join(b);
    CHECK(!overlap, "mutual exclusion across yields");
    furi_thread_free(b);

    CHECK(furi_mutex_acquire(r, 0) == FuriStatusOk, "recursive 1");
    CHECK(furi_mutex_acquire(r, 0) == FuriStatusOk, "recursive 2");
    CHECK(furi_mutex_get_owner(r) == furi_thread_get_current_id(), "owner");
    CHECK(furi_mutex_release(r) == FuriStatusOk && furi_mutex_get_owner(r) != NULL,
          "still held once");
    CHECK(furi_mutex_release(r) == FuriStatusOk && furi_mutex_get_owner(r) == NULL, "free");

    CHECK(furi_mutex_acquire(m, 0) == FuriStatusOk, "normal acquire");
    furi_thread_set_callback(a, try_release);
    furi_thread_set_context(a, m);
    furi_thread_start(a);
    furi_thread_join(a);
    CHECK(furi_thread_get_return_code(a) == FuriStatusErrorResource, "release by non-owner");
    furi_thread_free(a);
    furi_mutex_release(m);
    furi_mutex_free(m);
    furi_mutex_free(r);
}

/* ------------------------------------------------------------ semaphore */

static int32_t sem_taker(void* ctx) {
    return furi_semaphore_acquire(ctx, FuriWaitForever);
}

static void test_semaphore(void) {
    FuriSemaphore* s = furi_semaphore_alloc(2, 2);
    FuriThread* t;

    printf("semaphore\n");
    CHECK(furi_semaphore_acquire(s, 0) == FuriStatusOk, "take 1");
    CHECK(furi_semaphore_acquire(s, 0) == FuriStatusOk, "take 2");
    CHECK(furi_semaphore_acquire(s, 0) == FuriStatusErrorResource, "none left");
    CHECK(furi_semaphore_get_count(s) == 0, "count 0");
    t = furi_thread_alloc_ex("sem", 1024, sem_taker, s);
    furi_thread_start(t);
    furi_thread_yield();
    CHECK(furi_thread_get_state(t) == FuriThreadStateRunning, "taker blocked");
    furi_semaphore_release(s);
    furi_thread_join(t);
    CHECK(furi_thread_get_return_code(t) == FuriStatusOk, "taker woken by release");
    furi_semaphore_release(s);
    furi_semaphore_release(s);
    CHECK(furi_semaphore_release(s) == FuriStatusErrorResource, "release above max");
    furi_thread_free(t);
    furi_semaphore_free(s);
}

/* ---------------------------------------------------------------- timer */

static int ticks_seen, once_seen, pend_seen;

static void on_tick(void* ctx) {
    (void)ctx;
    ticks_seen++;
}

static void on_once(void* ctx) {
    FuriMessageQueue* q = ctx;
    uint32_t v = 42;

    once_seen++;
    furi_message_queue_put(q, &v, 0); /* timeout 0 is allowed here */
}

static void on_pending(void* ctx, uint32_t arg) {
    (void)ctx;
    pend_seen = (int)arg;
}

static void test_timer(void) {
    FuriMessageQueue* q = furi_message_queue_alloc(1, sizeof(uint32_t));
    FuriTimer* per = furi_timer_alloc(on_tick, FuriTimerTypePeriodic, NULL);
    FuriTimer* once = furi_timer_alloc(on_once, FuriTimerTypeOnce, q);
    uint32_t v = 0;

    printf("timers\n");
    CHECK(!furi_timer_is_running(per), "not running");
    furi_timer_start(per, 20);
    furi_timer_start(once, 40);
    CHECK(furi_timer_is_running(per) && furi_timer_is_running(once), "running");
    CHECK(furi_message_queue_get(q, &v, FuriWaitForever) == FuriStatusOk && v == 42,
          "a once timer wakes a queue reader");
    CHECK(once_seen == 1 && !furi_timer_is_running(once), "once: one call, then stopped");
    furi_delay_ms(110);
#ifdef FURI_HOST
    CHECK(ticks_seen >= 5 && ticks_seen <= 8, "periodic 20 ms over 150 ms");
#else
    CHECK(ticks_seen >= 1, "periodic timer runs (whole-second sleeps until R4b.6)");
#endif
    furi_timer_stop(per);
    ticks_seen = 0;
    furi_delay_ms(50);
    CHECK(ticks_seen == 0, "stopped");
    furi_timer_pending_callback(on_pending, NULL, 5);
    furi_thread_yield();
    CHECK(pend_seen == 5, "pending callback");
    furi_timer_free(per);
    furi_timer_free(once);
    furi_message_queue_free(q);
}

/* -------------------------------------------------------------- records */

static int32_t rec_user(void* ctx) {
    int* p = furi_record_open("svc");

    (void)ctx;
    furi_check(p != NULL);
    return *p;
}

static void test_record(void) {
    static int service = 99;
    FuriThread* u = furi_thread_alloc_ex("rec", 1024, rec_user, NULL);

    printf("records\n");
    CHECK(!furi_record_exists("svc"), "not there yet");
    furi_thread_start(u);
    furi_thread_yield();
    CHECK(furi_thread_get_state(u) == FuriThreadStateRunning, "open waits for create");
    furi_record_create("svc", &service);
    CHECK(furi_record_exists("svc"), "exists");
    furi_thread_join(u);
    CHECK(furi_thread_get_return_code(u) == 99, "open got the data");
    CHECK(!furi_record_destroy("svc"), "held: no destroy");
    furi_record_close("svc");
    CHECK(furi_record_destroy("svc"), "destroy");
    CHECK(!furi_record_exists("svc"), "gone");
    furi_thread_free(u);
}

/* --------------------------------------------------------------- string */

static void test_string(void) {
    FuriString* s = furi_string_alloc();
    FuriString* t = furi_string_alloc_set("hello");
    FuriString* u;
    FuriStringUTF8State st = FuriStringUTF8StateStarting;
    FuriStringUnicodeValue uv = 0;
    const char* euro = "\xE2\x82\xAC";
    int i;

    printf("furi_string\n");
    CHECK(furi_string_empty(s) && strcmp(furi_string_get_cstr(s), "") == 0, "empty");
    furi_string_set(s, t);
    furi_string_cat(s, " world");
    CHECK(furi_string_equal(s, "hello world"), "set + cat");
    CHECK(furi_string_size(s) == 11, "size");
    CHECK(furi_string_search(s, "o") == 4 && furi_string_search(s, "o", 5) == 7, "search");
    CHECK(furi_string_search(s, "x") == FURI_STRING_FAILURE, "search fails");
    CHECK(furi_string_search_char(s, 'l') == 2 && furi_string_search_rchar(s, 'l') == 9,
          "search char");
    CHECK(furi_string_start_with(s, "hell") && furi_string_end_with(s, t) == false &&
              furi_string_end_with(s, "world"),
          "start/end with");
    CHECK(furi_string_cmp(s, "hello world") == 0 && furi_string_cmpi(s, "HELLO World") == 0,
          "cmp, cmpi");
    furi_string_replace_all(s, "o", "0");
    CHECK(furi_string_equal(s, "hell0 w0rld"), "replace all");
    CHECK(furi_string_replace(s, "0", "oo") == 4 && furi_string_equal(s, "helloo w0rld"),
          "replace first");
    furi_string_printf(s, "%d-%s-%04x", -12, "ab", 0xbeef);
    CHECK(furi_string_equal(s, "-12-ab-beef"), "printf");
    furi_string_cat_printf(s, "/%lu", 4000000000UL);
    CHECK(furi_string_equal(s, "-12-ab-beef/4000000000"), "cat_printf");
    furi_string_reset(s);
    for(i = 0; i < 40; i++)
        furi_string_cat_printf(s, "%02d", i);
    CHECK(furi_string_size(s) == 80 && furi_string_get_char(s, 79) == '9', "printf grows");
    furi_string_set(s, "  \tpad me \n");
    furi_string_trim(s);
    CHECK(furi_string_equal(s, "pad me"), "trim");
    furi_string_set(s, "abcdef");
    furi_string_mid(s, 1, 3);
    CHECK(furi_string_equal(s, "bcd"), "mid");
    furi_string_right(s, 1);
    CHECK(furi_string_equal(s, "cd"), "right");
    furi_string_left(s, 1);
    CHECK(furi_string_equal(s, "c"), "left");
    furi_string_push_back(s, 'x');
    furi_string_set_char(s, 0, 'C');
    CHECK(furi_string_equal(s, "Cx"), "push_back, set_char");
    furi_string_set_strn(s, "truncated", 5);
    CHECK(furi_string_equal(s, "trunc"), "set_strn");
    furi_string_set_n(s, t, 1, 3);
    CHECK(furi_string_equal(s, "ell"), "set_n");
    furi_string_cat(s, s);
    CHECK(furi_string_equal(s, "ellell"), "cat with itself");
    furi_string_replace_at(s, 0, 3, furi_string_get_cstr(s));
    CHECK(furi_string_equal(s, "ellellell"), "replace_at from itself");
    furi_string_swap(s, t);
    CHECK(furi_string_equal(s, "hello") && furi_string_equal(t, "ellellell"), "swap");
    CHECK(furi_string_hash(s) != furi_string_hash(t), "hash");
    u = furi_string_alloc_move(t); /* t is freed */
    CHECK(furi_string_equal(u, "ellellell"), "alloc_move");
    furi_string_reset(u);
    furi_string_utf8_push(u, 0x20AC);
    CHECK(strcmp(furi_string_get_cstr(u), euro) == 0 && furi_string_utf8_length(u) == 1,
          "utf8 push/length");
    for(i = 0; euro[i]; i++)
        furi_string_utf8_decode(euro[i], &st, &uv);
    CHECK(st == FuriStringUTF8StateStarting && uv == 0x20AC, "utf8 decode");
    furi_string_free(u);
    u = furi_string_alloc_printf("%s=%d", "n", 5);
    CHECK(furi_string_equal_str(u, "n=5"), "alloc_printf");
    furi_string_free(u);
    furi_string_free(s);
}

/* --------------------------------------------------------- the crashes */

static int32_t deep(void* ctx) {
    volatile char pad[200];
    int n = *(int*)ctx;

    memset((char*)pad, 0x11, sizeof(pad)); /* the whole frame is used */
    if(n > 0) {
        n--;
        deep(&n);
    }
    furi_thread_yield();
    return pad[0];
}

static void crash_check(void) {
    int zero = 0;
    furi_check(zero == 1, "test crash");
}

static void crash_overflow(void) {
    int depth = 8; /* ~1.8 KB in a 512 B stack */
    FuriThread* t = furi_thread_alloc_ex("deep", 512, deep, &depth);
    furi_thread_start(t);
    furi_thread_join(t);
}

static void crash_deadlock(void) {
    FuriMessageQueue* q = furi_message_queue_alloc(1, 1);
    char c;
    furi_message_queue_get(q, &c, FuriWaitForever);
}

/* Run f in a child: it must end with SIGABRT (furi_crash -> abort()). */
static void expect_abort(void (*f)(void), const char* what) {
    pid_t pid;
    int st = 0;

    fflush(stdout);
    pid = fork();
    if(pid == 0) {
        f();
        _exit(0);
    }
    CHECK(pid > 0 && waitpid(pid, &st, 0) == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT,
          what);
}

static void test_crash(void) {
    printf("furi_check / furi_crash (expect three crash lines)\n");
    expect_abort(crash_check, "furi_check -> abort");
    expect_abort(crash_overflow, "thread stack overflow detected");
    expect_abort(crash_deadlock, "deadlock detected");
    FURI_LOG_I("furitest", "log line %d", 1);
}

/* ------------------------------------------------------------------ RAM */

/* sizes of the private structures (src/thread.c, sync.c, timer.c) */
size_t furi_debug_sizeof(int what);

static void report_ram(void) {
    char *a, *b;
    long over;

    /* malloc's own cost per block: two blocks in a row */
    a = malloc(16);
    b = malloc(16);
    over = (b > a ? b - a : a - b) - 16;
    free(b);
    free(a);
    if(over < 0 || over > 64) over = -1; /* not next to each other */
    printf("RAM (bytes, %u-bit): malloc adds %ld per block\n", (unsigned)(8 * sizeof(void*)),
           over);
    printf("  FuriThread      %u + stack (min %u; stack in the stack gap, not the heap)\n",
           (unsigned)furi_debug_sizeof(0), 512U);
    printf("  FuriMessageQueue %u + count*size\n", (unsigned)furi_debug_sizeof(1));
    printf("  FuriEventFlag %u, FuriMutex %u, FuriSemaphore %u, FuriTimer %u, FuriString %u\n",
           (unsigned)furi_debug_sizeof(2), (unsigned)furi_debug_sizeof(3),
           (unsigned)furi_debug_sizeof(4), (unsigned)furi_debug_sizeof(5),
           (unsigned)furi_debug_sizeof(6));
}

int main(void) {
    test_threads();
    test_queue();
    test_event_flag();
    test_thread_flags();
    test_mutex();
    test_semaphore();
    test_timer();
    test_record();
    test_string();
    test_crash();
    report_ram();
    printf("furitest: %d checks, %s\n", checks, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
