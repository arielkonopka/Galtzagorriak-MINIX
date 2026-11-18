/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri GUI (spec R5.2): ViewDispatcher, from the stock firmware
 * applications/services/gui/view_dispatcher.c (GPL-3.0).
 * Changes: no FuriEventLoop. view_dispatcher_run() waits on one message
 * queue that carries both input and custom events (8 bytes a message),
 * with the tick period as its timeout; the views are a small array of
 * (id, view) pairs instead of an M*LIB dictionary; no gui lock
 * (cooperative threads). */
#include "view_dispatcher_i.h"

#define TAG "ViewDispatcher"

#define VIEW_DISPATCHER_QUEUE_LEN (16U) /* as stock, each */

ViewDispatcher* view_dispatcher_alloc(void) {
    ViewDispatcher* view_dispatcher = calloc(1, sizeof(ViewDispatcher));
    furi_check(view_dispatcher, "out of memory");

    view_dispatcher->view_port = view_port_alloc();
    view_port_draw_callback_set(
        view_dispatcher->view_port, view_dispatcher_draw_callback, view_dispatcher);
    view_port_input_callback_set(
        view_dispatcher->view_port, view_dispatcher_input_callback, view_dispatcher);
    view_port_enabled_set(view_dispatcher->view_port, false);

    ViewArray_init(view_dispatcher->views);

    view_dispatcher->queue =
        furi_message_queue_alloc(VIEW_DISPATCHER_QUEUE_LEN, sizeof(ViewDispatcherMessage));

    return view_dispatcher;
}

void view_dispatcher_free(ViewDispatcher* view_dispatcher) {
    // Detach from gui
    if(view_dispatcher->gui) {
        gui_remove_view_port(view_dispatcher->gui, view_dispatcher->view_port);
    }
    // Crash if not all views were freed
    furi_check(!ViewArray_size(view_dispatcher->views));

    ViewArray_clear(view_dispatcher->views);
    view_port_free(view_dispatcher->view_port);
    furi_message_queue_free(view_dispatcher->queue);
    free(view_dispatcher);
}

void view_dispatcher_enable_queue(ViewDispatcher* view_dispatcher) {
    UNUSED(view_dispatcher);
}

void view_dispatcher_set_navigation_event_callback(
    ViewDispatcher* view_dispatcher,
    ViewDispatcherNavigationEventCallback callback) {
    furi_check(view_dispatcher);
    view_dispatcher->navigation_event_callback = callback;
}

void view_dispatcher_set_custom_event_callback(
    ViewDispatcher* view_dispatcher,
    ViewDispatcherCustomEventCallback callback) {
    furi_check(view_dispatcher);
    view_dispatcher->custom_event_callback = callback;
}

void view_dispatcher_set_tick_event_callback(
    ViewDispatcher* view_dispatcher,
    ViewDispatcherTickEventCallback callback,
    uint32_t tick_period) {
    furi_check(view_dispatcher);
    view_dispatcher->tick_event_callback = callback;
    view_dispatcher->tick_period = tick_period;
}

void view_dispatcher_set_event_callback_context(ViewDispatcher* view_dispatcher, void* context) {
    furi_check(view_dispatcher);
    view_dispatcher->event_context = context;
}

static void view_dispatcher_put(ViewDispatcher* view_dispatcher, ViewDispatcherMessage* m) {
    furi_check(
        furi_message_queue_put(view_dispatcher->queue, m, FuriWaitForever) == FuriStatusOk);
}

