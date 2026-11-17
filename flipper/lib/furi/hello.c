/* SPDX-License-Identifier: GPL-3.0-or-later
 * furihello: the libfuri GUI smoke test (spec R5.2), written like a stock
 * app: a ViewDispatcher with a Submenu and a Popup.
 *   "Say hello" -> a popup; Back -> the menu again
 *   "Quit" or Back in the menu -> exit 0
 * On exit it prints how many frames went to /dev/lcd and how much of the
 * gui thread's stack was never used. m2test drives it with fake buttons
 * in QEMU; on the Flipper run /usr/tests/furihello. */
#include <stdio.h>
#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/popup.h>

#include "src/gui/gui_i.h" /* the stats line only */

typedef enum {
    HelloViewMenu,
    HelloViewPopup,
} HelloView;

typedef enum {
    HelloItemHello,
    HelloItemQuit,
} HelloItem;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    Popup* popup;
} HelloApp;

static int frames;

static void hello_count_frame(uint8_t* data, size_t size, CanvasOrientation o, void* context) {
    UNUSED(data);
    UNUSED(size);
    UNUSED(o);
    UNUSED(context);
    frames++;
}

static void hello_menu_callback(void* context, uint32_t index) {
    HelloApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static bool hello_custom_event(void* context, uint32_t event) {
    HelloApp* app = context;
    if(event == HelloItemHello) {
        view_dispatcher_switch_to_view(app->view_dispatcher, HelloViewPopup);
    } else {
        view_dispatcher_stop(app->view_dispatcher);
    }
    return true;
}

static uint32_t hello_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static uint32_t hello_back_to_menu(void* context) {
    UNUSED(context);
    return HelloViewMenu;
}

int32_t furi_hello_app(void* p) {
    UNUSED(p);
    HelloApp* app = malloc(sizeof(HelloApp));

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_framebuffer_callback(app->gui, hello_count_frame, NULL);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, hello_custom_event);

    app->submenu = submenu_alloc();
    submenu_set_header(app->submenu, "libfuri");
    submenu_add_item(app->submenu, "Say hello", HelloItemHello, hello_menu_callback, app);
    submenu_add_item(app->submenu, "Quit", HelloItemQuit, hello_menu_callback, app);
    view_set_previous_callback(submenu_get_view(app->submenu), hello_exit);
    view_dispatcher_add_view(app->view_dispatcher, HelloViewMenu, submenu_get_view(app->submenu));

    app->popup = popup_alloc();
    popup_set_header(app->popup, "Hello, MINIX!", 64, 20, AlignCenter, AlignTop);
    popup_set_text(app->popup, "Back: menu", 64, 40, AlignCenter, AlignTop);
    view_set_previous_callback(popup_get_view(app->popup), hello_back_to_menu);
    view_dispatcher_add_view(app->view_dispatcher, HelloViewPopup, popup_get_view(app->popup));

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->view_dispatcher, HelloViewMenu);
    view_dispatcher_run(app->view_dispatcher);

    printf(
        "furihello: %d frames, gui stack %u of %u bytes never used\n",
        frames,
        (unsigned)furi_thread_get_stack_space(furi_thread_get_id(app->gui->thread)),
        GUI_THREAD_STACK);

    view_dispatcher_remove_view(app->view_dispatcher, HelloViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, HelloViewPopup);
    view_dispatcher_free(app->view_dispatcher);
    submenu_free(app->submenu);
    popup_free(app->popup);
    gui_remove_framebuffer_callback(app->gui, hello_count_frame, NULL);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}

int main(void) {
    return (int)furi_hello_app(NULL);
}
