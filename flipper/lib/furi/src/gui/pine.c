/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri GUI: the Pine screen style of the standard modules (spec K.2,
 * R5.6a.7, T22b); see pine.h. Only stack and const data: no RAM per app.
 * The 4x6 font is flipper/hal/common/font4x6.c (public domain X11
 * misc-fixed), the one flipper/gui and the console use. */
#include "pine.h"
#include "gui_i.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "../../../../hal/common/font4x6.c"

/* in the order of PineStr */
static const char pine_strs[] = "OK Select  Back Done\0Back Done\0<>Change  \0OK Select  \0"
                                "Any key Close\0Back Close\0<\0OK \0>\0"
                                "OK Save  Back Cancel\0OK Delete  Back Cancel\0"
                                "OK Type  Back Cancel\0> \0..\0_\0FURI_STYLE\0stock\0[\0]\0";

const char* pine_str(PineStr id) {
    const char* p = pine_strs;
    for(int i = 0; i < (int)id; i++)
        p += strlen(p) + 1;
    return p;
}

bool pine_on(void) {
    const char* s = getenv(pine_str(PineEnv));
    return !(s && strcmp(s, pine_str(PineStock)) == 0);
}

/* a pixel at frame (x, y); the buffer is u8g2's vertical_top_lsb */
static void pine_px(Canvas* canvas, int x, int y, bool on) {
    x += (int)canvas->offset_x;
    y += (int)canvas->offset_y;
    if(x < 0 || x >= GUI_DISPLAY_WIDTH || y < 0 || y >= GUI_DISPLAY_HEIGHT) return;
    uint8_t* b = &canvas->buffer[(y >> 3) * GUI_DISPLAY_WIDTH + x];
    if(on)
        *b |= (uint8_t)(1U << (y & 7));
    else
        *b &= (uint8_t)~(1U << (y & 7));
}

static bool pine_get(Canvas* canvas, int x, int y) {
    return (canvas->buffer[(y >> 3) * GUI_DISPLAY_WIDTH + x] >> (y & 7)) & 1;
}

static void pine_fill(Canvas* canvas, int x0, int y0, int x1, int y1, bool on) {
    for(int y = y0; y <= y1; y++)
        for(int x = x0; x <= x1; x++)
            pine_px(canvas, x, y, on);
}

static int pine_text_n(Canvas* canvas, int x, int y, const char* s, size_t n, bool inverse, bool caps) {
    int x0 = x;
    for(; n > 0 && *s; s++, n--, x += FONT_W) {
        int c = (unsigned char)*s;
        if(caps) c = toupper(c);
        if(c < 32 || c > 126) c = '?';
        for(int gy = 0; gy < FONT_H; gy++) {
            int bits = font_4x6[c - 32][gy];
            if(inverse) bits = ~bits;
            for(int gx = 0; gx < FONT_W; gx++)
                pine_px(canvas, x + gx, y + gy, (bits & (0x80 >> gx)) != 0);
        }
    }
    return x - x0;
}

int pine_text(Canvas* canvas, int x, int y, const char* s, bool inverse) {
    return pine_text_n(canvas, x, y, s, (size_t)-1, inverse, false);
}

/* in the window layer: the Gui's title bar is above the frame */
static bool pine_window(Canvas* canvas) {
    return canvas->offset_y >= GUI_TITLE_BAR_HEIGHT;
}

int pine_top(Canvas* canvas) {
    return pine_window(canvas) ? 1 : FONT_H + 1;
}

size_t pine_rows(Canvas* canvas) {
    int rule = (int)canvas->height - 8, top = pine_top(canvas);
    return rule - 1 > top + FONT_H - 1 ? (size_t)((rule - top) / PINE_ROW_H) : 0;
}

int pine_row_y(Canvas* canvas, size_t i) {
    return pine_top(canvas) + (int)i * PINE_ROW_H;
}

char* pine_pos(char* buf, size_t pos, size_t n) {
    char* p = buf + 11;
    *p = 0;
    do
        *--p = (char)('0' + n % 10);
    while((n /= 10) > 0 && p > buf + 6);
    *--p = '/';
    do
        *--p = (char)('0' + pos % 10);
    while((pos /= 10) > 0 && p > buf);
    return p;
}

