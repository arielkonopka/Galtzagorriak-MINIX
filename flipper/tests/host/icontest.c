/* Host test of the launcher's /etc/apps lines and icons (flipper/gui/
 * appline.c, spec R5.6a.1-2): every icon form, malformed ones, the old
 * two-field form, cuts of label, user and command. */
#include <stdio.h>
#include <string.h>
#include "../../gui/appline.h"
#include "../../hal/common/font4x6.c"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); \
	fails++; } else printf("  ok   %s\n", what); } while (0)

static int glyph(int c, int row)
{
	if (c < 32 || c > 126) c = '?';
	return font_4x6[c - 32][row];
}

static int parse(const char *in, struct app_fields *a, char *buf)
{
	strcpy(buf, in);
	return app_line(buf, a);
}

int main(void)
{
	static char buf[256];
	struct app_fields a;
	unsigned char r[ICON_H], want[ICON_H];
	char longl[200];
	int i;

	CHECK(parse("Console:nobody:/bin/console\n", &a, buf) == 0
		&& strcmp(a.label, "Console") == 0 && strcmp(a.user, "nobody") == 0
		&& a.icon == NULL && strcmp(a.cmd, "/bin/console") == 0,
		"old form: no icon, program after the user");
	CHECK(parse("Kanpaia:nobody:@bell:/bin/remind -x\n", &a, buf) == 0
		&& strcmp(a.icon, "@bell") == 0
		&& strcmp(a.cmd, "/bin/remind -x") == 0, "new form: icon field");
	CHECK(parse("Demo:nobody:=/bin/gmdemo -l /tmp/x\n", &a, buf) == 0
		&& a.icon == NULL && strcmp(a.cmd, "=/bin/gmdemo -l /tmp/x") == 0,
		"old form: a window app (=/program, spec K.8)");
	CHECK(parse("Demo:nobody:@app:=/bin/gmdemo\n", &a, buf) == 0
		&& strcmp(a.icon, "@app") == 0 && strcmp(a.cmd, "=/bin/gmdemo") == 0,
		"new form: a window app");
	CHECK(parse("E:root::/bin/x", &a, buf) == 0 && a.icon != NULL
		&& a.icon[0] == 0 && !icon_parse(a.icon, r, glyph),
		"empty icon field: no icon");
	CHECK(parse("# comment", &a, buf) < 0 && parse("", &a, buf) < 0
		&& parse("nocolon", &a, buf) < 0 && parse("L:u", &a, buf) < 0
		&& parse("L:u:Zz", &a, buf) < 0, "comments, empty and broken lines");

	CHECK(icon_parse("@bell", r, glyph) && icon_name(0, want) != NULL,
		"@bell is built in");
	CHECK(!icon_parse("@nosuch", r, glyph), "@nosuch: no icon");
	CHECK(icon_parse("#183C3C7E0018", r, glyph) && r[0] == 0x18
		&& r[3] == 0x7E && r[5] == 0x18, "# bitmap, 12 hex digits");
	CHECK(icon_parse("#183c3c7e0018", r, glyph) && r[3] == 0x7E,
		"# bitmap, lower case hex");
	CHECK(!icon_parse("#183C3C7E00", r, glyph)
		&& !icon_parse("#183C3C7E0018FF", r, glyph)
		&& !icon_parse("#183C3C7E00G8", r, glyph), "# bad length or hex");
	CHECK(icon_parse("Zz", r, glyph), "two characters");
	for (i = 0; i < ICON_H; i++)
		want[i] = (unsigned char) ((glyph('Z', i) & 0xF0)
					| (glyph('z', i) & 0xF0) >> 4);
	CHECK(memcmp(r, want, ICON_H) == 0, "two characters: the font's glyphs");
	CHECK(icon_parse("!", r, glyph) && r[0] == (glyph('!', 0) & 0xF0) >> 2,
		"one character, in the middle of the cell");
	CHECK(!icon_parse("abc", r, glyph), "three characters: no icon");
	CHECK(!icon_parse("\t", r, glyph), "not printable: no icon");

	memset(longl, 'x', sizeof longl);
	memcpy(longl, "L", 1);
	strcpy(longl + 30, ":averyveryverylonguser:/bin/");
	memset(longl + 58, 'c', 100);
	longl[158] = 0;
	CHECK(parse(longl, &a, buf) == 0 && strlen(a.label) == 23
		&& strlen(a.user) == 11 && strlen(a.cmd) == APP_CMDMAX - 1,
		"label 23, user 11, command 63: cut as before");

	CHECK(icon_count() >= 24, "at least 24 built-in icons");
	{
		static const char *need[] = { "app", "console", "info", "light",
			"power", "battery", "rfid", "nfc", "subghz", "ir",
			"ibutton", "gpio", "usb", "ble", "clock", "alarm", "bell",
			"timer", "sensor", "game", "music", "tools", "file",
			"settings" };
		char at[12];
		int ok = 1;

		for (i = 0; i < 24; i++) {
			sprintf(at, "@%s", need[i]);
			if (!icon_parse(at, r, glyph)) ok = 0;
		}
		CHECK(ok, "the spec's 24 names are all there");
	}
	printf(fails ? "icontest: %d FAILED\n" : "icontest: PASS\n", fails);
	return fails != 0;
}
