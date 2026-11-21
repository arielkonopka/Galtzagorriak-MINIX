/* nfc_shim_time.c - datetime, locale and RTC for the parsers (#404).
 * The stock API (lib/datetime, services/locale), written again: Unix time,
 * UTC, proleptic Gregorian calendar.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <time.h>

#include <datetime.h>
#include <locale/locale.h>
#include <furi_hal_rtc.h>

#define SECS_PER_DAY 86400u

bool datetime_is_leap_year(uint16_t year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

uint16_t datetime_get_days_per_year(uint16_t year) { return datetime_is_leap_year(year) ? 366 : 365; }

uint8_t datetime_get_days_per_month(bool leap_year, uint8_t month) {
    static const uint8_t d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if(month < 1 || month > 12) return 0;
    return (uint8_t)(d[month - 1] + (month == 2 && leap_year));
}

bool datetime_validate_datetime(DateTime* dt) {
    return dt->second < 60 && dt->minute < 60 && dt->hour < 24 && dt->year >= 1970 &&
           dt->month >= 1 && dt->month <= 12 && dt->day >= 1 &&
           dt->day <= datetime_get_days_per_month(datetime_is_leap_year(dt->year), dt->month) &&
           dt->weekday >= 1 && dt->weekday <= 7;
}

uint32_t datetime_datetime_to_timestamp(const DateTime* dt) {
    uint32_t days = 0;
    for(uint16_t y = 1970; y < dt->year; y++) days += datetime_get_days_per_year(y);
    bool leap = datetime_is_leap_year(dt->year);
    for(uint8_t m = 1; m < dt->month && m <= 12; m++) days += datetime_get_days_per_month(leap, m);
    if(dt->day) days += dt->day - 1u;
    return days * SECS_PER_DAY + dt->hour * 3600u + dt->minute * 60u + dt->second;
}

void datetime_timestamp_to_datetime(uint32_t ts, DateTime* dt) {
    uint32_t days = ts / SECS_PER_DAY, secs = ts % SECS_PER_DAY;
    dt->weekday = (uint8_t)((days + 3) % 7 + 1); /* 1970-01-01 was a Thursday */
    dt->hour = (uint8_t)(secs / 3600);
    dt->minute = (uint8_t)(secs / 60 % 60);
    dt->second = (uint8_t)(secs % 60);
    uint16_t y = 1970;
    while(days >= datetime_get_days_per_year(y)) days -= datetime_get_days_per_year(y++);
    dt->year = y;
    bool leap = datetime_is_leap_year(y);
    uint8_t m = 1;
    while(days >= datetime_get_days_per_month(leap, m)) days -= datetime_get_days_per_month(leap, m++);
    dt->month = m;
    dt->day = (uint8_t)(days + 1);
}

LocaleTimeFormat locale_get_time_format(void) { return LocaleTimeFormat24h; }
LocaleDateFormat locale_get_date_format(void) { return LocaleDateFormatDMY; }

void locale_format_time(FuriString* out, const DateTime* dt, LocaleTimeFormat format, bool show_seconds) {
    unsigned h = dt->hour;
    const char* ampm = "";
    if(format == LocaleTimeFormat12h) {
        ampm = h >= 12 ? " PM" : " AM";
        h = h % 12 ? h % 12 : 12;
    }
    if(show_seconds)
        furi_string_printf(out, "%02u:%02u:%02u%s", h, dt->minute, dt->second, ampm);
    else
        furi_string_printf(out, "%02u:%02u%s", h, dt->minute, ampm);
}

void locale_format_date(FuriString* out, const DateTime* dt, LocaleDateFormat format, const char* sep) {
    if(format == LocaleDateFormatMDY)
        furi_string_printf(out, "%02u%s%02u%s%04u", dt->month, sep, dt->day, sep, dt->year);
    else if(format == LocaleDateFormatYMD)
        furi_string_printf(out, "%04u%s%02u%s%02u", dt->year, sep, dt->month, sep, dt->day);
    else
        furi_string_printf(out, "%02u%s%02u%s%04u", dt->day, sep, dt->month, sep, dt->year);
}

uint32_t furi_hal_rtc_get_timestamp(void) { return (uint32_t)time(NULL); }
bool furi_hal_rtc_is_flag_set(FuriHalRtcFlag flag) { (void)flag; return false; }
