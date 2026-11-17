/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri GUI (spec R5.2): the Gui inside the app's own process, after
 * the stock firmware applications/services/gui/gui_i.h (GPL-3.0). */
#pragma once

#include "gui.h"

#include <furi.h>
#include "m-array.h"

#include "canvas.h"
#include "canvas_i.h"
#include "view_port.h"
#include "view_port_i.h"

#define GUI_DISPLAY_WIDTH  128
#define GUI_DISPLAY_HEIGHT 64

/* The stock status bar area: here the black title bar (flipper/gui). */
#define GUI_STATUS_BAR_X      0
#define GUI_STATUS_BAR_Y      0
#define GUI_STATUS_BAR_WIDTH  GUI_DISPLAY_WIDTH
#define GUI_STATUS_BAR_HEIGHT 13
#define GUI_TITLE_BAR_HEIGHT  11

#define GUI_WINDOW_X      0
#define GUI_WINDOW_Y      GUI_STATUS_BAR_HEIGHT
#define GUI_WINDOW_WIDTH  GUI_DISPLAY_WIDTH
#define GUI_WINDOW_HEIGHT (GUI_DISPLAY_HEIGHT - GUI_WINDOW_Y)

/* Input timing as stock (applications/services/input/input.c): Long
 * after 2 x 150 ms held, then Repeat every 150 ms. */
#define GUI_INPUT_PRESS_MS    150U
#define GUI_INPUT_LONG_COUNTS 2U

#define GUI_THREAD_STACK 1536U

ARRAY_DEF(ViewPortArray, ViewPort*, M_PTR_OPLIST);

typedef struct {
    uint32_t deadline; /* next 150 ms step while held */
    uint32_t counter; /* sequence */
    uint8_t press_counter;
    uint8_t down;
} GuiKey;

struct Gui {
    FuriThread* thread;
    bool lockdown;
    bool direct_draw;
    bool dirty; /* redraw due */
    ViewPortArray_t layers[GuiLayerMAX];
    Canvas* canvas;

    int lcd_fd;
    int buttons_fd;
    uint32_t sequence;
    GuiKey keys[InputKeyMAX];
    uint8_t ongoing_input;
    ViewPort* ongoing_input_view_port;
    const char* title; /* the title bar (Window, Desktop layers) */
};

/* Redraw soon (from any thread): wakes the gui thread. */
void gui_update(Gui* gui);
/* Feed one /dev/buttons event (key bit, pressed) or the passing of time
 * (gui.c; the host tests call these directly). */
void gui_buttons_event(Gui* gui, uint8_t bit, uint8_t pressed, uint32_t now);
/* ms to the next Long/Repeat step, FuriWaitForever if no key is held;
 * sends the steps that are due. */
uint32_t gui_input_tick(Gui* gui, uint32_t now);
/* Draw the layers into the canvas and write it to /dev/lcd now. */
void gui_redraw(Gui* gui);
Gui* gui_alloc_headless(void);
