/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri GUI host tests (spec R5.2).
 *
 *   test_gui DIR      render each module and compare it with DIR/<name>.pbm
 *   test_gui -w DIR   write DIR/<name>.pbm instead (make gui-ref)
 *
 * A mismatch prints both images as text and writes build/<name>.pbm.
 * Besides the images: the stock input sequence (Press, Short/Long/Repeat,
 * Release) from /dev/buttons events, ViewDispatcher + SceneManager
 * navigation, and the whole gui thread on a pipe standing in for
 * /dev/buttons (redraw only on change). */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/elements.h>
#include <gui/modules/submenu.h>
#include <gui/modules/popup.h>
#include <gui/modules/dialog_ex.h>
#include <gui/modules/loading.h>
#include <gui/modules/text_input.h>
#include <gui/modules/byte_input.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include <assets_icons.h>

#include "src/gui/gui_i.h"
#include "src/gui/view_i.h"
#include "src/gui/view_dispatcher_i.h"

static int fails, checks;
static bool write_refs;
static const char* ref_dir;
static Canvas* canvas;

#define CHECK(c, what)                                       \
    do {                                                     \
        checks++;                                            \
        if(!(c)) {                                           \
            fails++;                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); \
        }                                                    \
    } while(0)

/* ---- images ---- */

static int px(const uint8_t* fb, int x, int y) {
    return (fb[(y >> 3) * 128 + x] >> (y & 7)) & 1;
}

static void pbm_write(const char* path, const uint8_t* fb) {
    FILE* f = fopen(path, "wb");
    if(!f) {
        printf("FAIL: cannot write %s\n", path);
        fails++;
        return;
    }
    fprintf(f, "P4\n128 64\n");
    for(int y = 0; y < 64; y++)
        for(int xb = 0; xb < 128; xb += 8) {
            uint8_t b = 0;
            for(int i = 0; i < 8; i++)
                b |= (uint8_t)(px(fb, xb + i, y) << (7 - i));
            fputc(b, f);
        }
    fclose(f);
}

/* 1 if read, the image into fb (page layout) */
static int pbm_read(const char* path, uint8_t* fb) {
    char head[16];
    FILE* f = fopen(path, "rb");
    if(!f) return 0;
    if(!fgets(head, sizeof head, f) || strcmp(head, "P4\n") || !fgets(head, sizeof head, f) ||
       strcmp(head, "128 64\n")) {
        fclose(f);
        return 0;
    }
    memset(fb, 0, 1024);
    for(int y = 0; y < 64; y++)
        for(int xb = 0; xb < 128; xb += 8) {
            int b = fgetc(f);
            if(b < 0) {
                fclose(f);
                return 0;
            }
            for(int i = 0; i < 8; i++)
                if(b & (0x80 >> i)) fb[(y >> 3) * 128 + xb + i] |= (uint8_t)(1 << (y & 7));
        }
    fclose(f);
    return 1;
}

static void show(const uint8_t* fb) {
    for(int y = 0; y < 64; y++) {
        for(int x = 0; x < 128; x++)
            putchar(px(fb, x, y) ? '#' : '.');
        putchar('\n');
    }
}

static void compare(const char* name, const uint8_t* fb) {
    char path[256];
    uint8_t ref[1024];

    snprintf(path, sizeof path, "%s/%s.pbm", ref_dir, name);
    if(write_refs) {
        pbm_write(path, fb);
        printf("wrote %s\n", path);
        show(fb);
        return;
    }
    checks++;
    if(!pbm_read(path, ref)) {
        fails++;
        printf("FAIL %s: no reference %s\n", name, path);
        return;
    }
    if(memcmp(ref, fb, 1024) != 0) {
        fails++;
        printf("FAIL %s: differs from %s\n-- expected:\n", name, path);
        show(ref);
        printf("-- got:\n");
        show(fb);
        snprintf(path, sizeof path, "build/%s.pbm", name);
        pbm_write(path, fb);
    }
}

