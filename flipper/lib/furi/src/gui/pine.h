/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri GUI: the "Pine meets modern UX" screen style (spec K.2,
 * R5.6a.7, task T22b) for the standard modules (Submenu,
 * VariableItemList, Popup, DialogEx, TextInput).
 *
 * The parts are those of the launcher (flipper/gui/launcher.c), in the
 * same 4x6 font: a black title bar with the screen's name and a short
 * state on the right, rows of 6 pixels with a full-width inverted
 * selection bar, values in brackets, a 1-pixel scroll mark at x = 127,
 * and the hint line (a rule at y = 56, the hints at y = 58).
 *
 * Coordinates are those of the canvas frame. A view in the fullscreen
 * layer draws its own 7-pixel title bar at the top of the frame; a view
 * in the window layer sits under the Gui's 11-pixel title bar (frame
 * y = 13): the module paints its name and state on that bar, and its
 * content starts at frame y = 0. The window frame keeps its height
 * (51), so custom views and the stock elements_button_* are unchanged.
 *
 * The style: Pine unless the environment has FURI_STYLE=stock (the
 * launcher's "Style: stock", R5.6b / T31, exports it to the apps it
 * starts); then the modules draw as the stock firmware does. No state
 * in RAM: the switch is read from the environment at each draw. */
#pragma once

#include "canvas_i.h"
#include <stdbool.h>
#include <stddef.h>

#define PINE_ROW_H 6
#define PINE_ANY   64 /* "rows on screen" for the stock navigation code
                       * in Pine style: never scroll there, the draw
                       * callback keeps the selection visible */

/* The strings of the Pine parts, in one const table in pine.c: in the
 * shared library every string literal costs a GOT word of RAM in each
 * app, the table costs one. */
typedef enum {
    PineSelectDone, /* "OK Select  Back Done" */
    PineDone, /* "Back Done" */
    PineChange, /* "<>Change  " */
    PineSelect, /* "OK Select  " */
    PineAnyKeyClose, /* "Any key Close" */
    PineBackClose, /* "Back Close" */
    PineKeyLeft, /* "<" */
    PineKeyOk, /* "OK " */
    PineKeyRight, /* ">" */
    PineSave, /* "OK Save  Back Cancel" */
    PineDelete, /* "OK Delete  Back Cancel" */
    PineType, /* "OK Type  Back Cancel" */
    PinePrompt, /* "> " */
    PineDots, /* ".." */
    PineCursor, /* "_" */
    PineEnv, /* "FURI_STYLE" */
    PineStock, /* "stock" */
    PineOpen, /* "[" */
    PineClose, /* "]" */
    PineEmpty, /* "" */
} PineStr;

const char* pine_str(PineStr id);

/* true: draw the modules in Pine style */
bool pine_on(void);
/* text in the 4x6 font at frame (x, y); inverse: white on black.
 * Returns the width in pixels. */
int pine_text(Canvas* canvas, int x, int y, const char* s, bool inverse);
/* the y of the first content row's text */
int pine_top(Canvas* canvas);
/* how many rows fit between the top and the hint line */
size_t pine_rows(Canvas* canvas);
/* the y of row i's text */
int pine_row_y(Canvas* canvas, size_t i);
/* the title bar: name (NULL: the Gui's own or the appid) and a state of
 * at most 6 characters on the right (NULL: none) */
void pine_title(Canvas* canvas, const char* name, const char* state);
/* "pos/n" into buf (>= 12 bytes); returns buf */
char* pine_pos(char* buf, size_t pos, size_t n);
/* the first visible row so that sel is shown: top kept if it can be */
size_t pine_scroll_top(size_t top, size_t sel, size_t vis, size_t n);
/* a list row: the bar if selected, ">" at x = 1 if mark, the label at
 * x = 6, the value right-aligned in brackets (NULL: none) */
void pine_row(
    Canvas* canvas,
    size_t i,
    const char* label,
    const char* value,
    bool selected,
    bool mark);
/* the scroll mark at x = 127 (only if n > vis) */
void pine_scrollbar(Canvas* canvas, size_t top, size_t vis, size_t n, size_t sel);
/* the hint line: the rule and the hints */
void pine_hints(Canvas* canvas, const char* hints);
/* multi-line text ('\n') in the 4x6 font from (x, y) up to the hint
 * line; returns the y after the last line */
int pine_lines(Canvas* canvas, int x, int y, const char* s);
/* the body of a Popup or DialogEx: the header as the title, the text in
 * lines from the top, the icon at its x and at its y if it fits between
 * the title and the hint line (moved up if not; left out if too tall) */
void pine_body(
    Canvas* canvas,
    const char* header,
    const char* text,
    const Icon* icon,
    int icon_x,
    int icon_y);
