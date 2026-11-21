/* SPDX-License-Identifier: GPL-3.0-or-later
 * clock - the stock Clock app on Galtzagorriak MINIX (issue #420), a
 * window app (spec K.8).  The desktop's "clock" window draws the time in
 * big digits and redraws it on every new minute by itself; this app adds
 * the weekday and the date below and sleeps until midnight (or Back).
 * OK (the window's action) switches the date between "Sat 21 Nov 2026" and "2026-11-21".
 *
 *	Clock:nobody:@clock:=/usr/bin/clock	(/etc/apps)
 *
 * Stock's clock also shows seconds; here the screen changes once a
 * minute at most (rule 0.8: no periodic redraw for decoration).
 * RAM: no stdio, no malloc, no signal handler: no MM gap (K.8).
 */
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "gm.h"

static const char wd[7][4] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri",
			       "Sat" };
static const char mo[12][4] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
				"Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static int iso;

static char *two(char *p, int v)
{
	*p++ = (char) ('0' + v / 10 % 10);
	*p++ = (char) ('0' + v % 10);
	return p;
}

/* The date line of tm into l (at least 20 B). */
static void clock_date(char *l, const struct tm *tm, int iso8601)
{
	int y = tm->tm_year + 1900;
	char *p = l;

	if (iso8601) {
		p = two(two(p, y / 100), y % 100);
		*p++ = '-';
		p = two(p, tm->tm_mon + 1);
		*p++ = '-';
		p = two(p, tm->tm_mday);
	} else {
		memcpy(p, wd[tm->tm_wday % 7], 3), p += 3;
		*p++ = ' ';
		p = two(p, tm->tm_mday);
		*p++ = ' ';
		memcpy(p, mo[tm->tm_mon % 12], 3), p += 3;
		*p++ = ' ';
		p = two(two(p, y / 100), y % 100);
	}
	*p = 0;
}

static int w_clock(void)		/* ms to the next midnight */
{
	time_t now = time((time_t *) NULL);
	struct tm *tm = localtime(&now);
	char l[20];
	long s;

	clock_date(l, tm, iso);
	gm_win("clock", "clock", "Clock", 0, 0);
	gm_line(l);
	gm_action("fmt", "Date format");
	gm_show();
	s = 86400L - (tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec);
	return (int) (s > 3600 ? 3600000L : s * 1000L + 500);
}

int main(int argc, char **argv)
{
	char buf[128], ev[128], *f[GM_FIELDS];
	static const char use[] = "clock: start the Clock from the launcher\n";
	int ms;

	(void) argv;
	if (argc != 1 || gm_init(buf, sizeof buf) < 0) {
		(void) write(2, use, sizeof use - 1);
		return 1;
	}
	ms = w_clock();
	for (;;) {
		if (!gm_wait(ev, ms)) {		/* an hour, or midnight */
			ms = w_clock();
			continue;
		}
		switch (gm_event(ev, sizeof ev, f, GM_FIELDS)) {
		case GM_OK:
			iso = !iso;
			ms = w_clock();
			break;
		case GM_REDRAW:
			ms = w_clock();
			break;
		case GM_CLOSE:
		case GM_BACK:
			return 0;
		}
	}
}
