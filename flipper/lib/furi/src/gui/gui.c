/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri GUI (spec R5.2): the Gui service inside the app's process.
 * Layer logic (which ViewPort is drawn and gets input) from the stock
 * firmware applications/services/gui/gui.c (GPL-3.0); the input
 * service (applications/services/input/input.c) is folded in.
 *
 * One cooperative thread ("gui", GUI_THREAD_STACK bytes) does all of it:
 * - reads /dev/buttons (2-byte events: key bit, pressed) with ffd_wait(),
 *   so it blocks only itself, and turns them into the stock InputEvent
 *   sequence: Press, then Short + Release on a release before 300 ms, or
 *   Long at 300 ms and Repeat every 150 ms while held, then Release;
 * - delivers each event to the top enabled ViewPort's input callback
 *   (the one that got the Press keeps the key until its Release);
 * - redraws only when asked (view_port_update(), gui_update()): no
 *   periodic redraw. A redraw request wakes the thread out of its
 *   /dev/buttons wait at once (ffd_kick()).
 * The frame goes to /dev/lcd with one write of 1024 bytes.
 * Window and Desktop layers keep the stock geometry: the 13 pixels on top
 * that the stock firmware uses for its status bar carry the black title
 * bar of flipper/gui (the app's name, white on black). */
#include "gui_i.h"
#include "../furi_i.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#ifdef FURI_HOST
#define GUI_LCD     (getenv("FURI_LCD") ? getenv("FURI_LCD") : "/dev/null")
#define GUI_BUTTONS getenv("FURI_BUTTONS")
#else
#define GUI_LCD     "/dev/lcd"
#define GUI_BUTTONS "/dev/buttons"
#endif

/* /dev/buttons bits (flipper/hal/hal.h BTN_*) -> InputKey */
static const uint8_t gui_btn_key[6] = {
    InputKeyUp, InputKeyDown, InputKeyLeft, InputKeyRight, InputKeyOk, InputKeyBack};

static ViewPort* gui_view_port_find_enabled(ViewPortArray_t array) {
    ViewPortArray_it_t it;
    ViewPortArray_it_last(it, array);
    while(!ViewPortArray_end_p(it)) {
        ViewPort* view_port = *ViewPortArray_ref(it);
        if(view_port_is_enabled(view_port)) return view_port;
        ViewPortArray_previous(it);
    }
    return NULL;
}

static bool gui_redraw_layer(Gui* gui, GuiLayer layer, bool window) {
    ViewPort* view_port = gui_view_port_find_enabled(gui->layers[layer]);
    if(!view_port) return false;
    if(window)
        canvas_frame_set(gui->canvas, GUI_WINDOW_X, GUI_WINDOW_Y, GUI_WINDOW_WIDTH, GUI_WINDOW_HEIGHT);
    else
        canvas_frame_set(gui->canvas, 0, 0, GUI_DISPLAY_WIDTH, GUI_DISPLAY_HEIGHT);
    view_port_draw(view_port, gui->canvas);
    return true;
}

static void gui_redraw_title(Gui* gui) {
    Canvas* canvas = gui->canvas;
    canvas_frame_set(canvas, 0, 0, GUI_DISPLAY_WIDTH, GUI_DISPLAY_HEIGHT);
    canvas_set_orientation(canvas, CanvasOrientationHorizontal);
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, 0, 0, GUI_STATUS_BAR_WIDTH, GUI_STATUS_BAR_HEIGHT);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0, GUI_STATUS_BAR_WIDTH, GUI_TITLE_BAR_HEIGHT);
    if(gui->title) {
        canvas_set_color(canvas, ColorWhite);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, gui->title);
        canvas_set_color(canvas, ColorBlack);
    }
}

void gui_redraw(Gui* gui) {
    furi_check(gui);
    if(gui->direct_draw) return;
    canvas_reset(gui->canvas);
    if(gui->lockdown) {
        gui_redraw_layer(gui, GuiLayerDesktop, false);
        gui_redraw_title(gui);
    } else if(!gui_redraw_layer(gui, GuiLayerFullscreen, false)) {
        bool drawn = gui_redraw_layer(gui, GuiLayerWindow, true);
        if(!drawn) drawn = gui_redraw_layer(gui, GuiLayerDesktop, false);
        if(drawn) gui_redraw_title(gui);
    }
    canvas_commit(gui->canvas);
}

/* canvas_commit(): the frame to /dev/lcd (also after direct draw) */
static void gui_lcd_write(uint8_t* data, size_t size, CanvasOrientation orientation, void* context) {
    Gui* gui = context;
    UNUSED(orientation);
    lseek(gui->lcd_fd, 0L, SEEK_SET);
    if(write(gui->lcd_fd, data, size) < 0) FURI_LOG_W("Gui", "lcd write: %d", errno);
}

