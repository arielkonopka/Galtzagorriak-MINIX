/* power - the battery, from the fuel gauge (/dev/power, T15a).
 *
 * Now: the current (large), the gauge's average, charge, voltage,
 * temperature, charging or on battery; read every second while the
 * backlight is on.
 * Idle: the gauge's average current read after the backlight has been off
 * for 30 s or more, the draw of the device left alone (neither the
 * backlight nor this app's refresh in it); while the light is off the app
 * reads only every 10 s.  With USB plugged in there is no idle reading.
 *
 * Keys wait blocking; alarm() brings the refresh (the read gives EINTR).
 * Back quits; other keys do nothing yet (the rest of T15 adds the menu).
 */
#include <sys/types.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "gui.h"

#define BL_OFF		0x08000000L	/* /dev/backlight state: off (hal.h) */
#define IDLE_AFTER	30		/* s with the light off */

struct gauge {
	int soc, mv, ma, avg, sby, temp, usb, chg;
	unsigned flags;
};

static void tick(int sig)
{
	signal(SIGALRM, tick);
}

static int gauge(struct gauge *g)
{
	char line[96];
	int fd, n;

	if ((fd = open("/dev/power", O_RDONLY)) < 0) return -1;
	n = read(fd, line, sizeof line - 1);
	close(fd);
	if (n <= 0) return -1;
	line[n] = 0;
	g->chg = 0;
	return sscanf(line, "soc=%d mv=%d ma=%d avg=%d sby=%d temp=%d flags=0x%x"
		" usb=%d chg=%d", &g->soc, &g->mv, &g->ma, &g->avg, &g->sby,
		&g->temp, &g->flags, &g->usb, &g->chg) >= 8 ? 0 : -1;
}

static int light_off(void)
{
	long st = 0;
	int fd;

	if ((fd = open("/dev/backlight", O_RDONLY)) < 0) return 0;
	if (read(fd, (char *) &st, 4) != 4) st = 0;
	close(fd);
	return (st & BL_OFF) != 0;
}

int main(void)
{
	struct gauge g;
	char line[40];
	int ok, off, usb, have_idle = 0, idle = 0, k, w;
	time_t now, off_since = 0, idle_at = 0;
	long age;

	if (gui_open() < 0) return 1;
	signal(SIGALRM, tick);
	for (;;) {
		now = time((time_t *) 0);
		ok = gauge(&g) == 0;
		off = light_off();
		if (!off) off_since = 0;
		else if (off_since == 0) off_since = now;
		/* a USB host, or charging from a power supply */
		usb = ok && (g.usb || g.ma > 0);
		if (ok && off && !usb && now - off_since >= IDLE_AFTER) {
			idle = g.avg;
			idle_at = now;
			have_idle = 1;
		}

		gui_clear();
		gui_title("POWER");
		if (!ok) {
			gui_center(26, "no gauge", 0);
		} else {
			/* the current in segment digits (16x24, the user's
			 * wish), its unit beside it in the small font; the
			 * rows below moved 2 pixels down for it */
			sprintf(line, "%d", g.ma);
			w = (int) strlen(line) * 16 + 2 + 2 * 4;
			k = (GUI_W - w) / 2;
			k += gui_text_seg(k, 8, line) + 2;
			gui_text(k, 8 + 24 - 6, "mA", 0);
			sprintf(line, "avg %d mA   %d%%", g.avg, g.soc);
			gui_center(33, line, 0);
			sprintf(line, "%d.%03d V   %d.%d C", g.mv / 1000,
				g.mv % 1000, g.temp / 10,
				(g.temp < 0 ? -g.temp : g.temp) % 10);
			gui_center(40, line, 0);
			gui_center(47, g.chg == 3 ? "charged" :
				g.chg != 0 || g.ma > 0 ? "charging" :
				usb ? "on USB" : "on battery", 0);
			if (usb) {
				strcpy(line, "USB: unplug to measure");
			} else if (!have_idle) {
				strcpy(line, "Idle: leave it 1 min");
			} else {
				age = (now - idle_at) / 60;
				if (age < 1)
					sprintf(line, "Idle %d mA, just now",
									idle);
				else if (age < 120)
					sprintf(line, "Idle %d mA, %ld min ago",
								idle, age);
				else
					sprintf(line, "Idle %d mA, %ld h ago",
							idle, age / 60);
			}
			gui_center(55, line, 0);
		}
		gui_flush();

		/* The next refresh by the alarm; a key other than Back changes
		 * nothing, so it waits on (no gauge read, no frame). */
		alarm(off ? 10 : 1);
		while ((k = gui_key()) != 0 && k != KEY_BACK) ;
		alarm(0);
		if (k == KEY_BACK) break;
	}
	return 0;
}
