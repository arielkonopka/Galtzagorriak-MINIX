/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri GUI (spec R5.2): ViewPort, from the stock firmware
 * applications/services/gui/view_port.c (GPL-3.0).
 * Changes: no mutex (cooperative threads: a ViewPort call never runs
 * in the middle of another); no left-handed mode (no RTC flag). */
#include "view_port_i.h"

#include <furi.h>

#include "gui.h"
#include "gui_i.h"

_Static_assert(ViewPortOrientationMAX == 4, "Incorrect ViewPortOrientation count");
_Static_assert(InputKeyMAX == 6, "Incorrect InputKey count");

/* InputKey directional keys for the screen orientations */
static const uint8_t view_port_input_mapping[ViewPortOrientationMAX][InputKeyMAX] = {
    {InputKeyUp, InputKeyDown, InputKeyRight, InputKeyLeft, InputKeyOk, InputKeyBack},
    {InputKeyDown, InputKeyUp, InputKeyLeft, InputKeyRight, InputKeyOk, InputKeyBack},
    {InputKeyRight, InputKeyLeft, InputKeyDown, InputKeyUp, InputKeyOk, InputKeyBack},
    {InputKeyLeft, InputKeyRight, InputKeyUp, InputKeyDown, InputKeyOk, InputKeyBack},
};

static const uint8_t view_port_orientation_mapping[ViewPortOrientationMAX] = {
    [ViewPortOrientationHorizontal] = CanvasOrientationHorizontal,
    [ViewPortOrientationHorizontalFlip] = CanvasOrientationHorizontalFlip,
    [ViewPortOrientationVertical] = CanvasOrientationVertical,
    [ViewPortOrientationVerticalFlip] = CanvasOrientationVerticalFlip,
};

ViewPort* view_port_alloc(void) {
    ViewPort* view_port = calloc(1, sizeof(ViewPort));
    furi_check(view_port, "out of memory");
    view_port->orientation = ViewPortOrientationHorizontal;
    view_port->is_enabled = true;
    return view_port;
}

void view_port_free(ViewPort* view_port) {
    furi_check(view_port);
    furi_check(view_port->gui == NULL);
    free(view_port);
}

void view_port_set_width(ViewPort* view_port, uint8_t width) {
    furi_check(view_port);
    view_port->width = width;
}

uint8_t view_port_get_width(const ViewPort* view_port) {
    furi_check(view_port);
    return view_port->width;
}

void view_port_set_height(ViewPort* view_port, uint8_t height) {
    furi_check(view_port);
    view_port->height = height;
}

uint8_t view_port_get_height(const ViewPort* view_port) {
    furi_check(view_port);
    return view_port->height;
}

void view_port_enabled_set(ViewPort* view_port, bool enabled) {
    furi_check(view_port);
    if(view_port->is_enabled != enabled) {
        view_port->is_enabled = enabled;
        if(view_port->gui) gui_update(view_port->gui);
    }
}

bool view_port_is_enabled(const ViewPort* view_port) {
    furi_check(view_port);
    return view_port->is_enabled;
}

void view_port_draw_callback_set(ViewPort* view_port, ViewPortDrawCallback callback, void* context) {
    furi_check(view_port);
    view_port->draw_callback = callback;
    view_port->draw_callback_context = context;
}

void view_port_input_callback_set(
    ViewPort* view_port,
    ViewPortInputCallback callback,
    void* context) {
    furi_check(view_port);
    view_port->input_callback = callback;
    view_port->input_callback_context = context;
}

void view_port_update(ViewPort* view_port) {
    furi_check(view_port);
    if(view_port->gui && view_port->is_enabled) gui_update(view_port->gui);
}

void view_port_gui_set(ViewPort* view_port, Gui* gui) {
    furi_check(view_port);
    view_port->gui = gui;
}

void view_port_draw(ViewPort* view_port, Canvas* canvas) {
    furi_check(view_port);
    furi_check(canvas);
    furi_check(view_port->gui);
    if(view_port->draw_callback) {
        canvas_set_orientation(
            canvas, (CanvasOrientation)view_port_orientation_mapping[view_port->orientation]);
        view_port->draw_callback(canvas, view_port->draw_callback_context);
    }
}

void view_port_input(ViewPort* view_port, InputEvent* event) {
    furi_check(view_port);
    furi_check(event);
    furi_check(view_port->gui);
    if(view_port->input_callback) {
        furi_check(event->key < InputKeyMAX);
        if(event->sequence_source == INPUT_SEQUENCE_SOURCE_HARDWARE)
            event->key = (InputKey)view_port_input_mapping[view_port->orientation][event->key];
        view_port->input_callback(event, view_port->input_callback_context);
    }
}

void view_port_set_orientation(ViewPort* view_port, ViewPortOrientation orientation) {
    furi_check(view_port);
    furi_check(orientation < ViewPortOrientationMAX);
    view_port->orientation = orientation;
}

ViewPortOrientation view_port_get_orientation(const ViewPort* view_port) {
    furi_check(view_port);
    return view_port->orientation;
}
