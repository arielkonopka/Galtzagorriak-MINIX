/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri (spec R5.2): the u8x8 bus/GPIO layer and kerning tables are
 * not needed (no display behind u8g2, the stock fonts have no kerning
 * table); these stand in so libfuri links without --gc-sections too. */
#include "u8g2.h"

uint8_t u8x8_cad_empty(u8x8_t* u8x8, uint8_t msg, uint8_t arg_int, void* arg_ptr) {
    (void)u8x8;
    (void)msg;
    (void)arg_int;
    (void)arg_ptr;
    return 1;
}

void u8x8_cad_vsendf(u8x8_t* u8x8, const char* fmt, va_list va) {
    (void)u8x8;
    (void)fmt;
    (void)va;
}

void u8x8_gpio_call(u8x8_t* u8x8, uint8_t msg, uint8_t arg) {
    (void)u8x8;
    (void)msg;
    (void)arg;
}

uint8_t u8g2_GetKerning(u8g2_t* u8g2, u8g2_kerning_t* kerning, uint16_t e1, uint16_t e2) {
    (void)u8g2;
    (void)kerning;
    (void)e1;
    (void)e2;
    return 0;
}

uint8_t u8g2_GetKerningByTable(u8g2_t* u8g2, const uint16_t* kt, uint16_t e1, uint16_t e2) {
    (void)u8g2;
    (void)kt;
    (void)e1;
    (void)e2;
    return 0;
}
