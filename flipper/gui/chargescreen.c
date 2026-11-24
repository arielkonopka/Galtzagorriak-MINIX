/* chargescreen - the charging screen on the LCD (issue #407).
 *
 * The picture art/galtzagorri-charge.png (chargeart.h, made by
 * tools/mkbootart.py), with the battery on the Flipper's own screen in it
 * filled to the charge (12 columns), the last column blinking while it
 * charges, and the percentage in a small box at the top left.  Reads
 * /dev/power (soc=, chg=) every two seconds.  This is the app version, to see
 * the screen on the real LCD: Back leaves.  The charging mode of the start
 * (before anything runs) will draw the same in the HAL. */
#include <sys/types.h>
#include <sys/times.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "gui.h"
#include "chargeart.h"

static int field(const char *s, const char *key, int dflt)
{
	const char *p = strstr(s, key);

	return p == NULL ? dflt : atoi(p + strlen(key));
}

/* soc < 0: no gauge value. chg: the charger's status (1, 2 charging, 3 done) */
static void draw(int soc, int chg, int phase)
{
	char txt[8];
	int x, y, cols, c, w;

	gui_image(chargeart_fb);
	/* the Flipper's screen: dark inside, the battery light on it */
	gui_fill(15, 46, 30, 49, 1);
	for (x = 15; x <= 28; x++) { gui_pixel(x, 46, 0); gui_pixel(x, 49, 0); }
	for (y = 46; y <= 49; y++) { gui_pixel(15, y, 0); gui_pixel(28, y, 0); }
	gui_pixel(29, 47, 0);
	gui_pixel(29, 48, 0);
	if (soc >= 0) {
		cols = (soc * 12 + 50) / 100;
		for (c = 0; c < cols; c++) {
			if (chg >= 1 && chg <= 2 && soc < 100 && c == cols - 1
								&& phase)
				continue;		/* the blink */
			gui_pixel(16 + c, 47, 0);
			gui_pixel(16 + c, 48, 0);
		}
		sprintf(txt, "%d%%", soc);
		w = (int) strlen(txt) * 4 + 3;
		gui_fill(1, 1, 1 + w, 9, 0);	/* the box: light, a dark frame */
		gui_frame(1, 1, 1 + w, 9);
		gui_text(3, 2, txt, 0);
	}
	gui_flush();
}

int main(void)
{
	char buf[200];
	struct gui_in in;
	struct tms t;
	int soc, chg, fd, n, k;
	long tick;

	if (gui_open() < 0) return 1;
	memset(&in, 0, sizeof in);
	for (;;) {
		soc = -1; chg = 0;
		if ((fd = open("/dev/power", O_RDONLY)) >= 0) {
			if ((n = read(fd, buf, sizeof buf - 1)) > 0) {
				buf[n] = 0;
				soc = field(buf, "soc=", -1);
				chg = field(buf, "chg=", 0);
			}
			close(fd);
		}
		tick = times(&t) / CLK_TCK;
		draw(soc, chg, (int) (tick / 2 & 1));
		/* every second: the blink phase flips every 2 s, the gauge
		 * is read at the same pace */
		k = gui_input(&in, KEY_BACK, 1000);
		if (k == KEY_BACK) break;
	}
	gui_close();
	return 0;
}