void view_dispatcher_run(ViewDispatcher* view_dispatcher) {
    furi_check(view_dispatcher);

    ViewDispatcherMessage m;
    uint32_t period = view_dispatcher->tick_period;
    uint32_t next_tick = furi_get_tick() + period;
    uint32_t timeout;

    view_dispatcher->running = true;
    while(view_dispatcher->running) {
        timeout = FuriWaitForever;
        if(period) {
            int32_t left = (int32_t)(next_tick - furi_get_tick());
            timeout = left > 0 ? (uint32_t)left : 0;
        }
        if(furi_message_queue_get(view_dispatcher->queue, &m, timeout) != FuriStatusOk) {
            next_tick += period;
            if((int32_t)(next_tick - furi_get_tick()) < 0) next_tick = furi_get_tick() + period;
            view_dispatcher_handle_tick_event(view_dispatcher);
        } else if(m.kind == ViewDispatcherMessageInput) {
            InputEvent input;
            input.sequence = m.value;
            input.key = (InputKey)m.key;
            input.type = (InputType)m.type;
            view_dispatcher_handle_input(view_dispatcher, &input);
        } else if(m.kind == ViewDispatcherMessageCustom) {
            view_dispatcher_handle_custom_event(view_dispatcher, m.value);
        }
    }

    // Wait till all input events delivered
    while(view_dispatcher->ongoing_input) {
        furi_message_queue_get(view_dispatcher->queue, &m, FuriWaitForever);
        if(m.kind != ViewDispatcherMessageInput) continue;
        uint8_t key_bit = (1 << m.key);
        if(m.type == InputTypePress) {
            view_dispatcher->ongoing_input |= key_bit;
        } else if(m.type == InputTypeRelease) {
            view_dispatcher->ongoing_input &= ~key_bit;
        }
    }
}

void view_dispatcher_stop(ViewDispatcher* view_dispatcher) {
    furi_check(view_dispatcher);
    ViewDispatcherMessage m = {.kind = ViewDispatcherMessageStop};
    view_dispatcher->running = false;
    /* wake view_dispatcher_run() if it waits in another thread */
    furi_message_queue_put(view_dispatcher->queue, &m, 0);
}

static View** view_dispatcher_find(ViewDispatcher* view_dispatcher, uint32_t view_id) {
    for(size_t i = 0; i < ViewArray_size(view_dispatcher->views); i++) {
        ViewEntry* e = ViewArray_get(view_dispatcher->views, i);
        if(e->id == view_id) return &e->view;
    }
    return NULL;
}

bool view_dispatcher_check_id(ViewDispatcher* view_dispatcher, uint32_t view_id) {
    furi_check(view_dispatcher);
    return view_dispatcher_find(view_dispatcher, view_id) == NULL;
}

void view_dispatcher_add_view(ViewDispatcher* view_dispatcher, uint32_t view_id, View* view) {
    furi_check(view_dispatcher);
    furi_check(view);
    // Check if view id is not used and register view
    furi_check(view_dispatcher_find(view_dispatcher, view_id) == NULL);

    ViewEntry* e = ViewArray_push_new(view_dispatcher->views);
    e->id = view_id;
    e->view = view;
    view_set_update_callback(view, view_dispatcher_update);
    view_set_update_callback_context(view, view_dispatcher);
}

void view_dispatcher_remove_view(ViewDispatcher* view_dispatcher, uint32_t view_id) {
    furi_check(view_dispatcher);

    // Get View by ID
    View** view_pp = view_dispatcher_find(view_dispatcher, view_id);
    furi_check(view_pp);
    View* view = *view_pp;

    // Disable the view if it is active
    if(view_dispatcher->current_view == view) {
        view_dispatcher_set_current_view(view_dispatcher, NULL);
    }
    // Check if view is receiving input
    if(view_dispatcher->ongoing_input_view == view) {
        view_dispatcher->ongoing_input_view = NULL;
    }
    // Remove view
    View** slot = view_dispatcher_find(view_dispatcher, view_id);
    furi_check(slot);
    ViewArray_it_t it;
    ViewArray_it(it, view_dispatcher->views);
    it->i = (size_t)((ViewEntry*)(void*)slot - ViewArray_get(view_dispatcher->views, 0));
    ViewArray_remove(view_dispatcher->views, it);

    view_set_update_callback(view, NULL);
    view_set_update_callback_context(view, NULL);
}

