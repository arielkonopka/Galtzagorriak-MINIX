/* SPDX-License-Identifier: GPL-3.0-or-later
 * launcher_stock - the launcher's "Stock" style (spec R5.6b), a skin: it
 * only draws, with the gui.c primitives and the two stock fonts
 * (gui_text_font).  The launcher (launcher.c) keeps the pages, keys,
 * /etc/apps and settings in both styles (R5.6b.1).
 *
 * Ported per D.3 from the stock Flipper Zero firmware (GPL-3.0, commit
 * a8a24de3, the one libfuri follows):
 *   stock_menu     applications/services/gui/modules/menu.c,
 *                  menu_draw_callback()
 *   stock_submenu  applications/services/gui/modules/submenu.c,
 *                  submenu_view_draw_callback()
 *   the frame, scroll bar and rounded box: gui/elements.c
 *                  (elements_frame, elements_scrollbar,
 *                  elements_slightly_rounded_box)
 * Only the drawing is taken: no View, no model, no allocation.  The stock
 * menu draws a 14x14 icon at (4, y); the launcher's icons are 10x10
 * (R5.6b.3), drawn in the middle of that cell, and an 8x6 one in the
 * middle of the 10x10.  flipper/tests/host/stocktest.c renders the ported
 * menu.c (libfuri) and this file from the same entries and compares them.
 */
#include <string.h>
#include "gui.h"
#include "launcher_stock.h"
#include "appline.h"

/* elements_frame(): the selected entry's frame */
static void frame(int x, int y, int w, int h)
{
	gui_hline(x + 2, x + w - 2, y, 1);
	gui_hline(x + 1, x + w, y + h - 1, 1);
	gui_hline(x + 2, x + w - 1, y + h, 1);
	gui_vline(x, y + 2, y + h - 2, 1);
	gui_vline(x + w - 1, y + 1, y + h - 2, 1);
	gui_vline(x + w, y + 2, y + h - 2, 1);
	gui_pixel(x + 1, y + 1, 1);
}

/* elements_scrollbar(): the dotted line and the block at the right */
static void scrollbar(int pos, int total)
{
	int i, h;

	gui_fill(GUI_W - 3, 0, GUI_W - 1, GUI_H - 1, 0);
	for (i = 0; i < GUI_H; i += 2) gui_pixel(GUI_W - 2, i, 1);
	if (total > 0) {
		h = GUI_H / total;
		if (h < 1) h = 1;
		gui_fill(GUI_W - 3, GUI_H * pos / total, GUI_W - 1,
			 GUI_H * pos / total + h - 1, 1);
	}
}

/* elements_slightly_rounded_box(): a box without its corner pixels */
static void rbox(int x, int y, int w, int h, int on)
{
	gui_fill(x + 1, y, x + w - 2, y + h - 1, on);
	gui_vline(x, y + 1, y + h - 2, on);
	gui_vline(x + w - 1, y + 1, y + h - 2, on);
}

/* An icon in the 14x14 cell at x, y (the 10x10 in its middle). */
static void icon(int x, int y, const struct stock_row *r)
{
	int gx, gy;

	if (r->icon10 != NULL) {
		for (gy = 0; gy < 10; gy++)
			for (gx = 0; gx < 10; gx++)
				if (r->icon10[2 * gy + (gx >> 3)] & 0x80 >> (gx & 7))
					gui_pixel(x + 2 + gx, y + 2 + gy, 1);
	} else if (r->icon6 != NULL) {
		for (gy = 0; gy < ICON_H; gy++)
			for (gx = 0; gx < 8; gx++)
				if (r->icon6[gy] & 0x80 >> gx)
					gui_pixel(x + 3 + gx, y + 4 + gy, 1);
	}
}