void pine_title(Canvas* canvas, const char* name, const char* state) {
    size_t ox = canvas->offset_x, oy = canvas->offset_y, w = canvas->width, h = canvas->height;
    bool window = pine_window(canvas);
    int bar_h = window ? GUI_TITLE_BAR_HEIGHT : FONT_H + 1;
    int sy = window ? (GUI_TITLE_BAR_HEIGHT - FONT_H + 1) / 2 : 1;

    canvas_frame_set(canvas, 0, window ? 0 : (int32_t)oy, GUI_DISPLAY_WIDTH, GUI_DISPLAY_HEIGHT);
    if(!name) {
        /* the title the Gui draws (the appid) */
        if(furi_record_exists(RECORD_GUI)) {
            Gui* gui = furi_record_open(RECORD_GUI);
            name = gui->title;
            furi_record_close(RECORD_GUI);
        }
        if(!name) name = pine_str(PineEmpty);
    }
    {
        pine_fill(canvas, 0, 0, GUI_DISPLAY_WIDTH - 1, bar_h - 1, true);
        if(window) {
            /* the Gui's bar, as gui_redraw_title() draws it */
            char title[24];
            size_t i;
            for(i = 0; i < sizeof title - 1 && name[i]; i++)
                title[i] = (char)toupper((unsigned char)name[i]);
            title[i] = 0;
            canvas_set_color(canvas, ColorWhite);
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, title);
            canvas_set_color(canvas, ColorBlack);
        } else {
            size_t n = strlen(name);
            if(n > GUI_DISPLAY_WIDTH / FONT_W) n = GUI_DISPLAY_WIDTH / FONT_W;
            pine_text_n(
                canvas, (GUI_DISPLAY_WIDTH - (int)n * FONT_W) / 2, 1, name, n, true, true);
        }
    }
    if(state && *state) {
        size_t n = strlen(state);
        if(n > 6) n = 6;
        int x = GUI_DISPLAY_WIDTH - 1 - (int)n * FONT_W, free = 1;
        /* only where the name left the bar black */
        for(int y = 0; y < bar_h && free; y++)
            for(int xx = x - 3; xx < GUI_DISPLAY_WIDTH && free; xx++)
                if(!pine_get(canvas, xx, y + (int)canvas->offset_y)) free = 0;
        if(free) pine_text_n(canvas, x, sy, state, n, true, false);
    }
    canvas_frame_set(canvas, (int32_t)ox, (int32_t)oy, w, h);
}

size_t pine_scroll_top(size_t top, size_t sel, size_t vis, size_t n) {
    if(vis == 0) return sel;
    if(sel < top) top = sel;
    if(sel >= top + vis) top = sel - vis + 1;
    if(n > vis && top > n - vis) top = n - vis;
    if(n <= vis) top = 0;
    return top;
}

void pine_row(
    Canvas* canvas,
    size_t i,
    const char* label,
    const char* value,
    bool selected,
    bool mark) {
    int y = pine_row_y(canvas, i), w = (int)canvas->width;
    int vx = w - 2, room;
    size_t vn = 0;

    if(selected) pine_fill(canvas, 0, y - 1, w - 1, y + FONT_H - 1, true);
    if(value) {
        vn = strlen(value);
        if(vn > 12) vn = 12;
        vx = w - 3 - (int)(vn + 2) * FONT_W;
        pine_text_n(canvas, vx, y, pine_str(PineOpen), 1, selected, false);
        pine_text_n(canvas, vx + FONT_W, y, value, vn, selected, false);
        pine_text_n(canvas, vx + FONT_W * (int)(vn + 1), y, pine_str(PineClose), 1, selected, false);
    }
    room = (vx - FONT_W - 6) / FONT_W;
    if(room > 0) pine_text_n(canvas, 6, y, label, (size_t)room, selected, false);
    if(selected && mark) pine_text_n(canvas, 1, y, pine_str(PineKeyRight), 1, true, false);
}

void pine_scrollbar(Canvas* canvas, size_t top, size_t vis, size_t n, size_t sel) {
    if(n <= vis || vis == 0) return;
    int y0 = pine_top(canvas) - 1, h = (int)vis * PINE_ROW_H;
    int len = h * (int)vis / (int)n, off = h * (int)top / (int)n;
    int sy = pine_row_y(canvas, sel - top) - 1, x = (int)canvas->width - 1;
    if(len < 2) len = 2;
    for(int y = y0 + off; y < y0 + off + len && y < y0 + h; y++)
        pine_px(canvas, x, y, y < sy || y > sy + FONT_H); /* inverted on the bar */
}

void pine_hints(Canvas* canvas, const char* hints) {
    int rule = (int)canvas->height - 8, w = (int)canvas->width;
    pine_fill(canvas, 0, rule, w - 1, (int)canvas->height - 1, false);
    pine_fill(canvas, 0, rule, w - 1, rule, true);
    if(hints) pine_text(canvas, 1, rule + 2, hints, false);
}

int pine_lines(Canvas* canvas, int x, int y, const char* s) {
    int last = (int)canvas->height - 8 - FONT_H;
    while(s && *s && y <= last) {
        const char* e = strchr(s, '\n');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        pine_text_n(canvas, x, y, s, n, false, false);
        y += PINE_ROW_H;
        s = e ? e + 1 : NULL;
    }
    return y;
}

void pine_body(
    Canvas* canvas,
    const char* header,
    const char* text,
    const Icon* icon,
    int icon_x,
    int icon_y) {
    int top = pine_top(canvas), rule = (int)canvas->height - 8;

    canvas_clear(canvas);
    pine_title(canvas, header && *header ? header : NULL, NULL);
    if(icon) {
        int h = icon_get_height(icon);
        if(icon_y + h > rule - 1) icon_y = rule - 1 - h;
        if(icon_y < top) icon_y = top;
        if(icon_y + h <= rule) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_icon(canvas, icon_x, icon_y, icon);
        }
    }
    pine_lines(canvas, 2, top + 2, text);
}
