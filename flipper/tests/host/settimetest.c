/* settimetest - settings time and about (spec K.7, T24b, T24d) on the PC:
 * the zone table, /etc/TZ and the local time of a rule without the libc
 * (src/commands/settings/zones.c), the About rows' text (afmt.c), a
 * group's file (conf.c, with the Display and Sound rows).
 *
 * Every rule of the table must be one the ACK libc parses (alphabetic
 * names of 3+ letters, hours below 24); the host's libc then checks a few
 * of them against known offsets in winter and summer. */
#define _DEFAULT_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../../src/commands/settings/zones.h"
#include "../../../src/commands/settings/afmt.h"
#include "../../../src/commands/settings/sgroup.h"

/* Time and About for groups.c: only their rows matter here. */
const struct srow time_rows[] = {
	{ "Date", S_DATE, S_NOSAVE, 0, 0, 0, "", "" },
	{ "Time", S_TIME, S_NOSAVE, 0, 0, 0, "", "" },
	{ "Zone", S_ITEM, S_NOSAVE, 0, 0, 0, "", "" },
	{ "Clock", S_ENUM, 0, 0, 1, 1, "", "24 h|12 h" },
};
const struct srow about_rows[8];
int time_fn(int op, int row, void *buf) { (void) op; (void) row; (void) buf; return 0; }
int about_fn(int op, int row, void *buf) { (void) op; (void) row; (void) buf; return 0; }

static int fails, checks;

static void check(int ok, const char *what)
{
	checks++;
	if (!ok) {
		fails++;
		printf("FAIL %s\n", what);
	}
}

/* The ACK parser's limits (src/lib/ansi/misc.c parseTZ). */
static int ack_name(const char **pp)
{
	const char *p = *pp;
	int n = 0;

	while (*p && !isdigit((unsigned char) *p) && *p != ',' && *p != '-'
							&& *p != '+') {
		if (!isalpha((unsigned char) *p)) return 0;
		p++;
		n++;
	}
	*pp = p;
	return n >= 3 && n <= 10;
}

static int ack_time(const char **pp)
{
	const char *p = *pp;
	int n = 0, d = 0;

	while (isdigit((unsigned char) *p)) n = n * 10 + *p++ - '0', d++;
	if (d == 0 || n >= 24) return 0;
	while (*p == ':') {
		p++;
		for (n = 0, d = 0; isdigit((unsigned char) *p); d++)
			n = n * 10 + *p++ - '0';
		if (d == 0 || n >= 60) return 0;
	}
	*pp = p;
	return 1;
}

static int ack_rule(const char *p)
{
	int k;

	if (!ack_name(&p)) return 0;
	if (*p == '-' || *p == '+') p++;
	if (!ack_time(&p)) return 0;
	if (*p == 0) return 1;
	if (!ack_name(&p)) return 0;
	for (k = 0; k < 2; k++) {	/* ,Mm.w.d[/time] twice */
		if (*p++ != ',' || *p++ != 'M') return 0;
		while (isdigit((unsigned char) *p) || *p == '.') p++;
		if (*p == '/') {
			p++;
			if (!ack_time(&p)) return 0;
		}
	}
	return *p == 0;
}

/* Offset east of UTC in minutes for rule at the UTC instant t. */
static long offset(const char *rule, time_t t)
{
	struct tm l;

	setenv("TZ", rule, 1);
	tzset();
	l = *localtime(&t);
	return (long) (timegm(&l) - t) / 60;
}

static void zone_offset(const char *label, long jan, long jul)
{
	char what[96];
	int i = tz_find(label, NULL);
	time_t tj = 1768478400;		/* 2026-01-15 12:00 UTC */
	time_t tl = 1784116800;		/* 2026-07-15 12:00 UTC */

	snprintf(what, sizeof what, "%s: %ld/%ld min", label, jan, jul);
	check(i < tz_count() && offset(tz_rule(i), tj) == jan
		&& offset(tz_rule(i), tl) == jul, what);
}

