/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri (spec R5.1, D.1): furi_check, furi_assert, furi_crash.
 * The stock firmware reboots the device on a failed check; here only the
 * process ends: the message goes to /dev/log (and stderr), then abort(),
 * and the launcher shows "app crashed" (R5.6).
 * Both the old one-argument and the newer (expr, message) forms work. */
#ifndef FURI_CORE_CHECK_H
#define FURI_CORE_CHECK_H

#ifdef __cplusplus
extern "C" {
#endif

void __furi_crash_at(const char* message, const char* file, int line)
    __attribute__((noreturn));
void __furi_halt_at(const char* message, const char* file, int line)
    __attribute__((noreturn));

#define __FURI_ASSERT_MESSAGE_FLAG ((const char*)0)
#define __FURI_CHECK_MESSAGE_FLAG ((const char*)0)

#define furi_crash(...) __furi_crash_at(__FURI_MSG(__VA_ARGS__), __FILE__, __LINE__)
#define furi_halt(...) __furi_halt_at(__FURI_MSG(__VA_ARGS__), __FILE__, __LINE__)

/* __FURI_MSG(): no argument -> NULL, one argument -> it. */
#define __FURI_MSG(...) __FURI_MSG_(__VA_ARGS__ + 0, 0)
#define __FURI_MSG_(m, ...) ((const char*)(m))

#define __furi_check_1(e) \
    ((e) ? (void)0 : __furi_crash_at("furi_check failed: " #e, __FILE__, __LINE__))
#define __furi_check_2(e, m) \
    ((e) ? (void)0 : __furi_crash_at((m), __FILE__, __LINE__))
#define __FURI_SEL3(_1, _2, NAME, ...) NAME

#define furi_check(...) \
    __FURI_SEL3(__VA_ARGS__, __furi_check_2, __furi_check_1, 0)(__VA_ARGS__)

#ifdef FURI_NDEBUG
#define furi_assert(...) ((void)0)
#else
#define __furi_assert_1(e) \
    ((e) ? (void)0 : __furi_crash_at("furi_assert failed: " #e, __FILE__, __LINE__))
#define __furi_assert_2(e, m) \
    ((e) ? (void)0 : __furi_crash_at((m), __FILE__, __LINE__))
#define furi_assert(...) \
    __FURI_SEL3(__VA_ARGS__, __furi_assert_2, __furi_assert_1, 0)(__VA_ARGS__)
#endif

#ifdef __cplusplus
}
#endif

#endif
