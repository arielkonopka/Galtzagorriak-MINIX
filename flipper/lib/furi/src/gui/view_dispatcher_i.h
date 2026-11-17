/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri GUI (spec R5.2): from the stock firmware
 * applications/services/gui/view_dispatcher_i.h (GPL-3.0); see
 * view_dispatcher.c. */
#pragma once

#include "view_dispatcher.h"
#include "view_i.h"
#include "gui_i.h"

typedef struct {
    uint32_t id;
    View* view;
} ViewEntry;

ARRAY_DEF(ViewArray, ViewEntry, M_POD_OPLIST);

typedef enum {
    ViewDispatcherMessageInput,
    ViewDispatcherMessageCustom,
    ViewDispatcherMessageStop,
} ViewDispatcherMessageKind;

typedef struct {
    uint8_t kind;
    uint8_t key; /* input */
    uint8_t type; /* input */
    uint32_t value; /* input: sequence; custom: the event */
} ViewDispatcherMessage;

struct ViewDispatcher {
    FuriMessageQueue* queue;
    Gui* gui;
    ViewPort* view_port;
    ViewArray_t views;

    View* current_view;

    View* ongoing_input_view;
    uint8_t ongoing_input;
    bool running;

    ViewDispatcherCustomEventCallback custom_event_callback;
    ViewDispatcherNavigationEventCallback navigation_event_callback;
    ViewDispatcherTickEventCallback tick_event_callback;
    uint32_t tick_period;
    void* event_context;
};

void view_dispatcher_draw_callback(Canvas* canvas, void* context);
void view_dispatcher_input_callback(InputEvent* event, void* context);
void view_dispatcher_handle_input(ViewDispatcher* view_dispatcher, InputEvent* event);
void view_dispatcher_handle_tick_event(void* context);
void view_dispatcher_handle_custom_event(ViewDispatcher* view_dispatcher, uint32_t event);
void view_dispatcher_set_current_view(ViewDispatcher* view_dispatcher, View* view);
void view_dispatcher_update(View* view, void* context);
