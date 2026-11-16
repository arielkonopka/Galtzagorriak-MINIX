/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri (spec R5.1, D.1, D.2): furi_crash/furi_check and the log.
 * The stock firmware halts or reboots the Flipper; here the message goes
 * to /dev/log and stderr, then abort() ends this process only. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "furi_i.h"

static int log_fd = -2; /* -2: not opened yet */
static FuriLogLevel log_level = FuriLogLevelInfo;

void furi_log_init(void) {
    if(log_fd == -2) log_fd = open("/dev/log", O_WRONLY);
}

void flog_write(const char* s, size_t n) {
    furi_log_init();
    if(write(log_fd >= 0 ? log_fd : 2, s, n) < 0) {
        /* nowhere else to say it */
    }
}

/* Bounded concatenation into buf without stdio: a crash may come from a
 * thread with little stack left. */
static size_t put(char* buf, size_t at, size_t size, const char* s) {
    while(s && *s && at + 1 < size)
        buf[at++] = *s++;
    buf[at] = '\0';
    return at;
}

static size_t put_uint(char* buf, size_t at, size_t size, unsigned long v) {
    char d[12];
    int i = (int)sizeof(d) - 1;

    d[i] = '\0';
    do {
        d[--i] = (char)('0' + v % 10);
        v /= 10;
    } while(v && i > 0);
    return put(buf, at, size, d + i);
}

static void die(const char* what, const char* message, const char* file, int line)
    __attribute__((noreturn));
static void die(const char* what, const char* message, const char* file, int line) {
    static char buf[160];
    size_t n = 0;
    const char* tname = furi_thread_get_name(furi_thread_get_current_id());

    n = put(buf, n, sizeof(buf), what);
    n = put(buf, n, sizeof(buf), ": ");
    n = put(buf, n, sizeof(buf), message ? message : "fatal error");
    if(file) {
        const char* base = strrchr(file, '/');
        n = put(buf, n, sizeof(buf), " (");
        n = put(buf, n, sizeof(buf), base ? base + 1 : file);
        n = put(buf, n, sizeof(buf), ":");
        n = put_uint(buf, n, sizeof(buf), (unsigned long)line);
        n = put(buf, n, sizeof(buf), ")");
    }
    n = put(buf, n, sizeof(buf), " thread ");
    n = put(buf, n, sizeof(buf), tname ? tname : "?");
    n = put(buf, n, sizeof(buf), "\n");
    flog_write(buf, n);
    if(log_fd >= 0 && write(2, buf, n) < 0) {
        /* the log has it */
    }
    abort();
}

void __furi_crash_at(const char* message, const char* file, int line) {
    die("furi_crash", message, file, line);
}

void __furi_halt_at(const char* message, const char* file, int line) {
    die("furi_halt", message, file, line);
}

/* ------------------------------------------------------------------ log */

static const char level_char[] = "?NEWIDT";
static const char* const level_name[] = {
    "default", "none", "error", "warn", "info", "debug", "trace"};

static void log_v(FuriLogLevel level, const char* tag, const char* format, va_list ap) {
    static char buf[128];
    size_t n = 0;
    int r;

    if(level == FuriLogLevelDefault) level = FuriLogLevelInfo;
    if(level > log_level || level <= FuriLogLevelNone) return;
    if(tag) {
        n = put_uint(buf, n, sizeof(buf), (unsigned long)furi_get_tick());
        n = put(buf, n, sizeof(buf), " [");
        buf[n++] = level_char[level];
        n = put(buf, n, sizeof(buf), "][");
        n = put(buf, n, sizeof(buf), tag);
        n = put(buf, n, sizeof(buf), "] ");
    }
    r = fport_vformat(buf + n, sizeof(buf) - 1 - n, format, ap);
    if(r < 0) {
        n = sizeof(buf) - 5;
        memcpy(buf + n, "...", 3); /* cut */
        n += 3;
    } else {
        n += (size_t)r;
    }
    if(tag) buf[n++] = '\n';
    flog_write(buf, n);
}

void furi_log_print_format(FuriLogLevel level, const char* tag, const char* format, ...) {
    va_list ap;

    va_start(ap, format);
    log_v(level, tag ? tag : "", format, ap);
    va_end(ap);
}

void furi_log_print_raw_format(FuriLogLevel level, const char* format, ...) {
    va_list ap;

    va_start(ap, format);
    log_v(level, NULL, format, ap);
    va_end(ap);
}

void furi_log_set_level(FuriLogLevel level) {
    if(level == FuriLogLevelDefault) level = FuriLogLevelInfo;
    if(level <= FuriLogLevelTrace) log_level = level;
}

FuriLogLevel furi_log_get_level(void) {
    return log_level;
}

bool furi_log_level_to_string(FuriLogLevel level, const char** str) {
    if((unsigned)level > FuriLogLevelTrace) return false;
    *str = level_name[level];
    return true;
}

bool furi_log_level_from_string(const char* str, FuriLogLevel* level) {
    unsigned i;

    for(i = 0; i <= FuriLogLevelTrace; i++) {
        if(strcmp(str, level_name[i]) == 0) {
            *level = (FuriLogLevel)i;
            return true;
        }
    }
    return false;
}
