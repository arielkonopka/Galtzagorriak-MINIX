/* bootscreen - a picture and the last line of its input on the LCD (#399).
 *
 * Usage: fsck ... 2>&1 | bootscreen [-t TITLE]
 *
 * For the start, before the launcher runs: draws art/galtzagorri-fix.png
 * (bootart.h, made by tools/mkbootart.py) with a framed banner along the
 * bottom edge: TITLE until the first line of input comes, then the last line
 * (a carriage return or a newline ends one: fsck -v redraws its phase line
 * in place).  The input is copied to the output, so the console keeps it.
 * At the end of the input it leaves the last picture on the LCD and exits.
 * It opens /dev/lcd only, never /dev/buttons: fsck -k reads Back there. */
#include <sys/types.h>
#include <sys/times.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "gui.h"
#include "bootart.h"

#define LINE_MAX	24		/* columns of the banner (4x6 font) */

static void draw(const char *s)
{
	gui_image(bootart_fb);
	gui_fill(2, 50, 125, 62, 0);	/* the banner: white, framed */
	gui_frame(2, 50, 125, 62);
	gui_center(54, s, 1);
	gui_flush();
}

int main(int argc, char **argv)
{
	char buf[64], line[LINE_MAX + 1], shown[LINE_MAX + 1];
	const char *title = "Checking the card";
	struct tms t;
	long last = 0, now;
	int n, i, len = 0;

	if (argc == 3 && strcmp(argv[1], "-t") == 0) title = argv[2];
	if (gui_open_lcd() < 0) return 1;
	strncpy(shown, title, LINE_MAX);
	shown[LINE_MAX] = 0;
	draw(shown);
	while ((n = read(0, buf, sizeof buf)) > 0) {
		write(1, buf, n);
		for (i = 0; i < n; i++) {
			if (buf[i] == '\n' || buf[i] == '\r') {
				if (len > 0) {
					line[len] = 0;
					strcpy(shown, line);
					len = 0;
				}
			} else if (len < LINE_MAX && buf[i] >= 32 && buf[i] < 127)
				line[len++] = buf[i];
		}
		now = times(&t);		/* at most 6 redraws a second */
		if (now - last >= CLK_TCK / 6) {
			draw(shown);
			last = now;
		}
	}
	draw(shown);
	gui_close();
	return 0;
}
