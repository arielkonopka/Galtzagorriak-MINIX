/* lights - status LED colour and display backlight. */
#include <sys/types.h>
#include <stdio.h>
#include "gui.h"

static const char *names[] = { "off", "red", "green", "blue", "white", "gold" };
static const int rgb[][3] = {
	{ 0, 0, 0 }, { 80, 0, 0 }, { 0, 80, 0 }, { 0, 0, 80 },
	{ 60, 60, 60 }, { 90, 50, 0 },
};
#define NCOL	(sizeof names / sizeof names[0])

int main(void)
{
	int col = 0, bl = 255, k;
	char line[40];

	if (gui_open() < 0) return 1;
	for (;;) {
		gui_clear();
		gui_title("LIGHTS");
		sprintf(line, "< LED: %-5s >", names[col]);
		gui_center(18, line, 0);
		sprintf(line, "UP/DOWN backlight %3d", bl);
		gui_center(30, line, 0);
		gui_center(52, "BACK to return", 0);
		gui_flush();
		k = gui_key() & ~KEY_LONG;
		if (k == KEY_BACK) break;
		if (k == KEY_RIGHT) col = (col + 1) % NCOL;
		if (k == KEY_LEFT) col = (col + NCOL - 1) % NCOL;
		if (k == KEY_UP) bl = bl + 32 > 255 ? 255 : bl + 32;
		if (k == KEY_DOWN) bl = bl - 32 < 0 ? 0 : bl - 32;
		gui_led(rgb[col][0], rgb[col][1], rgb[col][2]);
		gui_backlight(bl);
	}
	gui_led(0, 0, 0);
	return 0;
}
