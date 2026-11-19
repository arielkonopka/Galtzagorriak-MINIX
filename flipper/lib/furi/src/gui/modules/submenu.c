/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri GUI (spec R5.2): from the stock firmware
 * applications/services/gui/modules/submenu.c (GPL-3.0). */
#include "submenu.h"

#include <gui/elements.h>
#include <furi.h>
#include "../m-array.h"
#include "pine.h"

struct Submenu {
    View* view;
};

typedef struct {
    FuriString* label;
    uint32_t index;
    union {
        SubmenuItemCallback callback;
        SubmenuItemCallbackEx callback_ex;
    };
    void* callback_context;
    bool has_extended_events;
} SubmenuItem;

static void SubmenuItem_init(SubmenuItem* item) {
    item->label = furi_string_alloc();
    item->index = 0;
    item->callback = NULL;
    item->callback_context = NULL;
}

static void SubmenuItem_clear(SubmenuItem* item) {
    furi_string_free(item->label);
}

/* libfuri: plain array; the labels are freed here (m-array.h) */
ARRAY_DEF(SubmenuItemArray, SubmenuItem, M_POD_OPLIST)

static SubmenuItem* SubmenuItemArray_push_item(SubmenuItemArray_t items) {
    SubmenuItem* item = SubmenuItemArray_push_new(items);
    SubmenuItem_init(item);
    return item;
}

static void SubmenuItemArray_clear_items(SubmenuItemArray_t items) {
    for(size_t i = 0; i < SubmenuItemArray_size(items); i++)
        SubmenuItem_clear(SubmenuItemArray_get(items, i));
    SubmenuItemArray_reset(items);
}

typedef struct {
    SubmenuItemArray_t items;
    FuriString* header;
    size_t position;
    size_t window_position;
} SubmenuModel;

static void submenu_process_up(Submenu* submenu);
static void submenu_process_down(Submenu* submenu);
static void submenu_process_ok(Submenu* submenu, InputType input_type);

/* Pine style (spec K.2, R5.6a.7): the header is the title, the position
 * on its right; 8 rows (7 in a window), the scroll mark, the hints. */
static void submenu_draw_pine(Canvas* canvas, SubmenuModel* model) {
    const size_t n = SubmenuItemArray_size(model->items), vis = pine_rows(canvas);
    char pos[12];

    canvas_clear(canvas);
    pine_title(
        canvas,
        furi_string_empty(model->header) ? NULL : furi_string_get_cstr(model->header),
        n > 0 ? pine_pos(pos, model->position + 1, n) : NULL);
    model->window_position = pine_scroll_top(model->window_position, model->position, vis, n);
    for(size_t i = 0; i < vis && model->window_position + i < n; i++) {
        const size_t p = model->window_position + i;
        pine_row(
            canvas,
            i,
            furi_string_get_cstr(SubmenuItemArray_cget(model->items, p)->label),
            NULL,
            p == model->position,
            true);
    }
    pine_scrollbar(canvas, model->window_position, vis, n, model->position);
    pine_hints(canvas, n > 0 ? "OK Select  Back Done" : "Back Done");
}

/* the rows the stock navigation code scrolls by */
static size_t submenu_items_on_screen(const SubmenuModel* model) {
    if(pine_on()) return PINE_ANY;
    return furi_string_empty(model->header) ? 4 : 3;
}

static void submenu_view_draw_callback(Canvas* canvas, void* _model) {
    SubmenuModel* model = _model;

    if(pine_on()) {
        submenu_draw_pine(canvas, model);
        return;
    }

    const uint8_t item_height = 16;
    uint8_t item_width = canvas_width(canvas) - 5;

    canvas_clear(canvas);

    if(!furi_string_empty(model->header)) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 4, 11, furi_string_get_cstr(model->header));
    }

    canvas_set_font(canvas, FontSecondary);

    size_t position = 0;
    SubmenuItemArray_it_t it;
    for(SubmenuItemArray_it(it, model->items); !SubmenuItemArray_end_p(it);
        SubmenuItemArray_next(it)) {
        const size_t item_position = position - model->window_position;
        const size_t items_on_screen = furi_string_empty(model->header) ? 4 : 3;
        uint8_t y_offset = furi_string_empty(model->header) ? 0 : 16;

        if(item_position < items_on_screen) {
            if(position == model->position) {
                canvas_set_color(canvas, ColorBlack);
                elements_slightly_rounded_box(
                    canvas,
                    0,
                    y_offset + (item_position * item_height) + 1,
                    item_width,
                    item_height - 2);
                canvas_set_color(canvas, ColorWhite);
            } else {
                canvas_set_color(canvas, ColorBlack);
            }

            FuriString* disp_str;
            disp_str = furi_string_alloc_set(SubmenuItemArray_cref(it)->label);
            elements_string_fit_width(canvas, disp_str, item_width - (6 * 2));

            canvas_draw_str(
                canvas,
                6,
                y_offset + (item_position * item_height) + item_height - 4,
                furi_string_get_cstr(disp_str));

            furi_string_free(disp_str);
        }

        position++;
    }

    elements_scrollbar(canvas, model->position, SubmenuItemArray_size(model->items));
}