void view_dispatcher_switch_to_view(ViewDispatcher* view_dispatcher, uint32_t view_id) {
    furi_check(view_dispatcher);
    if(view_id == VIEW_NONE) {
        view_dispatcher_set_current_view(view_dispatcher, NULL);
    } else if(view_id == VIEW_IGNORE) {
    } else {
        View** view_pp = view_dispatcher_find(view_dispatcher, view_id);
        furi_check(view_pp != NULL);
        view_dispatcher_set_current_view(view_dispatcher, *view_pp);
    }
}

void view_dispatcher_send_to_front(ViewDispatcher* view_dispatcher) {
    furi_check(view_dispatcher);
    furi_check(view_dispatcher->gui);
    gui_view_port_send_to_front(view_dispatcher->gui, view_dispatcher->view_port);
}

void view_dispatcher_send_to_back(ViewDispatcher* view_dispatcher) {
    furi_check(view_dispatcher);
    furi_check(view_dispatcher->gui);
    gui_view_port_send_to_back(view_dispatcher->gui, view_dispatcher->view_port);
}

void view_dispatcher_attach_to_gui(
    ViewDispatcher* view_dispatcher,
    Gui* gui,
    ViewDispatcherType type) {
    furi_check(view_dispatcher);
    furi_check(view_dispatcher->gui == NULL);
    furi_check(gui);

    if(type == ViewDispatcherTypeDesktop) {
        gui_add_view_port(gui, view_dispatcher->view_port, GuiLayerDesktop);
    } else if(type == ViewDispatcherTypeWindow) {
        gui_add_view_port(gui, view_dispatcher->view_port, GuiLayerWindow);
    } else if(type == ViewDispatcherTypeFullscreen) {
        gui_add_view_port(gui, view_dispatcher->view_port, GuiLayerFullscreen);
    } else {
        furi_crash();
    }
    view_dispatcher->gui = gui;
}

void view_dispatcher_draw_callback(Canvas* canvas, void* context) {
    ViewDispatcher* view_dispatcher = context;
    if(view_dispatcher->current_view) {
        view_draw(view_dispatcher->current_view, canvas);
    }
}

void view_dispatcher_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* view_dispatcher = context;
    ViewDispatcherMessage m = {
        .kind = ViewDispatcherMessageInput,
        .key = (uint8_t)event->key,
        .type = (uint8_t)event->type,
        .value = event->sequence};
    view_dispatcher_put(view_dispatcher, &m);
}

void view_dispatcher_handle_input(ViewDispatcher* view_dispatcher, InputEvent* event) {
    // Check input complementarity
    uint8_t key_bit = (1 << event->key);
    if(event->type == InputTypePress) {
        view_dispatcher->ongoing_input |= key_bit;
    } else if(event->type == InputTypeRelease) {
        view_dispatcher->ongoing_input &= ~key_bit;
    } else if(!(view_dispatcher->ongoing_input & key_bit)) {
        FURI_LOG_D(
            TAG,
            "non-complementary input, discarding key: %s, type: %s, sequence: %p",
            input_get_key_name(event->key),
            input_get_type_name(event->type),
            (void*)(uintptr_t)event->sequence);
        return;
    }

    // Set ongoing input view if this is event is first press event
    if(!(view_dispatcher->ongoing_input & ~key_bit) && event->type == InputTypePress) {
        view_dispatcher->ongoing_input_view = view_dispatcher->current_view;
    }

    // Deliver event
    if(view_dispatcher->current_view &&
       view_dispatcher->ongoing_input_view == view_dispatcher->current_view) {
        // Dispatch input to current view
        bool is_consumed = view_input(view_dispatcher->current_view, event);

        // Navigate if input is not consumed
        if(!is_consumed && (event->key == InputKeyBack) &&
           (event->type == InputTypeShort || event->type == InputTypeLong)) {
            // Navigate to previous
            uint32_t view_id = view_previous(view_dispatcher->current_view);
            if(view_id != VIEW_IGNORE) {
                // Switch to returned view
                view_dispatcher_switch_to_view(view_dispatcher, view_id);
            } else if(view_dispatcher->navigation_event_callback) {
                // Dispatch navigation event
                if(!view_dispatcher->navigation_event_callback(view_dispatcher->event_context)) {
                    view_dispatcher_stop(view_dispatcher);
                    return;
                }
            }
        }
    } else if(view_dispatcher->ongoing_input_view && event->type == InputTypeRelease) {
        FURI_LOG_D(
            TAG,
            "View changed while key press %p -> %p. Sending key: %s, type: %s, sequence: %p to previous view",
            view_dispatcher->ongoing_input_view,
            view_dispatcher->current_view,
            input_get_key_name(event->key),
            input_get_type_name(event->type),
            (void*)(uintptr_t)event->sequence);
        view_input(view_dispatcher->ongoing_input_view, event);
    }
}

