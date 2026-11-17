/* SPDX-License-Identifier: GPL-3.0-or-later
 * liblfrfid furi shim: FDX-B asks which units to print a temperature in. */
#ifndef LFRFID_SHIM_FURI_HAL_RTC_H
#define LFRFID_SHIM_FURI_HAL_RTC_H
typedef enum {
    FuriHalRtcLocaleUnitsMetric = 0,
    FuriHalRtcLocaleUnitsImperial = 1,
} FuriHalRtcLocaleUnits;
FuriHalRtcLocaleUnits furi_hal_rtc_get_locale_units(void);
/* FDX-B opens the storage record for the country names (iso_3166.c). */
void* furi_record_open(const char* name);
void furi_record_close(const char* name);
#endif
