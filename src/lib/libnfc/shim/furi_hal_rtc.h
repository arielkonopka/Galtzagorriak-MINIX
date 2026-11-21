/* SPDX-License-Identifier: GPL-3.0-or-later
 * libnfc shim (#404): the RTC calls the parsers make (the time now, to
 * show how old a pass is; the debug flag, never set here). */
#pragma once
#include <datetime.h>
typedef enum { FuriHalRtcFlagDebug = (1 << 0) } FuriHalRtcFlag;
uint32_t furi_hal_rtc_get_timestamp(void);
bool furi_hal_rtc_is_flag_set(FuriHalRtcFlag flag);
