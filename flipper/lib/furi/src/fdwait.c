/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri (issue #181, for R5.2/R5.4): a thread waits for a file
 * descriptor without stopping the other threads of the app.
 *
 * A thread that would block in read() calls ffd_read() (or ffd_wait()
 * and reads itself). Its wait record (FuriFdWait) lives on its own
 * stack while it waits: nothing is malloc'ed, and the static cost is the
 * list head and a counter. When no thread is ready, the scheduler calls
 * ffd_idle(): one poll() over every waiting fd, with the earliest timer
 * deadline as its timeout; the threads whose fd is ready wake up. While
 * other threads keep running, ffd_peek() (from the scheduler, every
 * FFD_PEEK switches) looks at the fds with timeout 0.
 *
 * "Ready" is a hint. Until the drivers answer DEV_SELECT (the local lane,
 * #131), poll() reports a character device (/dev/buttons, the tty) as
 * always ready. So ffd_read() reads with O_NONBLOCK; if poll() said ready
 * and the read still gives EAGAIN, the fd is left out of poll() for the
 * rest of that call and tried again after a clock tick (FPORT_TICK_MS;
 * on MINIX poll() waits at least two tick boundaries, so every 17-33 ms):
 * a device costs one read() per tick or two while it has no data, never
 * a spin (QEMU: 3 reads in 100 ms; the host, 10 ms ticks: 9).
 * Drop the retry path (FuriFdWait.retry) once flipdev answers DEV_SELECT:
 * then a ready device is really ready and the path is never taken. */
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "furi_i.h"

struct FuriFdWait {
    FuriFdWait* next;
    FuriWaitq wq; /* the waiting thread */
    uint32_t retry_at; /* retry: wake at this time */
    int16_t fd;
    int16_t events;
    int16_t revents; /* set when woken */
    uint8_t retry; /* not in poll(); woken after one tick */
};

static FuriFdWait* waits;
static uint8_t nwaits;
uint32_t ffd_reads; /* read() calls, for the tests' spin check */

bool ffd_waiting(void) {
    return waits != NULL;
}

int ffd_wait(int fd, int events, uint32_t timeout, bool retry) {
    FuriFdWait w, **pp;

    if(timeout == 0) return 0;
    furi_check(nwaits < FFD_MAX, "ffd_wait: too many fd waits");
    w.wq.head = w.wq.tail = NULL;
    w.fd = (int16_t)fd;
    w.events = (int16_t)events;
    w.revents = 0;
    w.retry = retry;
    w.retry_at = retry ? fport_now() + FPORT_TICK_MS : 0;
    /* at the tail: poll() order = wait order */
    for(pp = &waits; *pp; pp = &(*pp)->next)
        ;
    w.next = NULL;
    *pp = &w;
    nwaits++;
    fsched_block(&w.wq, timeout);
    for(pp = &waits; *pp != &w; pp = &(*pp)->next)
        ;
    *pp = w.next;
    nwaits--;
    return w.revents;
}

static void wake(FuriFdWait* w, int revents) {
    w->revents = (int16_t)revents;
    fsched_wake_all(&w->wq);
}

void ffd_idle(uint32_t ms) {
    struct pollfd pfd[FFD_MAX];
    FuriFdWait* w;
    uint32_t now = 0;
    int32_t d;
    unsigned n = 0, i = 0;
    int r;

    for(w = waits; w; w = w->next) {
        if(w->revents) continue; /* woken, not run yet */
        if(w->retry) {
            if(!now) now = fport_now();
            d = (int32_t)(w->retry_at - now);
            if(d < 0) d = 0;
            if((uint32_t)d < ms) ms = (uint32_t)d;
            continue;
        }
        pfd[n].fd = w->fd;
        pfd[n].events = w->events;
        pfd[n].revents = 0;
        n++;
    }
    r = (n || ms) ? fport_poll(pfd, n, ms) : 0;
    if(!waits) return;
    now = fport_now();
    for(w = waits; w; w = w->next) {
        if(w->revents) continue;
        if(w->retry) {
            if((int32_t)(w->retry_at - now) <= 0) wake(w, w->events);
            continue;
        }
        if(r > 0 && pfd[i].revents) wake(w, pfd[i].revents);
        i++;
    }
}

void ffd_kick(int fd) {
    FuriFdWait* w;

    for(w = waits; w; w = w->next)
        if(w->fd == fd && !w->revents) wake(w, FFD_KICK);
}

void ffd_peek(void) {
    static uint8_t count;

    if(waits && ++count >= FFD_PEEK) {
        count = 0;
        ffd_idle(0);
    }
}

/* read() with O_NONBLOCK for this call only: the flag belongs to the open
 * file, which the shell may share (stdin). */
static ssize_t read_nb(int fd, void* buf, size_t n) {
    int fl = fcntl(fd, F_GETFL), e;
    ssize_t r;

    if(fl < 0) return -1;
    ffd_reads++;
    if(fl & O_NONBLOCK) return read(fd, buf, n);
    if(fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
    r = read(fd, buf, n);
    e = errno;
    fcntl(fd, F_SETFL, fl);
    errno = e;
    return r;
}

ssize_t ffd_read(int fd, void* buf, size_t n, uint32_t timeout) {
    uint32_t start = fport_now(), left;
    bool retry = false, hinted = false;
    ssize_t r;
    int ev;

    for(;;) {
        r = read_nb(fd, buf, n);
        if(r >= 0 || errno != EAGAIN) return r;
        /* poll() said ready, read() found nothing: tick from now on */
        if(hinted) retry = true;
        left = fsched_left(timeout, start);
        if(left == 0) {
            errno = EAGAIN;
            return -1;
        }
        ev = ffd_wait(fd, POLLIN, left, retry);
        hinted = ev != 0;
    }
}

size_t fdebug_sizeof_fdwait(void) {
    return sizeof(FuriFdWait);
}
