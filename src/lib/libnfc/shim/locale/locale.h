/* SPDX-License-Identifier: GPL-3.0-or-later
 * libnfc shim (#404): stock's locale service, fixed to 24 h and
 * day.month.year (MINIX has no locale settings for it). */
#pragma once
#include <datetime.h>

typedef enum { LocaleTimeFormat24h = 0, LocaleTimeFormat12h = 1 } LocaleTimeFormat;
typedef enum { LocaleDateFormatDMY = 0, LocaleDateFormatMDY = 1, LocaleDateFormatYMD = 2 } LocaleDateFormat;

LocaleTimeFormat locale_get_time_format(void);
LocaleDateFormat locale_get_date_format(void);
void locale_format_time(FuriString* out, const DateTime* dt, LocaleTimeFormat format, bool show_seconds);
void locale_format_date(FuriString* out, const DateTime* dt, LocaleDateFormat format, const char* separator);