static bool submenu_view_input_callback(InputEvent* event, void* context) {
    Submenu* submenu = context;
    furi_assert(submenu);
    bool consumed = false;

    if(event->key == InputKeyOk) {
        consumed = true;
        submenu_process_ok(submenu, event->type);
    } else if(event->type == InputTypeShort) {
        switch(event->key) {
        case InputKeyUp:
            consumed = true;
            submenu_process_up(submenu);
            break;
        case InputKeyDown:
            consumed = true;
            submenu_process_down(submenu);
            break;
        default:
            break;
        }
    } else if(event->type == InputTypeRepeat) {
        if(event->key == InputKeyUp) {
            consumed = true;
            submenu_process_up(submenu);
        } else if(event->key == InputKeyDown) {
            consumed = true;
            submenu_process_down(submenu);
        }
    }

    return consumed;
}

Submenu* submenu_alloc(void) {
    Submenu* submenu = malloc(sizeof(Submenu));
    submenu->view = view_alloc();
    view_set_context(submenu->view, submenu);
    view_allocate_model(submenu->view, ViewModelTypeLocking, sizeof(SubmenuModel));
    view_set_draw_callback(submenu->view, submenu_view_draw_callback);
    view_set_input_callback(submenu->view, submenu_view_input_callback);

    with_view_model(
        submenu->view,
        SubmenuModel * model,
        {
            SubmenuItemArray_init(model->items);
            model->position = 0;
            model->window_position = 0;
            model->header = furi_string_alloc();
        },
        true);

    return submenu;
}

void submenu_free(Submenu* submenu) {
    furi_check(submenu);

    with_view_model(
        submenu->view,
        SubmenuModel * model,
        {
            furi_string_free(model->header);
            SubmenuItemArray_clear_items(model->items);
            SubmenuItemArray_clear(model->items);
        },
        true);
    view_free(submenu->view);
    free(submenu);
}

View* submenu_get_view(Submenu* submenu) {
    furi_check(submenu);
    return submenu->view;
}

void submenu_add_item(
    Submenu* submenu,
    const char* label,
    uint32_t index,
    SubmenuItemCallback callback,
    void* callback_context) {
    SubmenuItem* item = NULL;
    furi_check(label);
    furi_check(submenu);

    with_view_model(
        submenu->view,
        SubmenuModel * model,
        {
            item = SubmenuItemArray_push_item(model->items);
            furi_string_set_str(item->label, label);
            item->index = index;
            item->callback = callback;
            item->callback_context = callback_context;
            item->has_extended_events = false;
        },
        true);
}

void submenu_add_item_ex(
    Submenu* submenu,
    const char* label,
    uint32_t index,
    SubmenuItemCallbackEx callback,
    void* callback_context) {
    SubmenuItem* item = NULL;
    furi_check(label);
    furi_check(submenu);

    with_view_model(
        submenu->view,
        SubmenuModel * model,
        {
            item = SubmenuItemArray_push_item(model->items);
            furi_string_set_str(item->label, label);
            item->index = index;
            item->callback_ex = callback;
            item->callback_context = callback_context;
            item->has_extended_events = true;
        },
        true);
}

void submenu_change_item_label(Submenu* submenu, uint32_t index, const char* label) {
    furi_check(submenu);
    furi_check(label);

    with_view_model(
        submenu->view,
        SubmenuModel * model,
        {
            SubmenuItemArray_it_t it;
            for(SubmenuItemArray_it(it, model->items); !SubmenuItemArray_end_p(it);
                SubmenuItemArray_next(it)) {
                if(index == SubmenuItemArray_cref(it)->index) {
                    furi_string_set_str(SubmenuItemArray_cref(it)->label, label);
                    break;
                }
            }
        },
        true);
}