static void render(const char* name, View* view) {
    canvas_reset(canvas);
    canvas_frame_set(canvas, 0, 0, 128, 64);
    view_draw(view, canvas);
    compare(name, canvas_get_buffer(canvas));
}

static bool key(View* view, InputKey k, InputType type) {
    InputEvent e = {.sequence = 1, .key = k, .type = type};
    return view_input(view, &e);
}

static void tap(View* view, InputKey k) {
    key(view, k, InputTypePress);
    key(view, k, InputTypeShort);
    key(view, k, InputTypeRelease);
}

/* ---- Canvas ---- */

static void test_canvas(void) {
    canvas_reset(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Primary Font");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 20, "Secondary font 0123");
    canvas_set_font(canvas, FontKeyboard);
    canvas_draw_str(canvas, 2, 30, "Keyboard");
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(canvas, 126, 2, AlignRight, AlignTop, "42");
    canvas_draw_rframe(canvas, 2, 34, 40, 20, 3);
    canvas_draw_rbox(canvas, 46, 34, 20, 20, 3);
    canvas_draw_circle(canvas, 80, 44, 9);
    canvas_draw_disc(canvas, 102, 44, 6);
    canvas_draw_line(canvas, 0, 63, 127, 56);
    canvas_draw_icon(canvas, 116, 34, &I_ButtonCenter_7x7);
    canvas_set_color(canvas, ColorXOR);
    canvas_draw_box(canvas, 0, 0, 30, 12);
    canvas_set_color(canvas, ColorBlack);
    compare("canvas", canvas_get_buffer(canvas));
    CHECK(canvas_string_width(canvas, "42") > 0, "string width");
}

/* ---- modules ---- */

static uint32_t last_index = 0xFFFF;
static void submenu_cb(void* context, uint32_t index) {
    UNUSED(context);
    last_index = index;
}

static void test_submenu(void) {
    Submenu* submenu = submenu_alloc();
    View* view = submenu_get_view(submenu);
    submenu_set_header(submenu, "Hello");
    submenu_add_item(submenu, "First item", 10, submenu_cb, NULL);
    submenu_add_item(submenu, "Second item", 11, submenu_cb, NULL);
    submenu_add_item(submenu, "Third item", 12, submenu_cb, NULL);
    submenu_add_item(submenu, "Fourth item", 13, submenu_cb, NULL);
    submenu_add_item(submenu, "Fifth item", 14, submenu_cb, NULL);
    render("submenu", view);
    tap(view, InputKeyDown);
    tap(view, InputKeyDown);
    render("submenu_down", view);
    tap(view, InputKeyOk);
    CHECK(last_index == 12, "submenu: OK selects the third item");
    submenu_change_item_label(submenu, 12, "Changed");
    submenu_reset(submenu);
    submenu_add_item(submenu, "Only", 1, submenu_cb, NULL);
    tap(view, InputKeyOk);
    CHECK(last_index == 1, "submenu: reset and add");
    submenu_free(submenu);
}

static int popup_done;
static void popup_cb(void* context) {
    UNUSED(context);
    popup_done++;
}

static void test_popup(void) {
    Popup* popup = popup_alloc();
    View* view = popup_get_view(popup);
    popup_set_header(popup, "Saved!", 64, 4, AlignCenter, AlignTop);
    popup_set_text(popup, "The key was\nwritten.", 4, 30, AlignLeft, AlignTop);
    popup_set_icon(popup, 100, 30, &I_WarningDolphin_45x42);
    popup_set_callback(popup, popup_cb);
    render("popup", view);
    tap(view, InputKeyBack);
    CHECK(popup_done == 1, "popup: callback on a key");
    popup_free(popup);
}

static DialogExResult dialog_result = (DialogExResult)-1;
static void dialog_cb(DialogExResult result, void* context) {
    UNUSED(context);
    dialog_result = result;
}

static void test_dialog_ex(void) {
    DialogEx* dialog = dialog_ex_alloc();
    View* view = dialog_ex_get_view(dialog);
    dialog_ex_set_header(dialog, "Delete file?", 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(dialog, "notes.txt\nwill be gone.", 64, 26, AlignCenter, AlignCenter);
    dialog_ex_set_left_button_text(dialog, "Cancel");
    dialog_ex_set_center_button_text(dialog, "Info");
    dialog_ex_set_right_button_text(dialog, "Delete");
    dialog_ex_set_result_callback(dialog, dialog_cb);
    render("dialog_ex", view);
    tap(view, InputKeyRight);
    CHECK(dialog_result == DialogExResultRight, "dialog_ex: Right");
    tap(view, InputKeyLeft);
    CHECK(dialog_result == DialogExResultLeft, "dialog_ex: Left");
    dialog_ex_free(dialog);
}

static void test_loading(void) {
    Loading* loading = loading_alloc();
    View* view = loading_get_view(loading);
    view_enter(view);
    render("loading", view);
    view_exit(view);
    loading_free(loading);
}

static char text_buf[16] = "flip";
static int text_done;
static void text_cb(void* context) {
    UNUSED(context);
    text_done++;
}

static void test_text_input(void) {
    TextInput* text_input = text_input_alloc();
    View* view = text_input_get_view(text_input);
    text_input_set_header_text(text_input, "Name the key");
    text_input_set_result_callback(text_input, text_cb, NULL, text_buf, sizeof text_buf, false);
    render("text_input", view);
    tap(view, InputKeyOk); /* text given: the cursor starts on Save */
    CHECK(text_done == 1, "text_input: OK on Save returns the text");
    tap(view, InputKeyLeft);
    tap(view, InputKeyLeft);
    tap(view, InputKeyOk); /* a letter left of Save */
    key(view, InputKeyOk, InputTypePress);
    key(view, InputKeyOk, InputTypeLong); /* the same letter, upper case */
    key(view, InputKeyOk, InputTypeRelease);
    if(strlen(text_buf) != 6) printf("text_buf \"%s\"\n", text_buf);
    CHECK(strlen(text_buf) == 6, "text_input: two letters typed");
    render("text_input_typed", view);
    text_input_free(text_input);
}

static uint8_t bytes[4] = {0xDE, 0xAD, 0xBE, 0xEF};
static int bytes_changed;
static void bytes_changed_cb(void* context) {
    UNUSED(context);
    bytes_changed++;
}

static void test_byte_input(void) {
    ByteInput* byte_input = byte_input_alloc();
    View* view = byte_input_get_view(byte_input);
    byte_input_set_header_text(byte_input, "Enter the key");
    byte_input_set_result_callback(byte_input, NULL, bytes_changed_cb, NULL, bytes, sizeof bytes);
    render("byte_input", view);
    tap(view, InputKeyOk); /* the selected key: '0' */
    if(bytes[0] != 0x0E) printf("bytes %02x %02x, changed %d\n", bytes[0], bytes[1], bytes_changed);
    CHECK(bytes[0] == 0x0E && bytes_changed == 1, "byte_input: OK types the selected digit");
    byte_input_free(byte_input);
}

static const char* const on_off[] = {"OFF", "ON"};
static int vil_changes;
static void vil_change(VariableItem* item) {
    vil_changes++;
    variable_item_set_current_value_text(item, on_off[variable_item_get_current_value_index(item)]);
}

static void test_variable_item_list(void) {
    VariableItemList* list = variable_item_list_alloc();
    View* view = variable_item_list_get_view(list);
    VariableItem* item = variable_item_list_add(list, "Backlight", 2, vil_change, NULL);
    variable_item_set_current_value_index(item, 1);
    variable_item_set_current_value_text(item, "ON");
    item = variable_item_list_add(list, "Volume", 10, NULL, NULL);
    variable_item_set_current_value_index(item, 3);
    variable_item_set_current_value_text(item, "30%");
    variable_item_list_add(list, "About", 0, NULL, NULL);
    render("variable_item_list", view);
    tap(view, InputKeyLeft);
    CHECK(vil_changes == 1, "variable_item_list: Left changes the value");
    render("variable_item_list_off", view);
    variable_item_list_free(list);
}

static void test_widget(void) {
    Widget* widget = widget_alloc();
    View* view = widget_get_view(widget);
    widget_add_string_element(widget, 64, 2, AlignCenter, AlignTop, FontPrimary, "Widget");
    widget_add_string_multiline_element(
        widget, 2, 16, AlignLeft, AlignTop, FontSecondary, "two\nlines");
    widget_add_frame_element(widget, 60, 14, 64, 30, 3);
    widget_add_text_box_element(
        widget, 62, 16, 60, 26, AlignCenter, AlignCenter, "\e#Bold\e# text in a box", false);
    widget_add_icon_element(widget, 40, 18, &I_Hashmark_7x7);
    widget_add_button_element(widget, GuiButtonTypeLeft, "Back", NULL, NULL);
    widget_add_button_element(widget, GuiButtonTypeRight, "More", NULL, NULL);
    render("widget", view);
    widget_free(widget);
}

/* ---- Gui: title bar, input sequence ---- */

static InputEvent seen[16];
static int nseen;
static void record_input(InputEvent* event, void* context) {
    UNUSED(context);
    if(nseen < 16) seen[nseen++] = *event;
}

static void draw_hello(Canvas* c, void* context) {
    UNUSED(context);
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 12, "In a window");
    elements_button_center(c, "OK");
}

static void test_gui_window_and_input(void) {
    Gui* gui = gui_alloc_headless();
    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, draw_hello, NULL);
    view_port_input_callback_set(vp, record_input, NULL);
    gui->title = "hello";
    gui_add_view_port(gui, vp, GuiLayerWindow);
    gui_redraw(gui);
    compare("gui_window", canvas_get_buffer(gui->canvas));

    /* short press: Press, Short, Release */
    gui_buttons_event(gui, 0x10, 1, 1000);
    CHECK(gui_input_tick(gui, 1100) == 50, "input: next step in 50 ms");
    gui_buttons_event(gui, 0x10, 0, 1120);
    CHECK(nseen == 3 && seen[0].type == InputTypePress && seen[1].type == InputTypeShort &&
              seen[2].type == InputTypeRelease && seen[0].key == InputKeyOk,
          "input: short press");
    CHECK(seen[0].sequence_counter == seen[2].sequence_counter, "input: one sequence");
    /* held: Press, Long at 300 ms, Repeat every 150 ms, Release */
    nseen = 0;
    gui_buttons_event(gui, 0x02, 1, 2000);
    gui_input_tick(gui, 2299);
    CHECK(nseen == 1, "input: nothing before 300 ms");
    gui_input_tick(gui, 2300);
    gui_input_tick(gui, 2450);
    gui_input_tick(gui, 2600);
    gui_buttons_event(gui, 0x02, 0, 2610);
    CHECK(nseen == 5 && seen[1].type == InputTypeLong && seen[2].type == InputTypeRepeat &&
              seen[3].type == InputTypeRepeat && seen[4].type == InputTypeRelease &&
              seen[4].key == InputKeyDown,
          "input: long press with repeats, no Short");
    CHECK(gui_input_tick(gui, 2700) == FuriWaitForever, "input: no key held, no timer");
    /* a release without a press is dropped */
    nseen = 0;
    gui_buttons_event(gui, 0x20, 0, 3000);
    CHECK(nseen == 0, "input: stray release dropped");

    gui_remove_view_port(gui, vp);
    view_port_free(vp);
}

