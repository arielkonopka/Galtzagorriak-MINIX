/* av - self test module (spec R7b.3.2): sound, vibration, the RGB LED,
 * the display and the backlight, then the user says whether it all looked
 * and sounded right (OK) or not (Back).  Interactive.
 *
 * The tune is our own: a scale rising in whole tones from 220 Hz to about
 * 7 kHz and falling back, 60 ms a note.
 */
#include <sys/types.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "gui.h"
#include "st.h"

static void pause_ms(int ms)
{
	(void) poll((struct pollfd *) NULL, 0, ms);
}

static void put(const char *dev, const void *b, int n)
{
	int fd;

	if ((fd = open(dev, O_WRONLY)) < 0) return;
	(void) write(fd, b, n);
	close(fd);
}

static void caption(const char *s)
{
	gui_clear();
	gui_title("Sound and light");
	gui_center(30, s, 0);
	gui_flush();
}

static void tune(void)
{
	unsigned char n[4 * 62];
	unsigned hz = 220;
	int i, k = 0;

	for (i = 0; i < 31; i++, hz = hz * 1122 / 1000) {	/* +2 semitones */
		n[k++] = hz & 0xFF; n[k++] = hz >> 8; n[k++] = 60; n[k++] = 0;
	}
	for (i = 0; i < 31; i++) {
		hz = hz * 1000 / 1122;
		n[k++] = hz & 0xFF; n[k++] = hz >> 8; n[k++] = 60; n[k++] = 0;
	}
	put("/dev/speaker", n, k);	/* returns when it has played */
}

int main(int argc, char **argv)
{
	static const unsigned char rgb[5][3] = {
		{ 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 255 },
		{ 0, 0, 0 } };
	unsigned char bl;
	int i, x, y, k;

	if (argc > 1 && strcmp(argv[1], "-a") == 0) {
		st_result("SKIP", "interactive");
		return 0;
	}
	if (gui_open() < 0) {
		st_result("SKIP", "no screen");
		return 0;
	}
	caption("Scale up and down");
	tune();
	caption("Vibration: 3 pulses");
	for (i = 0; i < 3; i++) {
		put("/dev/vibro", "200", 3);
		pause_ms(450);
	}
	caption("LED: red green blue white");
	for (i = 0; i < 5; i++) {
		put("/dev/led", rgb[i], 3);
		pause_ms(500);
	}
	gui_fill(0, 0, GUI_W - 1, GUI_H - 1, 1);	/* all on */
	gui_flush();
	pause_ms(800);
	gui_clear();					/* all off */
	gui_flush();
	pause_ms(800);
	for (y = 0; y < GUI_H; y++)			/* checkerboard */
		for (x = 0; x < GUI_W; x++)
			gui_pixel(x, y, ((x >> 2) + (y >> 2)) & 1);
	gui_flush();
	pause_ms(800);
	for (y = 0; y < GUI_H; y++)			/* inverse */
		for (x = 0; x < GUI_W; x++)
			gui_pixel(x, y, !(((x >> 2) + (y >> 2)) & 1));
	gui_flush();
	pause_ms(800);
	caption("Backlight: dark to bright");
	for (i = 0; i <= 16; i++) {
		bl = i == 16 ? 255 : i * 16;
		put("/dev/backlight", &bl, 1);
		pause_ms(120);
	}
	gui_clear();
	gui_title("Sound and light");
	gui_center(22, "Did it all look and", 0);
	gui_center(30, "sound right?", 0);
	gui_center(52, "OK: yes   Back: no", 0);
	gui_flush();
	do k = gui_key(); while (k != KEY_OK && k != KEY_BACK);
	gui_close();
	st_result(k == KEY_OK ? "PASS" : "FAIL", k == KEY_OK ? NULL
						: "the user saw or heard a fault");
	return k != KEY_OK;
}