int main(void)
{
	char rule[TZ_RULE_MAX], label[TZ_LABEL_MAX], buf[128], v[24];
	unsigned char uid[8] = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0 };
	int i, j, n = tz_count(), ok;

	/* the table */
	check(n >= 60 && n <= 100, "about 80 zones");
	check(strcmp(tz_label(0), "UTC") == 0 && strcmp(tz_rule(0), "UTC0") == 0,
		"entry 0 is UTC");
	check(strcmp(tz_label(n), "other") == 0 && tz_rule(n)[0] == 0,
		"past the end: other, no rule");
	for (ok = 1, i = 0; i < n; i++) {
		if (!ack_rule(tz_rule(i))) {
			printf("  rule not for the ACK libc: %s %s\n",
				tz_label(i), tz_rule(i));
			ok = 0;
		}
		if (strlen(tz_rule(i)) >= TZ_RULE_MAX
				|| strlen(tz_label(i)) >= TZ_LABEL_MAX
				|| strlen(tz_label(i)) > 22)
			ok = 0;		/* "[label]" fits the row (24 chars) */
		for (j = 0; j < i; j++)
			if (strcmp(tz_label(i), tz_label(j)) == 0) ok = 0;
	}
	check(ok, "every rule parses in the ACK libc, labels fit and differ");

	zone_offset("UTC", 0, 0);
	zone_offset("Europe/London", 0, 60);
	zone_offset("Europe/Warsaw", 60, 120);
	zone_offset("Europe/Helsinki", 120, 180);
	zone_offset("Europe/Moscow", 180, 180);
	zone_offset("Atlantic/Azores", -60, 0);
	zone_offset("Asia/Tehran", 210, 210);
	zone_offset("Asia/Kolkata", 330, 330);
	zone_offset("Asia/Kathmandu", 345, 345);
	zone_offset("Asia/Tokyo", 540, 540);
	zone_offset("Australia/Adelaide", 630, 570);
	zone_offset("Australia/Sydney", 660, 600);
	zone_offset("Pacific/Auckland", 780, 720);
	zone_offset("America/St_Johns", -210, -150);
	zone_offset("America/New_York", -300, -240);
	zone_offset("America/Los_Angeles", -480, -420);
	zone_offset("America/Phoenix", -420, -420);
	zone_offset("America/Sao_Paulo", -180, -180);

	/* /etc/TZ */
	i = tz_find("Europe/Warsaw", NULL);
	check(tz_format(buf, sizeof buf, i) > 0
		&& strcmp(buf, "# Europe/Warsaw\nCET-1CEST,M3.5.0,M10.5.0/3\n") == 0,
		"format: label comment, then the rule");
	check(tz_parse(buf, rule, label) == 0 && strcmp(rule, tz_rule(i)) == 0
		&& strcmp(label, "Europe/Warsaw") == 0 && tz_find(label, rule) == i,
		"parse(format(i)) gives i back");
	check(tz_format(buf, 20, i) < 0, "format: too small a buffer");
	check(tz_format(buf, sizeof buf, n) < 0, "format: other has no file");
	check(tz_parse("CET-1CEST,M3.5.0,M10.5.0/3\n", rule, label) == 0
		&& label[0] == 0 && tz_find(label, rule) == tz_find("Europe/Madrid", NULL),
		"a bare rule: the first zone with it");
	check(tz_parse("#Europe/Paris\r\n\n  CET-1CEST,M3.5.0,M10.5.0/3  \r\n", rule,
		label) == 0 && strcmp(label, "Europe/Paris") == 0
		&& tz_find(label, rule) == tz_find("Europe/Paris", NULL),
		"blanks, CR and empty lines");
	check(tz_parse("# Mars/Olympus\nMST7\n", rule, label) == 0
		&& tz_find(label, rule) == tz_find("America/Phoenix", NULL),
		"unknown label, known rule");
	check(tz_parse("# Europe/Warsaw\nXYZ-3\n", rule, label) == 0
		&& tz_find(label, rule) == n, "a known label with another rule: other");
	check(tz_parse("# only a comment\n", rule, label) < 0, "no rule line");
	check(tz_parse("", rule, label) < 0, "empty file");
	memset(buf, 'A', 60);
	buf[60] = 0;
	check(tz_parse(buf, rule, label) < 0, "a rule too long");

	/* the calendar */
	check(tz_mdays(2024, 2) == 29 && tz_mdays(2026, 2) == 28
		&& tz_mdays(2100, 2) == 28 && tz_mdays(2000, 2) == 29
		&& tz_mdays(2026, 4) == 30 && tz_mdays(2026, 12) == 31
		&& tz_mdays(2026, 13) == 0, "days in a month");

	/* local time without the libc: every zone, 2024-2031, against the
	 * host's libc; the way back (tz_utc) outside the repeated hour */
	{
		long t, o, bad = 0, back = 0;
		int y, m, d;

		for (i = 0; i < n; i++)
			for (t = 1704067200L; t < 1924992000L; t += 3600L * 5 + 37) {
				o = offset(tz_rule(i), (time_t) t) * 60;
				if (tz_off(tz_rule(i), t) != o) {
					if (bad++ < 5)
						printf("  %s at %ld: %ld, libc %ld\n",
							tz_rule(i), t,
							tz_off(tz_rule(i), t), o);
				}
				if (tz_utc(tz_rule(i), t + o) != t	/* the
					 * repeated hour can be either */
				    && offset(tz_rule(i), (time_t) t - 3600) * 60 == o
				    && offset(tz_rule(i), (time_t) t + 3600) * 60 == o)
					back++;
			}
		for (i = 0; i < n; i++)		/* after 2038: 64-bit time_t */
			for (t = 2240611200L; t < 2272147200L; t += 3600L * 23 + 7)
				if (tz_off(tz_rule(i), (time_t) t)
				    != offset(tz_rule(i), (time_t) t) * 60) bad++;
		check(bad == 0, "tz_off: every zone's offset as the libc's (2024-2031, 2041)");
		check(back == 0, "tz_utc: local time back to UTC");
		check(tz_off("CET-1CEST", 1784116800L) == 7200
			&& tz_off("CET-1CEST", 1768478400L) == 3600,
			"a DST name without dates: the default rule");
		check(tz_off("<+03>-3", 0L) == 10800 && tz_off("UTC0", 5L) == 0
			&& tz_off("", 5L) == 0, "<+03>, UTC, empty");
		check(tz_off("EST5EDT,J60/2,J300/2", 1784116800L) == -4 * 3600
			&& tz_off("EST5EDT,59/2,299/2", 1768478400L) == -5 * 3600,
			"Jn and n change dates");
		for (ok = 1, t = -800; t < 60000; t++) {
			tz_date(t, &y, &m, &d);
			if (tz_days(y, m, d) != t || d < 1 || d > tz_mdays(y, m))
				ok = 0;
		}
		check(ok && tz_days(1970, 1, 1) == 0 && tz_days(2026, 11, 18) == 20775,
			"tz_days / tz_date both ways");
	}

	/* a group's file: Display (Light is never saved), Time (Clock) */
	{
		char f[200];

		for (i = 0; i < SG_ROWS; i++) sg_val[i] = -1;
		strcpy(f, "Brightness: 60\nTimeout: 1 min\r\nLight: on\n"
			"junk\nBrightness: 7\nTimeout: 3 min\n");
		conf_parse(sg_find("display"), f);
		check(sg_val[0] == 60 && sg_val[1] == 4 && sg_val[2] == -1,
			"conf: numbers, options, out of range and NOSAVE ignored");
		sg_val[0] = 100; sg_val[1] = 7; sg_val[2] = 2;
		check(conf_format(sg_find("display"), f, sizeof f) > 0
			&& strcmp(f, "Brightness: 100\nTimeout: never\n") == 0,
			"conf: format, without the forced light");
		check(conf_format(sg_find("display"), f, 10) < 0,
			"conf: format, too small a buffer");
		for (i = 0; i < SG_ROWS; i++) sg_val[i] = -1;
		strcpy(f, "Clock: 12 h\nDate: 2026-01-01\n");
		conf_parse(sg_find("time"), f);
		check(sg_val[3] == 1 && sg_val[0] == -1, "conf: Clock, Date not kept");
		strcpy(f, "Volume: 30x\nVolume: -10\nVolume:\n");
		sg_val[0] = -1;
		conf_parse(sg_find("sound"), f);
		check(sg_val[0] == -1, "conf: malformed numbers ignored");
		check(sg_find("about") == 3 && sg_find("led") < 0, "groups by name");
		{
			int len;
			const char *o = sg_opt("a|bb|c", 1, &len);

			check(o != NULL && len == 2 && strncmp(o, "bb", 2) == 0
				&& sg_opt("a|bb|c", 3, &len) == NULL,
				"sg_opt: option k, none past the end");
		}
	}

	/* About */
	check(af_field("Version: 4.07\n", "Version", v, sizeof v)
		&& strcmp(v, "4.07") == 0, "field: Version");
	check(!af_field("Versions: 1\n", "Version", v, sizeof v)
		&& !af_field("Package: base\n", "Version", v, sizeof v),
		"field: other keys");
	check(af_field("Version:\t1.2.3-long-long-long-long\n", "Version", v, 8)
		&& strcmp(v, "1.2.3-l") == 0, "field: cut to the buffer");
	af_ram(v, 146432L, 122880L);
	check(strcmp(v, "143 KB (hole 120)") == 0, "free RAM row");
	af_lib(v, 21000UL, 612UL);
	check(strcmp(v, "21 KB, 612 B each") == 0, "libc row");
	af_lib(v, 4294967295UL, 4294967295UL);
	check(strlen(v) < 24, "library row: never past 24 bytes");
	af_ram(v, 2147483647L, 2147483647L);
	check(strlen(v) < 24, "RAM row: never past 24 bytes");
	af_hex(v, uid, 8);
	check(strcmp(v, "123456789ABCDEF0") == 0, "serial in hex");

	printf("settimetest: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