/* ---- ViewDispatcher + SceneManager ---- */

typedef struct {
    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    Popup* popup;
    int entered[2];
} App;

enum {
    SceneMenu,
    ScenePopup,
};
enum {
    ViewMenu,
    ViewPopup,
};

static void app_menu_cb(void* context, uint32_t index) {
    App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static void scene_menu_on_enter(void* context) {
    App* app = context;
    app->entered[SceneMenu]++;
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewMenu);
}
static bool scene_menu_on_event(void* context, SceneManagerEvent event) {
    App* app = context;
    if(event.type == SceneManagerEventTypeCustom && event.event == 7) {
        scene_manager_next_scene(app->scene_manager, ScenePopup);
        return true;
    }
    return false;
}
static void scene_on_exit(void* context) {
    UNUSED(context);
}
static void scene_popup_on_enter(void* context) {
    App* app = context;
    app->entered[ScenePopup]++;
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewPopup);
}
static bool scene_popup_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

static void (*const scene_on_enter_h[])(void*) = {scene_menu_on_enter, scene_popup_on_enter};
static bool (*const scene_on_event_h[])(void*, SceneManagerEvent) = {
    scene_menu_on_event,
    scene_popup_on_event};
static void (*const scene_on_exit_h[])(void*) = {scene_on_exit, scene_on_exit};
static const SceneManagerHandlers handlers = {
    .on_enter_handlers = scene_on_enter_h,
    .on_event_handlers = scene_on_event_h,
    .on_exit_handlers = scene_on_exit_h,
    .scene_num = 2,
};

