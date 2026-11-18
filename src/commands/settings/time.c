/* Time (spec K.7, I.1): the date and time, the time zone and 12/24 h.
 *
 * The RTC keeps UTC; the rows show local time.  A date or time row moves
 * the clock at once (stime) by its step from the time now, so the minutes
 * that pass while the screen is open are not lost; hour and minute also
 * zero the seconds, as on a watch.  The zone comes from the const table
 * shared with the first-boot wizard (zones.c); changing it only changes
 * what this screen shows until Back, which writes /etc/TZ once (through
 * a new file and rename, R18b.3) and gives INIT SIGHUP (the zone of what
 * it starts) and SIGRTC (timed re-arms the local-time timers).  "other":
 * a rule not in the table, kept as it is.  Clock (12/24 h) is saved in
 * /etc/flipper/time.conf for the clock apps.  None of it is applied at
 * boot: the RTC runs on, and INIT reads /etc/TZ itself. */
#include <sys/types.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "sgroup.h"
#include "zones.h"

#define PATH_TZ		"/etc/TZ"

enum { R_YEAR, R_MON, R_DAY, R_HOUR, R_MIN, R_ZONE, R_CLOCK };

static const char * const clock_name[] = { "24 h", "12 h" };

static struct srow rows[] = {
	{ "Year", S_INT, S_NOSAVE, 2024, 2099, 1, "", 0, 2026 },
	{ "Month", S_INT, S_NOSAVE, 1, 12, 1, "", 0, 1 },
	{ "Day", S_INT, S_NOSAVE, 1, 31, 1, "", 0, 1 },
	{ "Hour", S_INT, S_NOSAVE, 0, 23, 1, "", 0, 0 },
	{ "Minute", S_INT, S_NOSAVE, 0, 59, 1, "", 0, 0 },
	{ "Zone", S_ENUM, S_NOSAVE, 0, 0, 1, 0, 0, 0 },	/* label(): zones.c */
	{ "Clock", S_ENUM, 0, 0, 1, 1, 0, clock_name, 0 },
};

static int shown[R_MIN + 1];		/* the date and time rows as drawn */
static int nzones;			/* tz_count(): "other" is that one */
static int zone0;			/* the zone in /etc/TZ */
static char rule0[TZ_RULE_MAX];		/* its rule ("other": kept) */
static char tzenv[3 + TZ_RULE_MAX] = "TZ=";

static void use_zone(const char *rule)
{
	strcpy(tzenv + 3, rule[0] != 0 ? rule : "UTC0");
	putenv(tzenv);
}

/* The rows from the clock now, in this screen's zone. */
static void show_time(void)
{
	time_t now = time((time_t *) 0);
	struct tm *tm = localtime(&now);
	int i;

	rows[R_YEAR].val = tm->tm_year + 1900;
	rows[R_MON].val = tm->tm_mon + 1;
	rows[R_DAY].val = tm->tm_mday;
	rows[R_HOUR].val = tm->tm_hour;
	rows[R_MIN].val = tm->tm_min;
	for (i = 0; i <= R_MIN; i++) shown[i] = rows[i].val;
}

static void load(struct sgroup *g)
{
	char buf[96], label[TZ_LABEL_MAX];
	int fd, n = -1;

	(void) g;
	nzones = tz_count();
	rows[R_ZONE].max = (short) nzones;	/* the last one: "other" */
	rule0[0] = 0;
	if ((fd = open(PATH_TZ, O_RDONLY)) >= 0) {
		n = (int) read(fd, buf, sizeof buf - 1);
		close(fd);
	}
	if (n < 0 || (buf[n] = 0, tz_parse(buf, rule0, label) < 0)) {
		strcpy(rule0, "UTC0");		/* no /etc/TZ: UTC */
		label[0] = 0;
	}
	zone0 = tz_find(label, rule0);
	rows[R_ZONE].val = zone0;
	use_zone(rule0);
	show_time();
}

/* A date or time row moved by d from what was shown: the clock too. */
static void set_clock(int row, int d)
{
	time_t now = time((time_t *) 0), t;
	struct tm tm = *localtime(&now);
	int md;

	switch (row) {
	case R_YEAR:	tm.tm_year += d; break;
	case R_MON:	tm.tm_mon += d; break;
	case R_DAY:	tm.tm_mday += d; break;
	case R_HOUR:	tm.tm_hour += d; tm.tm_sec = 0; break;
	default:	tm.tm_min += d; tm.tm_sec = 0; break;
	}
	if (row == R_YEAR || row == R_MON) {	/* 31 Jan -> 28 Feb, not 3 Mar */
		md = tz_mdays(tm.tm_year + 1900, tm.tm_mon + 1);
		if (tm.tm_mday > md) tm.tm_mday = md;
	}
	if (row == R_DAY) {			/* within the month, no wrap */
		md = tz_mdays(tm.tm_year + 1900, tm.tm_mon + 1);
		if (tm.tm_mday < 1 || tm.tm_mday > md) return;
	}
	tm.tm_isdst = -1;
	if ((t = mktime(&tm)) != (time_t) -1) (void) stime(&t);
}

static void apply(struct sgroup *g, int row)
{
	(void) g;
	if (row < 0) return;			/* boot: nothing to do */
	if (row <= R_MIN) set_clock(row, rows[row].val - shown[row]);
	else if (row == R_ZONE)
		use_zone(rows[R_ZONE].val < nzones
			? tz_rule(rows[R_ZONE].val) : rule0);
	else return;				/* Clock: the file on Back */
	show_time();
}

/* Back: /etc/TZ only when another zone of the table was chosen. */
static void leave(struct sgroup *g)
{
	char buf[TZ_LABEL_MAX + TZ_RULE_MAX + 8];
	int z = rows[R_ZONE].val, fd, n, ok;

	(void) g;
	if (z == zone0 || z >= nzones) return;
	if ((n = tz_format(buf, (int) sizeof buf, z)) < 0) return;
	if ((fd = open(PATH_TZ ".new", O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
		ok = 0;
	else {
		ok = write(fd, buf, (size_t) n) == n;
		ok = close(fd) == 0 && ok;
	}
	if (!ok || rename(PATH_TZ ".new", PATH_TZ) < 0) {
		(void) unlink(PATH_TZ ".new");
		fprintf(stderr, "settings: cannot write %s\n", PATH_TZ);
		return;
	}
	(void) kill(1, SIGHUP);			/* INIT: TZ for what it starts */
	(void) kill(1, SIGRTC);			/* timed: the local timers */
}

struct sgroup time_group = {
	"time", "TIME", rows, 7, load, apply, leave, tz_label
};
