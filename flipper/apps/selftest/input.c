/* input - self test module (spec R7b.3.3): a drawing of the d-pad, OK
 * and Back; each lights while pressed.  Passes when every button has been
 * seen down and up; fails after 30 s without a press naming the ones
 * missing.  Interactive.  Long Back still ends it (R4b.8).
 */
#include <sys/types.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "gui.h"
#include "st.h"

#define ALL	(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_OK | KEY_BACK)
#define WAIT_MS	30000

static const struct { int key, x, y; const char *name; } pad[6] = {
	{ KEY_UP,    56, 10, "Up" },   { KEY_DOWN,  56, 40, "Down" },
	{ KEY_LEFT,  36, 25, "Left" }, { KEY_RIGHT, 76, 25, "Right" },
	{ KEY_OK,    56, 25, "OK" },   { KEY_BACK, 100, 46, "Back" },
};

static void draw(unsigned down, unsigned done)
{
	int i, x, y;

	gui_clear();
	gui_title("Input: press each");
	for (i = 0; i < 6; i++) {
		x = pad[i].x; y = pad[i].y;
		if (down & pad[i].key) gui_fill(x, y, x + 15, y + 12, 1);
		else gui_frame(x, y, x + 15, y + 12);
		if (done & pad[i].key) gui_pixel(x + 7, y + 6, !(down & pad[i].key));
	}
	gui_flush();
}

int main(int argc, char **argv)
{
	unsigned char ev[2];
	unsigned down = 0, seen_d = 0, seen_u = 0;
	char why[48];
	struct pollfd p;
	int fd, i;

	if (argc > 1 && strcmp(argv[1], "-a") == 0) {
		st_result("SKIP", "interactive");
		return 0;
	}
	if (gui_open() < 0 || (fd = open("/dev/buttons", O_RDONLY)) < 0) {
		st_result("SKIP", "no screen or buttons");
		return 0;
	}
	draw(0, 0);
	while ((seen_d & seen_u) != ALL) {
		p.fd = fd; p.events = POLLIN; p.revents = 0;
		if (poll(&p, 1, WAIT_MS) <= 0) break;	/* nobody presses */
		if (read(fd, ev, 2) != 2) continue;
		if (ev[1]) {
			down |= ev[0];
			seen_d |= ev[0];
		} else {
			down &= ~ev[0];
			if (seen_d & ev[0]) seen_u |= ev[0];
		}
		draw(down, seen_d & seen_u);
	}
	close(fd);
	gui_close();
	if ((seen_d & seen_u) == ALL) {
		st_result("PASS", NULL);
		return 0;
	}
	strcpy(why, "not pressed:");
	for (i = 0; i < 6; i++)
		if (!(seen_d & seen_u & pad[i].key)) {
			strcat(why, " ");
			strcat(why, pad[i].name);
		}
	st_result("FAIL", why);
	return 1;
}