static bool app_custom(void* context, uint32_t event) {
    App* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}
static bool app_back(void* context) {
    App* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static App* app_alloc(void) {
    App* app = calloc(1, sizeof(App));
    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&handlers, app);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, app_custom);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, app_back);
    app->submenu = submenu_alloc();
    submenu_set_header(app->submenu, "Hello");
    submenu_add_item(app->submenu, "Say hello", 7, app_menu_cb, app);
    submenu_add_item(app->submenu, "Quit", 8, app_menu_cb, app);
    view_dispatcher_add_view(app->view_dispatcher, ViewMenu, submenu_get_view(app->submenu));
    app->popup = popup_alloc();
    popup_set_header(app->popup, "Hello, MINIX!", 64, 20, AlignCenter, AlignTop);
    view_dispatcher_add_view(app->view_dispatcher, ViewPopup, popup_get_view(app->popup));
    return app;
}

static void app_free(App* app) {
    view_dispatcher_remove_view(app->view_dispatcher, ViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, ViewPopup);
    submenu_free(app->submenu);
    popup_free(app->popup);
    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);
    free(app);
}

/* a thread pressing buttons through the headless gui */
static int32_t presser(void* context) {
    Gui* gui = context;
    static const uint8_t keys[] = {0x10, 0x20, 0x20};
    for(size_t i = 0; i < sizeof keys; i++) {
        furi_delay_ms(5);
        gui_buttons_event(gui, keys[i], 1, furi_get_tick());
        gui_buttons_event(gui, keys[i], 0, furi_get_tick());
    }
    return 0;
}

