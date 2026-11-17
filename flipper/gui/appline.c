/* appline - the lines of /etc/apps and the launcher's icons (spec R5.6a.1,
 * R5.6a.2).  Pure functions, no I/O: the launcher calls them when it reads
 * /etc/apps, the host test (flipper/tests/host/icontest.c) feeds them every
 * form.
 *
 *	Label:user:/program arg ...		(the old form, no icon)
 *	Label:user:icon:/program arg ...	(icon: see icon_parse)
 *	Label:user:icon:=/program arg ...	a window app (spec K.8)
 *
 * The field after the user is the program if it starts with '/' or "=/"
 * (programs in /etc/apps are absolute paths; '=' marks a window app, which
 * the launcher serves itself: winsrv.c), else the icon.
 */
#include <string.h>
#include "appline.h"

/* Built-in icons, 8x6, one byte per row, the MSB on the left: original
 * drawings for Galtzagorriak (tools/iconpng.py shows them enlarged).
 * const, so in the program's text: flash, 0 B of RAM. */
static const struct {
	char name[9];
	unsigned char rows[ICON_H];
} icon_tab[] = {
  { "app", { 0x7E, 0x54, 0x7E, 0x42, 0x7E, 0x00 } },
  { "console", { 0xFF, 0x81, 0xA1, 0x96, 0x81, 0xFF } },
  { "info", { 0x18, 0x00, 0x38, 0x18, 0x18, 0x3C } },
  { "light", { 0x3C, 0x42, 0x42, 0x24, 0x18, 0x18 } },
  { "power", { 0x10, 0x54, 0x92, 0x82, 0x44, 0x38 } },
  { "battery", { 0x00, 0xFE, 0xB3, 0xB3, 0xFE, 0x00 } },
  { "rfid", { 0x92, 0x54, 0x38, 0x28, 0x38, 0x00 } },
  { "nfc", { 0x64, 0x92, 0x92, 0x92, 0x92, 0x64 } },
  { "subghz", { 0x81, 0xA5, 0xAD, 0x24, 0x18, 0x18 } },
  { "ir", { 0x80, 0x44, 0x26, 0x7D, 0x26, 0x44 } },
  { "ibutton", { 0x3C, 0x42, 0x99, 0x99, 0x42, 0x3C } },
  { "gpio", { 0xAA, 0xAA, 0xFE, 0xFE, 0xAA, 0xAA } },
  { "usb", { 0x10, 0x38, 0x52, 0x56, 0x38, 0x10 } },
  { "ble", { 0x10, 0x18, 0x54, 0x30, 0x54, 0x18 } },
  { "clock", { 0x3C, 0x4A, 0x89, 0x8E, 0x42, 0x3C } },
  { "alarm", { 0xBD, 0x42, 0x91, 0x99, 0x42, 0xBD } },
  { "bell", { 0x18, 0x3C, 0x3C, 0x7E, 0x00, 0x18 } },
  { "timer", { 0x7E, 0x24, 0x18, 0x18, 0x24, 0x7E } },
  { "sensor", { 0x30, 0x28, 0x37, 0x78, 0x78, 0x30 } },
  { "game", { 0x7E, 0x91, 0xBB, 0x91, 0x66, 0x00 } },
  { "music", { 0x1E, 0x12, 0x12, 0x76, 0xF6, 0x60 } },
  { "tools", { 0xC3, 0x66, 0x3C, 0x3C, 0x66, 0xC3 } },
  { "file", { 0xF8, 0x8C, 0x84, 0xB4, 0x84, 0xFC } },
  { "settings", { 0x18, 0x7E, 0x66, 0x66, 0x7E, 0x18 } },
};
#define ICON_N	(sizeof(icon_tab) / sizeof(icon_tab[0]))

int icon_count(void)
{
	return (int) ICON_N;
}

const char *icon_name(int i, unsigned char *rows)
{
	if (i < 0 || i >= (int) ICON_N) return NULL;
	memcpy(rows, icon_tab[i].rows, ICON_H);
	return icon_tab[i].name;
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* An icon field into 6 rows: 1 if there is an icon, 0 if none (empty,
 * unknown name, malformed).  Forms: "@name" built in; "#" and 12 hex
 * digits; 1 or 2 printable characters drawn in the list font (`glyph`
 * gives a character's font row, the high 4 bits; one character sits in
 * the middle of the cell). */
int icon_parse(const char *f, unsigned char *rows, int (*glyph)(int c, int row))
{
	int i, h, l, n = (int) strlen(f);

	if (n == 0) return 0;
	if (f[0] == '@') {
		for (i = 0; i < (int) ICON_N; i++)
			if (strcmp(f + 1, icon_tab[i].name) == 0) {
				memcpy(rows, icon_tab[i].rows, ICON_H);
				return 1;
			}
		return 0;
	}
	if (f[0] == '#') {
		if (n != 1 + 2 * ICON_H) return 0;
		for (i = 0; i < ICON_H; i++) {
			if ((h = hexval(f[1 + 2 * i])) < 0
			    || (l = hexval(f[2 + 2 * i])) < 0) return 0;
			rows[i] = (unsigned char) (h << 4 | l);
		}
		return 1;
	}
	if (n > 2 || f[0] == '/') return 0;
	for (i = 0; i < n; i++)
		if (f[i] < 32 || f[i] > 126 || f[i] == ':') return 0;
	for (i = 0; i < ICON_H; i++) {
		if (n == 1)
			rows[i] = (unsigned char) ((glyph(f[0], i) & 0xF0) >> 2);
		else
			rows[i] = (unsigned char) ((glyph(f[0], i) & 0xF0)
					| (glyph(f[1], i) & 0xF0) >> 4);
	}
	return 1;
}

/* Split one line of /etc/apps in place.  0 and the fields in *a (icon
 * NULL in the old form), or -1 for a comment, an empty or a broken line.
 * Lengths are cut as the launcher always did: label 23, user 11, command
 * APP_CMDMAX - 1. */
int app_line(char *line, struct app_fields *a)
{
	char *u, *r, *c;

	line[strcspn(line, "\n")] = 0;
	if (line[0] == '#' || line[0] == 0) return -1;
	if ((u = strchr(line, ':')) == NULL) return -1;
	*u++ = 0;
	if ((r = strchr(u, ':')) == NULL) return -1;
	*r++ = 0;
	if (r[0] == '/' || (r[0] == '=' && r[1] == '/')) {	/* the old form */
		a->icon = NULL;
		c = r;
	} else {
		if ((c = strchr(r, ':')) == NULL) return -1;
		*c++ = 0;
		a->icon = r;
	}
	if (strlen(line) > 23) line[23] = 0;
	if (strlen(u) > 11) u[11] = 0;
	if (strlen(c) > APP_CMDMAX - 1) c[APP_CMDMAX - 1] = 0;
	a->label = line;
	a->user = u;
	a->cmd = c;
	return 0;
}
