/* ui - list, message and keyboard screens for the ported stock apps
 * (infrared, archive; spec Phase 7).  The look of the other GUI apps
 * (lfrfid.c): an inverse title bar, five rows, the selected row inverse,
 * a scroll mark on the right.  A list asks for its items one row at a
 * time (ui_item_fn), so a directory or a remote file is never held whole
 * in RAM: the caller reads the rows on screen from the file each time.
 * All on the stack: no .bss.
 */
#include <string.h>
#include <stdio.h>
#include "gui.h"
#include "ui.h"

int ui_list(const char *title, ui_item_fn item, void *ctx, int n, int sel,
	int right, int *key)
{
	char buf[UI_ITEM];
	int top, i, k;

	if (n <= 0) return -1;
	if (sel < 0 || sel >= n) sel = 0;
	top = sel - sel % UI_ROWS;
	for (;;) {
		gui_clear();
		gui_title(title);
		for (i = 0; i < UI_ROWS && top + i < n; i++) {
			if (item(ctx, top + i, buf) < 0) strcpy(buf, "?");
			buf[UI_ITEM - 1] = 0;
			gui_text(6, 14 + i * 10, buf, top + i == sel);
		}
		if (n > UI_ROWS) {		/* where we are: a bar on the right */
			int h = 50 * UI_ROWS / n, y = 12 + 50 * top / n;

			if (h < 3) h = 3;
			gui_vline(126, y, y + h - 1, 1);
		}
		gui_flush();
		k = gui_key() & ~KEY_LONG;
		if (k == KEY_BACK) { if (key) *key = KEY_BACK; return -1; }
		if (k == KEY_UP) sel = (sel + n - 1) % n;
		if (k == KEY_DOWN) sel = (sel + 1) % n;
		if (k == KEY_LEFT) sel = sel >= UI_ROWS ? sel - UI_ROWS : 0;
		if (k == KEY_RIGHT && !right)
			sel = sel + UI_ROWS < n ? sel + UI_ROWS : n - 1;
		if (k == KEY_OK || (k == KEY_RIGHT && right)) {
			if (key) *key = k;
			return sel;
		}
		top = sel - sel % UI_ROWS;
	}
}

struct fixed { const char *const *items; };

static int fixed_item(void *ctx, int i, char *buf)
{
	strncpy(buf, ((struct fixed *) ctx)->items[i], UI_ITEM - 1);
	buf[UI_ITEM - 1] = 0;
	return 0;
}

int ui_menu(const char *title, const char *const *items, int n)
{
	struct fixed f;

	f.items = items;
	return ui_list(title, fixed_item, &f, n, 0, 0, NULL);
}

void ui_msg(const char *l1, const char *l2)
{
	gui_clear();
	gui_center(22, l1, 0);
	if (l2) gui_center(36, l2, 0);
	gui_flush();
}

void ui_note(const char *l1, const char *l2)
{
	ui_msg(l1, l2);
	gui_key();
}

/* As lfrfid's keyboard: three rows of ten characters (the current case),
 * then Shift (lower, UPPER, digits and symbols), Bksp, Del, Ret. */
#define KB_COLS		10
#define KB_FKEYS	4
int ui_text(const char *title, char *buf, int max)
{
	static const char *const cs[3] = {	/* exactly 30 each */
		"abcdefghijklmnopqrstuvwxyz .-_",
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ .-_",
		"0123456789@#$%&()+=,.!?*-_ :;~"
	};
	static const char *const fn[KB_FKEYS] = { "Shift", "Bksp", "Del", "Ret" };
	int mode = 0, cx = 0, cy = 0, k, len = strlen(buf), r, c, w;

	for (;;) {
		gui_clear();
		gui_title(title);
		gui_text(2, 10, buf, 0);
		gui_vline(2 + len * 4, 8, 15, 1);
		for (r = 0; r < 3; r++)
			for (c = 0; c < KB_COLS; c++) {
				char s[2];

				s[0] = cs[mode][r * KB_COLS + c];
				s[1] = 0;
				gui_text(4 + c * 12, 22 + r * 10, s,
					r == cy && c == cx);
			}
		for (c = 0; c < KB_FKEYS; c++)
			gui_text(3 + c * 31, 54, fn[c], cy == 3 && c == cx);
		gui_flush();
		w = (cy < 3) ? KB_COLS : KB_FKEYS;
		k = gui_key() & ~KEY_LONG;
		if (k == KEY_BACK) return 0;
		if (k == KEY_UP) cy = (cy + 3) % 4;
		if (k == KEY_DOWN) cy = (cy + 1) % 4;
		if (k == KEY_LEFT) cx = (cx + w - 1) % w;
		if (k == KEY_RIGHT) cx = (cx + 1) % w;
		if (cy == 3) { if (cx >= KB_FKEYS) cx = KB_FKEYS - 1; }
		else if (cx >= KB_COLS) cx = KB_COLS - 1;
		if (k != KEY_OK) continue;
		if (cy < 3) {
			if (len < max - 1) {
				buf[len++] = cs[mode][cy * KB_COLS + cx];
				buf[len] = 0;
			}
		} else switch (cx) {
		case 0: mode = (mode + 1) % 3; break;
		case 1: if (len > 0) buf[--len] = 0; break;
		case 2: len = 0; buf[0] = 0; break;
		case 3: return len > 0;
		}
	}
}