static void test_view_dispatcher(void) {
    Gui* gui = gui_alloc_headless();
    App* app = app_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, gui, ViewDispatcherTypeFullscreen);
    scene_manager_next_scene(app->scene_manager, SceneMenu);
    gui_redraw(gui);
    compare("dispatcher_menu", canvas_get_buffer(gui->canvas));

    FuriThread* t = furi_thread_alloc_ex("presser", 2048, presser, gui);
    furi_thread_start(t);
    /* OK: custom event 7 -> popup scene; Back: back to the menu; Back:
     * no previous scene -> navigation returns false -> stop */
    view_dispatcher_run(app->view_dispatcher);
    furi_thread_join(t);
    furi_thread_free(t);
    CHECK(app->entered[ScenePopup] == 1 && app->entered[SceneMenu] == 2,
          "dispatcher: menu -> popup -> back to menu");
    app_free(app);
}

/* ---- the gui thread on a pipe ---- */

static int lcd_writes;
static void count_commit(uint8_t* data, size_t size, CanvasOrientation o, void* context) {
    UNUSED(data);
    UNUSED(size);
    UNUSED(o);
    UNUSED(context);
    lcd_writes++;
}

static int btn_pipe[2];
static int32_t pipe_presser(void* context) {
    UNUSED(context);
    static const uint8_t ev[][2] = {{0x02, 1}, {0x02, 0}, {0x10, 1}, {0x10, 0}};
    furi_delay_ms(30);
    for(size_t i = 0; i < 4; i++) {
        if(write(btn_pipe[1], ev[i], 2) != 2) return 1;
        furi_delay_ms(20);
    }
    return 0;
}

