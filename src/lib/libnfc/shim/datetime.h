/* SPDX-License-Identifier: GPL-3.0-or-later
 * libnfc shim (#404): stock's lib/datetime API for the supported_cards
 * parsers that print dates.  Implemented in nfc_shim_time.c (UTC, no
 * time zone: the parsers add the card's own offsets). */
#pragma once
#include <furi.h>

typedef struct {
    uint8_t hour;    /* 0-23 */
    uint8_t minute;  /* 0-59 */
    uint8_t second;  /* 0-59 */
    uint8_t day;     /* 1-31 */
    uint8_t month;   /* 1-12 */
    uint16_t year;
    uint8_t weekday; /* 1-7, Monday first */
} DateTime;

bool datetime_validate_datetime(DateTime* datetime);
uint32_t datetime_datetime_to_timestamp(const DateTime* datetime);
void datetime_timestamp_to_datetime(uint32_t timestamp, DateTime* datetime);
uint16_t datetime_get_days_per_year(uint16_t year);
bool datetime_is_leap_year(uint16_t year);
uint8_t datetime_get_days_per_month(bool leap_year, uint8_t month);
