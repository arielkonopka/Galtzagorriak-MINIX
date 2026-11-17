/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri GUI (spec R5.2): from the stock firmware
 * applications/services/gui/icon_i.h (GPL-3.0). */
/**
 * @file icon_i.h
 * GUI: internal Icon API
 */

#pragma once
#include <stdint.h>

struct Icon {
    const uint16_t width;
    const uint16_t height;
    const uint8_t frame_count;
    const uint8_t frame_rate;
    const uint8_t* const* frames;
};
