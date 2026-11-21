/* SPDX-License-Identifier: GPL-3.0-or-later
 * passport - the stock Passport on Galtzagorriak MINIX (issue #420), a
 * window app (spec K.8): who this Flipper is.  Stock shows the dolphin's
 * name, mood and level; MINIX has no dolphin, so the passport shows the
 * device's name (uname's nodename, /etc/hostname.file), the system and
 * its release, the time since boot and the battery (/dev/power), read
 * once when it opens.  OK reads them again.
 *
 *	Passport:nobody:@info:=/usr/bin/passport	(/etc/apps)
 *
 * RAM: no stdio, no malloc, no signal handler: no MM gap (K.8).
 */
#include <sys/types.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "gm.h"

static char *num(char *t, long v)		/* t[12] */
{
	char *p = t + 11;

	*p = 0;
	do *--p = (char) ('0' + v % 10); while ((v /= 10) > 0);
	return p;
}

/* "soc=NN ..." of /dev/power: the charge in %, or -1. */
static int battery(void)
{
	char l[96];
	int fd, n, v = 0;

	if ((fd = open("/dev/power", O_RDONLY)) < 0) return -1;
	n = (int) read(fd, l, sizeof l - 1);
	close(fd);
	if (n < 5 || memcmp(l, "soc=", 4) != 0) return -1;
	for (n = 4; l[n] >= '0' && l[n] <= '9' && n < 8; n++)
		v = v * 10 + (l[n] - '0');
	return n > 4 ? v : -1;
}

static void w_main(void)
{
	struct utsname u;
	struct tms tm;
	char l[48], t[12];
	long up = (long) times(&tm) / CLK_TCK;
	int b = battery();

	gm_win("pass", "text", "Passport", 0, 0);
	if (uname(&u) == 0) {
		strcpy(l, "Name ");
		strncat(l, u.nodename, 30);
		gm_line(l);
		strncpy(l, u.sysname, 20);
		l[20] = 0;
		strcat(l, " ");
		strncat(l, u.release, 20);
		gm_line(l);
	}
	strcpy(l, "Up ");
	if (up >= 86400L) {
		strcat(l, num(t, up / 86400L));
		strcat(l, " d ");
	}
	strcat(l, num(t, up / 3600 % 24));
	strcat(l, " h ");
	strcat(l, num(t, up / 60 % 60));
	strcat(l, " min");
	gm_line(l);
	if (b >= 0) {
		strcpy(l, "Battery ");
		strcat(l, num(t, b));
		strcat(l, " %");
		gm_line(l);
	}
	gm_action("again", "Refresh");
	gm_show();
}

int main(int argc, char **argv)
{
	char buf[128], ev[128], *f[GM_FIELDS];
	static const char use[] = "passport: start the Passport from the "
		"launcher\n";

	(void) argv;
	if (argc != 1 || gm_init(buf, sizeof buf) < 0) {
		(void) write(2, use, sizeof use - 1);
		return 1;
	}
	w_main();
	for (;;) switch (gm_event(ev, sizeof ev, f, GM_FIELDS)) {
	case GM_OK:
	case GM_REDRAW:
		w_main();
		break;
	case GM_CLOSE:
	case GM_BACK:
		return 0;
	}
}
