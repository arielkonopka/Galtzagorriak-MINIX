/* SPDX-License-Identifier: GPL-3.0-or-later
 * stocktest - the launcher's Stock style (spec R5.6b, AC5.7) on the PC.
 *
 * The same entry list is drawn twice: by the stock menu.c as ported into
 * libfuri (its View on a libfuri Canvas) and by the launcher's
 * launcher_stock.c (gui.c's frame buffer), at several positions; the
 * frames must be byte-identical.  Likewise the stock Submenu against
 * stock_submenu.  Also the "##" icon form (appline.c) and the 10x10
 * built-in icons.  A mismatch prints both frames.
 *
 *	stocktest [-d]		-d: print every frame (for the manual)
 */
#include <furi.h>
#include <gui/canvas.h>
#include <gui/view.h>
#include <gui/modules/menu.h>
#include <gui/modules/submenu.h>
#include "src/gui/view_i.h"
#include "src/gui/canvas_i.h"
#include "icon_i.h"

#define GUI_HOST_U8G2
#ifndef CLK_TCK
#define CLK_TCK 100
#endif
#define font_4x6 gui_font_4x6	/* libfuri's pine.c has one too (T22b) */
#include "../../gui/gui.c"		/* its frame buffer fb */
#undef font_4x6
#include "../../gui/launcher_stock.h"
#include "../../gui/appline.h"

static int fails, dump;

/* The entries: label and icon field as in /etc/apps. */
static const char *const lab[] = { "Sub-GHz", "125 kHz RFID", "NFC",
	"Infrared", "Kanpaia", "Hutsa", "Nire app" };
static const char *const fld[] = { "@subghz", "@rfid", "@nfc", "@ir",
	"@bell", "",
	"##FFC080408040804080408040804080408040FFC0" };
#define N	7

static unsigned char r6[N][ICON_H], r10[N][ICON10_B];
static int has6[N], has10[N];
/* The 14x14 icons for menu.c: the 10x10 (or the 8x6) in the middle, as
 * the launcher draws them (XBM: LSB left, 2 bytes a row). */
static uint8_t xbm[N][1 + 28];
static const uint8_t *frames[N][1];
static Icon icons[N];

static void row(int i, struct stock_row *r)
{
	r->label = lab[i];
	r->icon6 = has6[i] ? r6[i] : NULL;
	r->icon10 = has10[i] ? r10[i] : has6[i] ? icon10_of(r6[i]) : NULL;
}

static void setpix(uint8_t *x, int px, int py)
{
	x[1 + py * 2 + px / 8] |= (uint8_t) (1 << (px & 7));
}

static void make_icons(void)
{
	struct stock_row r;
	int i, x, y;

	for (i = 0; i < N; i++) {
		has6[i] = icon_parse(fld[i], r6[i], gui_glyph_row);
		has10[i] = icon10_parse(fld[i], r10[i]);
		row(i, &r);
		memset(xbm[i], 0, sizeof xbm[i]);
		for (y = 0; y < 10; y++)
			for (x = 0; x < 10; x++)
				if (r.icon10 != NULL
				    && r.icon10[2 * y + x / 8] & 0x80 >> (x & 7))
					setpix(xbm[i], 2 + x, 2 + y);
		for (y = 0; y < 6; y++)
			for (x = 0; x < 8; x++)
				if (r.icon10 == NULL && r.icon6 != NULL
				    && r.icon6[y] & 0x80 >> x)
					setpix(xbm[i], 3 + x, 4 + y);
		frames[i][0] = xbm[i];
		memcpy(&icons[i], &(Icon){ .width = 14, .height = 14,
		       .frame_count = 1, .frame_rate = 0, .frames = frames[i] },
		       sizeof(Icon));
	}
}

static void show(const char *name, const uint8_t *a)
{
	int x, y;

	printf("FRAME %s\n", name);
	for (y = 0; y < 64; y++) {
		for (x = 0; x < 128; x++)
			putchar(a[(y >> 3) * 128 + x] >> (y & 7) & 1 ? '#' : '.');
		putchar('\n');
	}
}

static void same(const char *name, const uint8_t *ref)
{
	int ok = memcmp(ref, fb, sizeof fb) == 0;

	printf("  %s %s\n", ok ? "ok  " : "FAIL", name);
	if (!ok) {
		fails++;
		show("stock (libfuri)", ref);
		show("launcher", fb);
	} else if (dump)
		show(name, fb);
}

static void menu_cb(void *ctx, uint32_t index)
{
	(void) ctx;
	(void) index;
}

static void sub_row(int i, struct stock_row *r)
{
	r->label = lab[i];
	r->icon6 = r->icon10 = NULL;
}

int main(int argc, char **argv)
{
	Canvas *c = canvas_init();
	Menu *m = menu_alloc();
	Submenu *s = submenu_alloc();
	char name[40];
	int i, pos;

	setenv("FURI_STYLE", "stock", 1);	/* libfuri's Submenu as stock (T22b) */

	dump = argc > 1 && strcmp(argv[1], "-d") == 0;
	make_icons();
	for (i = 0; i < N; i++)
		menu_add_item(m, lab[i], &icons[i], i, menu_cb, NULL);
	for (pos = 0; pos < N; pos++) {
		menu_set_selected_item(m, pos);
		view_draw(menu_get_view(m), c);
		stock_menu(N, pos, row);
		snprintf(name, sizeof name, "menu.c vs stock_menu, entry %d", pos);
		same(name, canvas_get_buffer(c));
	}
	menu_reset(m);			/* no entries: "Empty" */
	view_draw(menu_get_view(m), c);
	stock_menu(0, 0, row);
	same("menu.c vs stock_menu, empty", canvas_get_buffer(c));

	submenu_set_header(s, "SETTINGS");
	for (i = 0; i < N; i++) submenu_add_item(s, lab[i], i, menu_cb, NULL);
	for (pos = 0; pos < N; pos++) {
		submenu_set_selected_item(s, pos);
		view_draw(submenu_get_view(s), c);
		stock_submenu("SETTINGS", N, pos, sub_row);
		snprintf(name, sizeof name, "submenu vs stock_submenu, row %d", pos);
		same(name, canvas_get_buffer(c));
	}

	/* the ## form, and a Pine icon after it */
	{
		unsigned char a[ICON10_B], b[ICON_H];
		int ok = icon10_parse(fld[6], a) && a[0] == 0xFF && a[1] == 0xC0
			&& !icon_parse(fld[6], b, gui_glyph_row)
			&& icon_parse("##FFC080408040804080408040804080408040FFC0@bell",
				      b, gui_glyph_row) && b[0] == 0x18
			&& !icon10_parse("##FFC0", a) && !icon10_parse("#183C3C7E0018", a)
			&& icon10_of(r6[0]) != NULL && icon10_of(r6[4]) == NULL;

		printf("  %s the ## form; 10x10 built-ins\n", ok ? "ok  " : "FAIL");
		if (!ok) fails++;
	}
	if (dump) {
		stock_sample(0);
		show("sample pine", fb);
		stock_sample(1);
		show("sample stock", fb);
		stock_menu(N, 1, row);
		stock_popup("unknown user");
		show("popup", fb);
	}
	submenu_free(s);
	menu_free(m);
	canvas_free(c);
	printf("stocktest: %d failed\n", fails);
	return fails != 0;
}
