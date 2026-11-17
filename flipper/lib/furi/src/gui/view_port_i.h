/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri GUI (spec R5.2): from the stock firmware
 * applications/services/gui/view_port_i.h (GPL-3.0); no mutex (the
 * threads are cooperative). */
#pragma once

#include "gui_i.h"
#include "view_port.h"

struct ViewPort {
    Gui* gui;
    bool is_enabled;
    ViewPortOrientation orientation;
    uint8_t width;
    uint8_t height;
    ViewPortDrawCallback draw_callback;
    void* draw_callback_context;
    ViewPortInputCallback input_callback;
    void* input_callback_context;
};

void view_port_gui_set(ViewPort* view_port, Gui* gui);
void view_port_draw(ViewPort* view_port, Canvas* canvas);
void view_port_input(ViewPort* view_port, InputEvent* event);