void stock_menu(int n, int pos, stock_row_fn row)
{
	struct stock_row r;
	int line;

	gui_clear();
	if (n == 0) {
		gui_text_font(2, 32, GUI_FONT_SECONDARY, "Empty", 1);
		scrollbar(0, 0);
		return;
	}
	/* the entry before, the selected one, the one after (wrapping) */
	for (line = 0; line < 3; line++) {
		row((line + pos + n - 1) % n, &r);
		icon(4, 3 + 22 * line, &r);
		gui_text_font(22, 14 + 22 * line, line == 1 ? GUI_FONT_PRIMARY
				: GUI_FONT_SECONDARY, r.label, 1);
	}
	frame(0, 21, GUI_W - 5, 21);
	scrollbar(pos, n);
}

/* elements_string_fit_width(): cut to w pixels, with "..." */
static void fit(char *s, int w)
{
	int n = (int) strlen(s);

	if (gui_font_width(GUI_FONT_SECONDARY, s) <= w) return;
	w -= gui_font_width(GUI_FONT_SECONDARY, "...");
	do s[--n] = 0;
	while (n > 0 && gui_font_width(GUI_FONT_SECONDARY, s) > w);
	strcpy(s + n, "...");
}

void stock_submenu(const char *header, int n, int pos, stock_row_fn row)
{
	struct stock_row r;
	char t[40];
	int i, y, w = GUI_W - 5, top = pos > 0 ? pos - 1 : 0;

	if (n <= STOCK_SUB_ROWS) top = 0;
	else if (top > n - STOCK_SUB_ROWS) top = n - STOCK_SUB_ROWS;

	gui_clear();
	gui_text_font(4, 11, GUI_FONT_PRIMARY, header, 1);
	for (i = 0; i < STOCK_SUB_ROWS && top + i < n; i++) {
		y = 16 + i * 16;
		row(top + i, &r);
		if (top + i == pos) rbox(0, y + 1, w, 14, 1);
		strncpy(t, r.label, sizeof t - 4);
		t[sizeof t - 4] = 0;
		fit(t, w - 12);
		gui_text_font(6, y + 12, GUI_FONT_SECONDARY, t, top + i != pos);
	}
	scrollbar(pos, n);
}

void stock_popup(const char *msg)
{
	int w = gui_font_width(GUI_FONT_SECONDARY, msg) + 12;

	if (w > GUI_W - 8) w = GUI_W - 8;
	rbox((GUI_W - w) / 2 - 1, 21, w + 2, 22, 1);	/* the frame */
	rbox((GUI_W - w) / 2, 22, w, 20, 0);
	gui_text_font((GUI_W - w) / 2 + 6, 35, GUI_FONT_SECONDARY, msg, 1);
}

/* The wizard's sample: three built-in icons of R5.6a.2. */
static const char sample_name[3][8] = { "subghz", "rfid", "nfc" };
static const char sample_label[3][8] = { "Sub-GHz", "125 kHz", "NFC" };

static void sample_row(int i, struct stock_row *r)
{
	char f[10];

	f[0] = '@';
	strcpy(f + 1, sample_name[i]);
	r->label = sample_label[i];
	r->icon6 = NULL;
	r->icon10 = icon_parse(f, r->rows, gui_glyph_row)
			? icon10_of(r->rows) : NULL;
}

void stock_sample(int stock)
{
	struct stock_row r;
	unsigned char rows[ICON_H];
	char f[10];
	int i, y;

	if (stock) {
		stock_menu(3, 1, sample_row);
		return;
	}
	gui_clear();				/* Pine (R5.6a.7) */
	gui_title("APPS");
	for (i = 0; i < 3; i++) {
		y = 7 + i * 6;
		sample_row(i, &r);
		f[0] = '@';
		strcpy(f + 1, sample_name[i]);
		if (i == 0) gui_fill(0, y - 1, GUI_W - 1, y + 5, 1);
		if (icon_parse(f, rows, gui_glyph_row))
			gui_bits8(6, y, rows, ICON_H, i == 0);
		gui_text(16, y, r.label, i == 0);
		if (i == 0) gui_text(1, y, ">", 1);
	}
}
