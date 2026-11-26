/* Time (spec K.7, I.1): the date and time, the time zone and 12/24 h.
 *
 * The RTC keeps UTC; the rows show local time.  A new date (OK, the
 * fields, OK) moves the clock at once and keeps the time of day; a new
 * time keeps the date and sets the seconds to 0, as on a watch.  The
 * local time comes from the zone's POSIX rule (zones.c tz_off): no
 * localtime(), mktime() or putenv(), whose zone code and data would be
 * linked into the program.  Zone opens a list of the zones in the card's
 * /usr/share/zones (zones.c), a long list read page by page
 * (K.8 ROWS=); choosing one only changes what this screen shows until
 * Back, which writes /etc/TZ once (through a new file and rename,
 * R18b.3) and gives INIT SIGHUP (the zone of what it starts) and SIGRTC
 * (timed re-arms the local-time timers).  "other": a rule not in the
 * table, kept as it is.  Clock (12/24 h) and Date fmt (the order of a
 * date's fields, as stock's Date format) are saved in
 * /etc/flipper/time.conf for the clock apps.  None of it is applied at
 * boot: the RTC runs on, and INIT reads /etc/TZ itself. */
#include <sys/types.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>		/* rename() only: no stdio is used */
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "sgroup.h"
#include "zones.h"
#include "gm.h"

#define PATH_TZ		"/etc/TZ"
#define ZPAGE		12	/* zones per page of the list (<= 16 rows,
				 * <= 640 B of records) */

enum { R_DATE, R_TIME, R_ZONE, R_CLOCK, R_DFMT };

const struct srow time_rows[] = {
	{ "Date", S_DATE, S_NOSAVE, 0, 0, 0, "", "" },
	{ "Time", S_TIME, S_NOSAVE, 0, 0, 0, "", "" },
	{ "Zone", S_ITEM, S_NOSAVE, 0, 0, 0, "", "" },
	{ "Clock", S_ENUM, 0, 0, 1, 1, "", "24 h|12 h" },
	{ "Date fmt", S_ENUM, 0, 0, 2, 1, "", "D.M.Y|M/D/Y|Y-M-D" },
};

static short nzones;			/* tz_count(): "other" is that one */
static short zone0;			/* the zone in /etc/TZ */
static short zone;			/* the zone on this screen */
static char rule0[TZ_RULE_MAX];		/* /etc/TZ's rule ("other": kept) */

static const char *rule(void)
{
	return zone < nzones ? tz_rule(zone) : rule0;
}

static long day_of(time_t t)		/* floor(t / 86400) */
{
	return (long) (t / 86400 - (t % 86400 < 0));
}

/* The rows from the clock now, in this screen's zone. */
static void show_time(void)
{
	time_t loc = time((time_t *) 0);
	long d;
	int y, m, dd;

	loc += tz_off(rule(), loc);
	d = day_of(loc);
	tz_date(d, &y, &m, &dd);
	if (y < 2000 || y > 2127) y = 2000, m = dd = 1;	/* the row's range */
	sg_val[R_DATE] = (short) SG_DATE(y, m, dd);
	sg_val[R_TIME] = (short) ((loc - (time_t) d * 86400) / 60);
}

static void load(void)
{
	char buf[96], label[TZ_LABEL_MAX];
	int fd, n = -1;

	nzones = (short) tz_count();
	rule0[0] = 0;
	if ((fd = open(PATH_TZ, O_RDONLY)) >= 0) {
		n = (int) read(fd, buf, sizeof buf - 1);
		close(fd);
	}
	if (n < 0 || (buf[n] = 0, tz_parse(buf, rule0, label) < 0)) {
		strcpy(rule0, "UTC0");		/* no /etc/TZ: UTC */
		label[0] = 0;
	}
	zone = zone0 = (short) tz_find(label, rule0);
	if (sg_val[R_CLOCK] < 0) sg_val[R_CLOCK] = 0;
	if (sg_val[R_DFMT] < 0) sg_val[R_DFMT] = 0;
	show_time();
}