static void gui_input(Gui* gui, InputEvent* input_event) {
    uint8_t key_bit = (uint8_t)(1U << input_event->key);

    if(input_event->type == InputTypeRelease) {
        gui->ongoing_input &= (uint8_t)~key_bit;
    } else if(input_event->type == InputTypePress) {
        gui->ongoing_input |= key_bit;
    } else if(!(gui->ongoing_input & key_bit)) {
        return; /* non-complementary input */
    }
    if(gui->direct_draw && !gui->ongoing_input_view_port) return;

    ViewPort* view_port;
    if(gui->lockdown) {
        view_port = gui_view_port_find_enabled(gui->layers[GuiLayerDesktop]);
    } else {
        view_port = gui_view_port_find_enabled(gui->layers[GuiLayerFullscreen]);
        if(!view_port) view_port = gui_view_port_find_enabled(gui->layers[GuiLayerWindow]);
        if(!view_port) view_port = gui_view_port_find_enabled(gui->layers[GuiLayerDesktop]);
    }
    if(!(gui->ongoing_input & ~key_bit) && input_event->type == InputTypePress)
        gui->ongoing_input_view_port = view_port;

    if(view_port && view_port == gui->ongoing_input_view_port) {
        view_port_input(view_port, input_event);
    } else if(gui->ongoing_input_view_port && input_event->type == InputTypeRelease) {
        view_port_input(gui->ongoing_input_view_port, input_event);
    }
}

static void gui_send(Gui* gui, InputKey key, InputType type) {
    InputEvent event;
    event.sequence_source = INPUT_SEQUENCE_SOURCE_HARDWARE;
    event.sequence_counter = gui->keys[key].counter;
    event.key = key;
    event.type = type;
    gui_input(gui, &event);
}

void gui_buttons_event(Gui* gui, uint8_t bit, uint8_t pressed, uint32_t now) {
    unsigned i;
    for(i = 0; i < COUNT_OF(gui_btn_key); i++)
        if(bit == (1U << i)) break;
    if(i == COUNT_OF(gui_btn_key)) return;
    InputKey key = (InputKey)gui_btn_key[i];
    GuiKey* k = &gui->keys[key];

    if(pressed) {
        if(k->down) return;
        k->down = 1;
        k->press_counter = 0;
        k->counter = ++gui->sequence;
        k->deadline = now + GUI_INPUT_PRESS_MS;
        gui_send(gui, key, InputTypePress);
    } else {
        if(!k->down) return;
        k->down = 0;
        if(k->press_counter < GUI_INPUT_LONG_COUNTS) gui_send(gui, key, InputTypeShort);
        k->press_counter = 0;
        gui_send(gui, key, InputTypeRelease);
    }
}

uint32_t gui_input_tick(Gui* gui, uint32_t now) {
    uint32_t wait = FuriWaitForever;
    unsigned key;

    for(key = 0; key < InputKeyMAX; key++) {
        GuiKey* k = &gui->keys[key];
        while(k->down && (int32_t)(k->deadline - now) <= 0) {
            k->deadline += GUI_INPUT_PRESS_MS;
            k->press_counter++;
            if(k->press_counter == GUI_INPUT_LONG_COUNTS) {
                gui_send(gui, (InputKey)key, InputTypeLong);
            } else if(k->press_counter > GUI_INPUT_LONG_COUNTS) {
                k->press_counter--;
                gui_send(gui, (InputKey)key, InputTypeRepeat);
            }
        }
        if(k->down && k->deadline - now < wait) wait = k->deadline - now;
    }
    return wait;
}

static int32_t gui_thread(void* context) {
    Gui* gui = context;
    uint8_t ev[16];
    bool hinted = false, retry = false;
    ssize_t n, i;
    uint32_t wait;
    int r;

    for(;;) {
        if(gui->dirty) {
            gui->dirty = false;
            gui_redraw(gui);
        }
        wait = gui_input_tick(gui, fport_now());
        if(gui->buttons_fd < 0) {
            furi_thread_flags_wait(1, FuriFlagWaitAny, wait);
            continue;
        }
        n = ffd_read(gui->buttons_fd, ev, sizeof ev, 0);
        if(n > 0) {
            for(i = 0; i + 1 < n; i += 2)
                gui_buttons_event(gui, ev[i], ev[i + 1], fport_now());
            hinted = retry = false;
            continue;
        }
        if(n < 0 && errno != EAGAIN) {
            FURI_LOG_E("Gui", "buttons: %d", errno);
            close(gui->buttons_fd);
            gui->buttons_fd = -1;
            continue;
        }
        if(gui->dirty) continue;
        /* poll() said ready, read() found nothing: try once per tick */
        if(hinted) retry = true;
        r = ffd_wait(gui->buttons_fd, POLLIN, wait, retry);
        hinted = r != 0 && !(r & FFD_KICK);
    }
    return 0;
}

void gui_update(Gui* gui) {
    furi_check(gui);
    if(gui->dirty) return;
    gui->dirty = true;
    if(!gui->thread) return;
    if(gui->buttons_fd >= 0)
        ffd_kick(gui->buttons_fd);
    else
        furi_thread_flags_set(furi_thread_get_id(gui->thread), 1);
}

Gui* gui_alloc_headless(void) {
    Gui* gui = calloc(1, sizeof(Gui));
    furi_check(gui, "out of memory");
    for(size_t i = 0; i < GuiLayerMAX; i++)
        ViewPortArray_init(gui->layers[i]);
    gui->canvas = canvas_init();
    gui->lcd_fd = gui->buttons_fd = -1;
    return gui;
}