void view_dispatcher_handle_tick_event(void* context) {
    ViewDispatcher* view_dispatcher = context;
    if(view_dispatcher->tick_event_callback) {
        view_dispatcher->tick_event_callback(view_dispatcher->event_context);
    }
}

void view_dispatcher_handle_custom_event(ViewDispatcher* view_dispatcher, uint32_t event) {
    bool is_consumed = false;
    if(view_dispatcher->current_view) {
        is_consumed = view_custom(view_dispatcher->current_view, event);
    }
    // If custom event is not consumed in View, call callback
    if(!is_consumed && view_dispatcher->custom_event_callback) {
        view_dispatcher->custom_event_callback(view_dispatcher->event_context, event);
    }
}

void view_dispatcher_send_custom_event(ViewDispatcher* view_dispatcher, uint32_t event) {
    furi_check(view_dispatcher);

    ViewDispatcherMessage m = {.kind = ViewDispatcherMessageCustom, .value = event};
    view_dispatcher_put(view_dispatcher, &m);
}

static const ViewPortOrientation view_dispatcher_view_port_orientation_table[] = {
    [ViewOrientationVertical] = ViewPortOrientationVertical,
    [ViewOrientationVerticalFlip] = ViewPortOrientationVerticalFlip,
    [ViewOrientationHorizontal] = ViewPortOrientationHorizontal,
    [ViewOrientationHorizontalFlip] = ViewPortOrientationHorizontalFlip,
};

void view_dispatcher_set_current_view(ViewDispatcher* view_dispatcher, View* view) {
    furi_check(view_dispatcher);
    // Dispatch view exit event
    if(view_dispatcher->current_view) {
        view_exit(view_dispatcher->current_view);
    }
    // Set current view
    view_dispatcher->current_view = view;
    // Dispatch view enter event
    if(view_dispatcher->current_view) {
        ViewPortOrientation orientation =
            view_dispatcher_view_port_orientation_table[view->orientation];
        if(view_port_get_orientation(view_dispatcher->view_port) != orientation) {
            view_port_set_orientation(view_dispatcher->view_port, orientation);
            // we just rotated input keys, now it's time to sacrifice some input
            view_dispatcher->ongoing_input = 0;
        }
        view_enter(view_dispatcher->current_view);
        view_port_enabled_set(view_dispatcher->view_port, true);
        view_port_update(view_dispatcher->view_port);
    } else {
        view_port_enabled_set(view_dispatcher->view_port, false);
        view_dispatcher_stop(view_dispatcher);
    }
}

void view_dispatcher_update(View* view, void* context) {
    furi_check(view);
    furi_check(context);

    ViewDispatcher* view_dispatcher = context;

    if(view_dispatcher->current_view == view) {
        view_port_update(view_dispatcher->view_port);
    }
}