/* A new date or time from the desktop: the clock too.  1: the window
 * again (a date the RTC cannot hold: it counts years 2000-2099). */
static int set_clock(int row)
{
	time_t loc = time((time_t *) 0), nl, t;
	long d, tod;
	int v = sg_val[row], y = SG_Y(v);

	loc += tz_off(rule(), loc);
	d = day_of(loc);
	tod = (long) (loc - (time_t) d * 86400);
	if (row == R_DATE) {
		if (y > 2099 || SG_M(v) < 1 || SG_M(v) > 12
		    || SG_D(v) < 1 || SG_D(v) > tz_mdays(y, SG_M(v))) {
			show_time();
			return 1;
		}
		nl = (time_t) tz_days(y, SG_M(v), SG_D(v)) * 86400 + tod;
	} else
		nl = (time_t) d * 86400 + (long) (v / 60 % 24) * 3600
			+ (v % 60) * 60;
	t = tz_utc(rule(), nl);
	(void) stime(&t);
	show_time();
	return 0;
}

static char *dec(char *t, int v)	/* t: 8 bytes */
{
	char *p = t + 7;

	*p = 0;
	do *--p = (char) ('0' + v % 10); while ((v /= 10) > 0);
	return p;
}

/* One page of the zone list, from entry first. */
static void zones(int first)
{
	int all = nzones + (zone0 >= nzones), i;
	char t[8];

	if (first < 0 || first >= all) first = 0;
	gm_win("zone", "list", "Zone", all, first);
	for (i = first; i < all && i < first + ZPAGE; i++)
		gm_item(dec(t, i), tz_label(i), i == zone ? "*" : "");
	(void) gm_show();
}

/* Back: /etc/TZ only when another zone of the table was chosen. */
static void leave(void)
{
	char buf[TZ_LABEL_MAX + TZ_RULE_MAX + 8];
	int fd, n, ok;

	if (zone == zone0 || zone >= nzones) return;
	if ((n = tz_format(buf, (int) sizeof buf, zone)) < 0) return;
	if ((fd = open(PATH_TZ ".new", O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
		ok = 0;
	else {
		ok = write(fd, buf, (size_t) n) == n;
		ok = close(fd) == 0 && ok;
	}
	if (!ok || rename(PATH_TZ ".new", PATH_TZ) < 0) {
		(void) unlink(PATH_TZ ".new");
		(void) write(2, "settings: cannot write /etc/TZ\n", 31);
		return;
	}
	(void) kill(1, SIGHUP);			/* INIT: TZ for what it starts */
	(void) kill(1, SIGRTC);			/* timed: the local timers */
}

/* An event of the zone list (row: its GM_ type). */
static int event(int type, char **f)
{
	const char *p;
	int v = 0;

	if (f[1] == NULL || strcmp(f[1], "zone") != 0) return 0;
	if (type == GM_OK || type == GM_PAGE) {
		if (f[2] == NULL) return 0;
		for (p = f[2]; *p >= '0' && *p <= '9' && v < 1000; p++)
			v = v * 10 + (*p - '0');
		if (type == GM_PAGE) {
			zones(v);
			return 1;
		}
		if (v <= nzones && (v < nzones || zone0 == nzones)) zone = (short) v;
		show_time();
		sg_show(sg_find("time"));	/* the parent again, updated (not
					 * a number: LED came before Time
					 * and "2" showed LED) */
	}
	return 1;
}

int time_fn(int op, int row, void *buf)
{
	switch (op) {
	case SG_LOAD: load(); break;
	case SG_APPLY:
		if (row == R_DATE || row == R_TIME) return set_clock(row);
		break;				/* boot, Clock: nothing to do */
	case SG_LEAVE: leave(); break;
	case SG_TEXT:
		strncpy((char *) buf, tz_label(zone), SG_TEXTMAX - 1);
		((char *) buf)[SG_TEXTMAX - 1] = 0;
		break;
	case SG_OPEN: zones(zone); break;
	case SG_EVENT: return event(row, (char **) buf);
	}
	return 0;
}