void fgui_service_start(void) {
    const char* path;
    Gui* gui = gui_alloc_headless();

    gui->lcd_fd = open(GUI_LCD, O_WRONLY);
    if(gui->lcd_fd >= 0) canvas_add_framebuffer_callback(gui->canvas, gui_lcd_write, gui);
    path = GUI_BUTTONS;
    gui->buttons_fd = path ? open(path, O_RDONLY) : -1;
    gui->thread = furi_thread_alloc_ex("gui", GUI_THREAD_STACK, gui_thread, gui);
    furi_record_create(RECORD_GUI, gui);
    furi_thread_start(gui->thread);
}

void gui_add_view_port(Gui* gui, ViewPort* view_port, GuiLayer layer) {
    furi_check(gui);
    furi_check(view_port);
    furi_check(layer < GuiLayerMAX);
    ViewPortOrientation orientation = view_port_get_orientation(view_port);
    furi_check(
        (layer == GuiLayerFullscreen) || ((orientation != ViewPortOrientationVertical) &&
                                          (orientation != ViewPortOrientationVerticalFlip)));
    furi_check(view_port->gui == NULL, "ViewPort already added");
    if(!gui->title) {
        FuriThreadId id = furi_thread_get_current_id();
        gui->title = furi_thread_get_appid(id);
        if(!gui->title) gui->title = furi_thread_get_name(id);
    }
    ViewPortArray_push_back(gui->layers[layer], view_port);
    view_port_gui_set(view_port, gui);
    gui_update(gui);
}

static bool gui_unlink(Gui* gui, ViewPort* view_port) {
    ViewPortArray_it_t it;
    for(size_t i = 0; i < GuiLayerMAX; i++) {
        ViewPortArray_it(it, gui->layers[i]);
        while(!ViewPortArray_end_p(it)) {
            if(*ViewPortArray_ref(it) == view_port) {
                ViewPortArray_remove(gui->layers[i], it);
                return true;
            }
            ViewPortArray_next(it);
        }
    }
    return false;
}

void gui_remove_view_port(Gui* gui, ViewPort* view_port) {
    furi_check(gui);
    furi_check(view_port);
    view_port_gui_set(view_port, NULL);
    gui_unlink(gui, view_port);
    if(gui->ongoing_input_view_port == view_port) gui->ongoing_input_view_port = NULL;
    gui_update(gui);
}

static GuiLayer gui_layer_of(Gui* gui, ViewPort* view_port) {
    for(size_t i = 0; i < GuiLayerMAX; i++)
        for(size_t j = 0; j < ViewPortArray_size(gui->layers[i]); j++)
            if(*ViewPortArray_get(gui->layers[i], j) == view_port) return (GuiLayer)i;
    furi_crash("ViewPort not found");
}

void gui_view_port_send_to_front(Gui* gui, ViewPort* view_port) {
    furi_check(gui);
    furi_check(view_port);
    GuiLayer layer = gui_layer_of(gui, view_port);
    gui_unlink(gui, view_port);
    ViewPortArray_push_back(gui->layers[layer], view_port);
    gui_update(gui);
}

void gui_view_port_send_to_back(Gui* gui, ViewPort* view_port) {
    furi_check(gui);
    furi_check(view_port);
    GuiLayer layer = gui_layer_of(gui, view_port);
    gui_unlink(gui, view_port);
    ViewPortArray_push_at(gui->layers[layer], 0, view_port);
    gui_update(gui);
}

void gui_add_framebuffer_callback(Gui* gui, GuiCanvasCommitCallback callback, void* context) {
    furi_check(gui);
    canvas_add_framebuffer_callback(gui->canvas, callback, context);
    gui_update(gui);
}

void gui_remove_framebuffer_callback(Gui* gui, GuiCanvasCommitCallback callback, void* context) {
    furi_check(gui);
    canvas_remove_framebuffer_callback(gui->canvas, callback, context);
}

size_t gui_get_framebuffer_size(const Gui* gui) {
    furi_check(gui);
    return canvas_get_buffer_size(gui->canvas);
}

void gui_set_lockdown(Gui* gui, bool lockdown) {
    furi_check(gui);
    gui->lockdown = lockdown;
    gui_update(gui);
}

Canvas* gui_direct_draw_acquire(Gui* gui) {
    furi_check(gui);
    gui->direct_draw = true;
    canvas_reset(gui->canvas);
    return gui->canvas;
}

void gui_direct_draw_release(Gui* gui) {
    furi_check(gui);
    gui->direct_draw = false;
    canvas_reset(gui->canvas);
    gui_update(gui);
}

const char* input_get_key_name(InputKey key) {
    static const char names[InputKeyMAX][6] = {"Up", "Down", "Right", "Left", "Ok", "Back"};
    return key < InputKeyMAX ? names[key] : "Unknown";
}

const char* input_get_type_name(InputType type) {
    static const char names[InputTypeMAX][8] = {"Press", "Release", "Short", "Long", "Repeat"};
    return type < InputTypeMAX ? names[type] : "Unknown";
}