void submenu_remove_item(Submenu* submenu, uint32_t index) {
    furi_check(submenu);

    with_view_model(
        submenu->view,
        SubmenuModel * model,
        {
            SubmenuItemArray_it_t it;
            for(SubmenuItemArray_it(it, model->items); !SubmenuItemArray_end_p(it);
                SubmenuItemArray_next(it)) {
                if(index == SubmenuItemArray_cref(it)->index) {
                    SubmenuItem_clear(SubmenuItemArray_ref(it));
                    SubmenuItemArray_remove(model->items, it);
                    break;
                }
            }
        },
        true);
}

void submenu_reset(Submenu* submenu) {
    furi_check(submenu);

    with_view_model(
        submenu->view,
        SubmenuModel * model,
        {
            SubmenuItemArray_clear_items(model->items);
            model->position = 0;
            model->window_position = 0;
            furi_string_reset(model->header);
        },
        true);
}

uint32_t submenu_get_selected_item(Submenu* submenu) {
    furi_check(submenu);

    uint32_t selected_item_index = 0;

    with_view_model(
        submenu->view,
        SubmenuModel * model,
        {
            if(model->position < SubmenuItemArray_size(model->items)) {
                const SubmenuItem* item = SubmenuItemArray_cget(model->items, model->position);
                selected_item_index = item->index;
            }
        },
        false);

    return selected_item_index;
}

void submenu_set_selected_item(Submenu* submenu, uint32_t index) {
    furi_check(submenu);
    with_view_model(
        submenu->view,
        SubmenuModel * model,
        {
            size_t position = 0;
            SubmenuItemArray_it_t it;
            for(SubmenuItemArray_it(it, model->items); !SubmenuItemArray_end_p(it);
                SubmenuItemArray_next(it)) {
                if(index == SubmenuItemArray_cref(it)->index) {
                    break;
                }
                position++;
            }

            const size_t items_size = SubmenuItemArray_size(model->items);

            if(position >= items_size) {
                position = 0;
            }

            model->position = position;
            model->window_position = position;

            if(model->window_position > 0) {
                model->window_position -= 1;
            }

            const size_t items_on_screen = submenu_items_on_screen(model);

            if(items_size <= items_on_screen) {
                model->window_position = 0;
            } else {
                const size_t pos = items_size - items_on_screen;
                if(model->window_position > pos) {
                    model->window_position = pos;
                }
            }
        },
        true);
}

void submenu_process_up(Submenu* submenu) {
    with_view_model(
        submenu->view,
        SubmenuModel * model,
        {
            const size_t items_on_screen = submenu_items_on_screen(model);
            const size_t items_size = SubmenuItemArray_size(model->items);

            if(model->position > 0) {
                model->position--;
                if((model->position == model->window_position) && (model->window_position > 0)) {
                    model->window_position--;
                }
            } else {
                model->position = items_size - 1;
                if(model->position > items_on_screen - 1) {
                    model->window_position = model->position - (items_on_screen - 1);
                }
            }
        },
        true);
}

void submenu_process_down(Submenu* submenu) {
    with_view_model(
        submenu->view,
        SubmenuModel * model,
        {
            const size_t items_on_screen = submenu_items_on_screen(model);
            const size_t items_size = SubmenuItemArray_size(model->items);

            if(model->position < items_size - 1) {
                model->position++;
                if((model->position - model->window_position > items_on_screen - 2) &&
                   (model->window_position < items_size - items_on_screen)) {
                    model->window_position++;
                }
            } else {
                model->position = 0;
                model->window_position = 0;
            }
        },
        true);
}

void submenu_process_ok(Submenu* submenu, InputType input_type) {
    SubmenuItem* item = NULL;

    with_view_model(
        submenu->view,
        SubmenuModel * model,
        {
            const size_t items_size = SubmenuItemArray_size(model->items);
            if(model->position < items_size) {
                item = SubmenuItemArray_get(model->items, model->position);
            }
        },
        true);

    if(!item) return;

    if(!item->has_extended_events && input_type == InputTypeShort && item->callback) {
        item->callback(item->callback_context, item->index);
    } else if(item->has_extended_events && item->callback_ex) {
        item->callback_ex(item->callback_context, input_type, item->index);
    }
}

void submenu_set_header(Submenu* submenu, const char* header) {
    furi_check(submenu);

    with_view_model(
        submenu->view,
        SubmenuModel * model,
        {
            if(header == NULL) {
                furi_string_reset(model->header);
            } else {
                furi_string_set_str(model->header, header);
            }
        },
        true);
}
