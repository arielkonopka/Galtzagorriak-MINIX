/* SPDX-License-Identifier: GPL-3.0-or-later
 * liblfrfid furi shim: furi_check, furi_assert, furi_crash (stock furi
 * core/check.h).  A failed check ends the process with the message on
 * stderr (the stock firmware reboots the Flipper). */
#ifndef LFRFID_SHIM_CHECK_H
#define LFRFID_SHIM_CHECK_H

void lfrfid_crash(const char* message, const char* file, int line)
    __attribute__((noreturn));

#define __LFRFID_MSG(...) __LFRFID_MSG_(__VA_ARGS__ + 0, 0)
#define __LFRFID_MSG_(m, ...) ((const char*)(m))
#define furi_crash(...) lfrfid_crash(__LFRFID_MSG(__VA_ARGS__), __FILE__, __LINE__)

#define __lfrfid_check_1(e) ((e) ? (void)0 : lfrfid_crash("check failed: " #e, __FILE__, __LINE__))
#define __lfrfid_check_2(e, m) ((e) ? (void)0 : lfrfid_crash((m), __FILE__, __LINE__))
#define __LFRFID_SEL3(_1, _2, NAME, ...) NAME
#define furi_check(...) __LFRFID_SEL3(__VA_ARGS__, __lfrfid_check_2, __lfrfid_check_1, 0)(__VA_ARGS__)
#define furi_assert(...) furi_check(__VA_ARGS__)

#endif