static bool quit_custom(void* context, uint32_t event) {
    App* app = context;
    if(event == 8) view_dispatcher_stop(app->view_dispatcher);
    return true;
}

static void test_gui_thread(void) {
    char path[32];
    uint8_t lcd[1024];

    if(pipe(btn_pipe) != 0) {
        CHECK(0, "pipe");
        return;
    }
    snprintf(path, sizeof path, "/dev/fd/%d", btn_pipe[0]);
    setenv("FURI_BUTTONS", path, 1);
    setenv("FURI_LCD", "build/lcd.bin", 1);
    unlink("build/lcd.bin");
    close(open("build/lcd.bin", O_WRONLY | O_CREAT, 0644));

    Gui* gui = furi_record_open(RECORD_GUI);
    CHECK(gui && gui->buttons_fd >= 0 && gui->lcd_fd >= 0, "gui service started");
    gui_add_framebuffer_callback(gui, count_commit, NULL);
    App* app = app_alloc();
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, quit_custom);
    view_dispatcher_attach_to_gui(app->view_dispatcher, gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewMenu);

    FuriThread* t = furi_thread_alloc_ex("pipe", 2048, pipe_presser, NULL);
    furi_thread_start(t);
    /* Down, then OK on "Quit": custom event 8 stops the dispatcher */
    view_dispatcher_run(app->view_dispatcher);
    furi_thread_join(t);
    CHECK(furi_thread_get_return_code(t) == 0, "pipe writes");
    furi_thread_free(t);

    int writes = lcd_writes;
    furi_delay_ms(300); /* idle: no redraws */
    CHECK(lcd_writes == writes, "gui: no redraw while nothing changes");
    CHECK(writes >= 2 && writes <= 6, "gui: a few redraws (switch, Down, OK)");
    printf("gui thread: %d frames, stack unused %u of %u bytes\n",
           writes,
           (unsigned)furi_thread_get_stack_space(furi_thread_get_id(gui->thread)),
           GUI_THREAD_STACK);

    int fd = open("build/lcd.bin", O_RDONLY);
    CHECK(fd >= 0 && read(fd, lcd, sizeof lcd) == sizeof lcd, "lcd file has a frame");
    if(fd >= 0) close(fd);
    compare("gui_thread_lcd", lcd);

    gui_remove_framebuffer_callback(gui, count_commit, NULL);
    app_free(app);
    furi_record_close(RECORD_GUI);
}

/* ---- RAM ---- */

static void report_ram(void) {
    printf(
        "RAM (host sizes; the target's are smaller, 4-byte pointers): Gui %zu + Canvas %zu, "
        "ViewPort %zu, ViewDispatcher %zu, View %zu\n",
        sizeof(Gui),
        sizeof(Canvas),
        sizeof(ViewPort),
        sizeof(ViewDispatcher),
        sizeof(View));
}

int main(int argc, char** argv) {
    int i = 1;
    if(i < argc && strcmp(argv[i], "-w") == 0) {
        write_refs = true;
        i++;
    }
    ref_dir = i < argc ? argv[i] : "tests/gui";
    canvas = canvas_init();

    test_canvas();
    test_submenu();
    test_popup();
    test_dialog_ex();
    test_loading();
    test_text_input();
    test_byte_input();
    test_variable_item_list();
    test_widget();
    test_gui_window_and_input();
    test_view_dispatcher();
    test_gui_thread();
    report_ram();
    canvas_free(canvas);
    printf("test_gui: %d checks, %s\n", checks, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
