/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri (spec R5.1, D.2): logging (stock furi/core/log.h). Lines go to
 * /dev/log as "<tick> [L][tag] text"; if /dev/log cannot be opened, to
 * stderr. Default level: Info. */
#ifndef FURI_CORE_LOG_H
#define FURI_CORE_LOG_H

#include <stdarg.h>
#include "core/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FuriLogLevelDefault = 0,
    FuriLogLevelNone = 1,
    FuriLogLevelError = 2,
    FuriLogLevelWarn = 3,
    FuriLogLevelInfo = 4,
    FuriLogLevelDebug = 5,
    FuriLogLevelTrace = 6,
} FuriLogLevel;

void furi_log_init(void);
void furi_log_print_format(FuriLogLevel level, const char* tag, const char* format, ...)
    __attribute__((format(printf, 3, 4)));
void furi_log_print_raw_format(FuriLogLevel level, const char* format, ...)
    __attribute__((format(printf, 2, 3)));
void furi_log_set_level(FuriLogLevel level);
FuriLogLevel furi_log_get_level(void);
bool furi_log_level_to_string(FuriLogLevel level, const char** str);
bool furi_log_level_from_string(const char* str, FuriLogLevel* level);

#define FURI_LOG_E(tag, format, ...) \
    furi_log_print_format(FuriLogLevelError, tag, format, ##__VA_ARGS__)
#define FURI_LOG_W(tag, format, ...) \
    furi_log_print_format(FuriLogLevelWarn, tag, format, ##__VA_ARGS__)
#define FURI_LOG_I(tag, format, ...) \
    furi_log_print_format(FuriLogLevelInfo, tag, format, ##__VA_ARGS__)
#define FURI_LOG_D(tag, format, ...) \
    furi_log_print_format(FuriLogLevelDebug, tag, format, ##__VA_ARGS__)
#define FURI_LOG_T(tag, format, ...) \
    furi_log_print_format(FuriLogLevelTrace, tag, format, ##__VA_ARGS__)
#define FURI_LOG_RAW_E(format, ...) \
    furi_log_print_raw_format(FuriLogLevelError, format, ##__VA_ARGS__)
#define FURI_LOG_RAW_W(format, ...) \
    furi_log_print_raw_format(FuriLogLevelWarn, format, ##__VA_ARGS__)
#define FURI_LOG_RAW_I(format, ...) \
    furi_log_print_raw_format(FuriLogLevelInfo, format, ##__VA_ARGS__)
#define FURI_LOG_RAW_D(format, ...) \
    furi_log_print_raw_format(FuriLogLevelDebug, format, ##__VA_ARGS__)
#define FURI_LOG_RAW_T(format, ...) \
    furi_log_print_raw_format(FuriLogLevelTrace, format, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif
