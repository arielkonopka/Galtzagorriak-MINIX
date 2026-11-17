/* test_app.c - host tests of libapp (spec R7b.4): the timer rules
 * (AC7b.1.1, AC7b.1.3, AC7b.1.4), UTC and a local zone with daylight
 * saving (Phase 2d), the file format, the spool and settings.
 *	make -C flipper/lib/app test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "app.h"

static int fails, checks;

#define CHECK(c, what) check((c) != 0, what, __LINE__)
static void check(int ok, const char *what, int line)
{
	checks++;
	if (!ok) {
		fails++;
		printf("FAIL line %d: %s\n", line, what);
	}
}

static void zone(const char *tz)
{
	setenv("TZ", tz, 1);
	tzset();
}

/* 2026-11-16 is a Monday. */
static time_t utc(int y, int mo, int d, int h, int mi, int s)
{
	struct tm tm;
	char *old = getenv("TZ"), save[64];

	strncpy(save, old ? old : "UTC0", sizeof save - 1);
	save[sizeof save - 1] = 0;
	zone("UTC0");
	memset(&tm, 0, sizeof tm);
	tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
	tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s;
	tm.tm_isdst = 0;
	{
		time_t t = mktime(&tm);
		zone(save);
		return t;
	}
}

static void parse(struct app_timer *t, const char *text)
{
	char buf[APP_FILE_MAX];

	strcpy(buf, text);
	strcpy(t->id, "t");
	if (timer_parse(t, buf) != 0) {
		printf("FAIL: does not parse:\n%s", text);
		fails++;
	}
}

static const char water[] =
	"Filetype: Galtzagorriak Timer\nVersion: 1\nKind: interval\n"
	"Days: Mo Tu We Th Fr\nStart: 09:00\nEnd: 17:00\nEvery: 30\n"
	"Category: reminder\nMissed: skip\nAction: notify\n"
	"Title: Water\nText: Drink a glass of water\nSnooze: 5\n";

/* AC7b.1.1: every 30 min from 09:00 to 17:00 on weekdays. */
static void interval_week(const char *tz, int off_h)
{
	struct app_timer t;
	time_t c, start = utc(2026, 11, 16, 0, 0, 0), end;
	int n, incl, bad;

	zone(tz);
	for (incl = 0; incl <= 1; incl++) {
		parse(&t, water);
		t.incl = incl;
		end = start + 7 * 86400L;
		n = bad = 0;
		for (c = timer_next(&t, start); c != 0 && c < end;
						c = timer_next(&t, c)) {
			time_t local = c + off_h * 3600L;
			int wd = (int) ((local / 86400 + 4) % 7);	/* 1970-01-01: Thu */
			int m = (int) (local % 86400 / 60);

			if (wd == 0 || wd == 6 || m < 540 || m > 1020
			    || (m == 1020 && !incl) || m % 30 != 0) bad++;
			n++;
		}
		CHECK(bad == 0, "interval: only weekdays, inside the window");
		CHECK(n == (incl ? 85 : 80), "interval: 16 (17 inclusive) a day");
		CHECK(timer_next(&t, start) == utc(2026, 11, 16, 9 - off_h, 0, 0),
			"interval: first at Monday 09:00 local");
		c = utc(2026, 11, 20, 16 - off_h, 30, 0);	/* Friday */
		CHECK(timer_next(&t, c) == (incl ? c + 1800
				: utc(2026, 11, 23, 9 - off_h, 0, 0)),
			"interval: after Friday 16:30 (17:00) Monday 09:00");
		CHECK(timer_prev(&t, utc(2026, 11, 21, 12, 0, 0))
			== utc(2026, 11, 20, (incl ? 17 : 16) - off_h,
							incl ? 0 : 30, 0),
			"interval: prev from Saturday is Friday's last");
		CHECK(timer_prev(&t, utc(2026, 11, 17, 9 - off_h, 45, 0))
			== utc(2026, 11, 17, 9 - off_h, 30, 0),
			"interval: prev inside the window");
		CHECK(timer_prev(&t, utc(2026, 11, 17, 9 - off_h, 30, 0))
			== utc(2026, 11, 17, 9 - off_h, 30, 0),
			"interval: prev at an occurrence is itself");
	}
}

/* next and prev agree, over two weeks in steps of 7 min 13 s. */
static void sweep(const char *tz, const char *text)
{
	struct app_timer t;
	time_t s, n, p;
	int bad = 0;

	zone(tz);
	parse(&t, text);
	for (s = utc(2026, 3, 22, 0, 0, 0); s < utc(2026, 4, 5, 0, 0, 0);
								s += 433) {
		n = timer_next(&t, s);
		p = timer_prev(&t, s);
		if (n <= s || p > s || p == 0 || timer_prev(&t, n) != n
		    || timer_next(&t, p) != n || timer_prev(&t, n - 1) != p)
			bad++;
	}
	CHECK(bad == 0, "next/prev agree over two weeks");
}

static void daily_dst(void)
{
	struct app_timer t;

	zone("CET-1CEST,M3.5.0,M10.5.0/3");
	parse(&t, "Kind: daily\nTime: 08:00\nDays: all\nCategory: alarm\n");
	CHECK(timer_next(&t, utc(2026, 3, 28, 12, 0, 0))
		== utc(2026, 3, 29, 6, 0, 0),
		"daily 08:00 local: CEST after the spring change (06:00 UTC)");
	CHECK(timer_next(&t, utc(2026, 3, 27, 12, 0, 0))
		== utc(2026, 3, 28, 7, 0, 0), "daily 08:00 local: CET before");
	CHECK(timer_prev(&t, utc(2026, 10, 25, 7, 30, 0))
		== utc(2026, 10, 25, 7, 0, 0),
		"daily: prev on the autumn change day (CET again)");
	parse(&t, "Kind: daily\nTime: 07:15\nDays: Sa\n");
	CHECK(timer_next(&t, utc(2026, 11, 16, 0, 0, 0))
		== utc(2026, 11, 21, 6, 15, 0), "daily: only on Saturday");
	zone("UTC0");
	CHECK(timer_next(&t, utc(2026, 11, 21, 7, 15, 0))
		== utc(2026, 11, 28, 7, 15, 0), "daily: a week later in UTC");
}

/* AC7b.1.4: the clock moves +2 h at 10:00:05 on a Monday. */
static void missed(void)
{
	struct app_timer t;
	time_t last = utc(2026, 11, 16, 10, 0, 5);

	zone("UTC0");
	parse(&t, water);
	CHECK(timer_due(&t, last, utc(2026, 11, 16, 12, 10, 0)) == TD_SKIP,
		"+2 h, Missed: skip: nothing fires");
	t.missed = 1;
	CHECK(timer_due(&t, last, utc(2026, 11, 16, 12, 10, 0)) == TD_LATE,
		"+2 h, Missed: fire-once: fires once");
	CHECK(timer_due(&t, last, utc(2026, 11, 16, 12, 0, 30)) == TD_FIRE,
		"+2 h onto 12:00: that one is on time");
	CHECK(timer_due(&t, last, utc(2026, 11, 16, 10, 20, 0)) == TD_NONE,
		"nothing between 10:00 and 10:20");
	CHECK(timer_due(&t, utc(2026, 11, 16, 10, 29, 59),
		utc(2026, 11, 16, 10, 30, 1)) == TD_FIRE, "alarm on time");
	CHECK(timer_next(&t, utc(2026, 11, 16, 12, 10, 0))
		== utc(2026, 11, 16, 12, 30, 0), "next after the jump");
	parse(&t, "Kind: once\nAt: 1790000000\nMissed: fire-once\n");
	CHECK(timer_due(&t, 1789999990, 1790000001) == TD_FIRE, "once: fires");
	CHECK(timer_due(&t, 1790000000, 1790000100) == TD_NONE,
		"once: not twice");
	CHECK(timer_due(&t, 1700000000, 1790090000) == TD_LATE,
		"once after power-off: fire-once");
	CHECK(timer_next(&t, 1790000000) == 0, "once: no next");
}

/* AC7b.1.3: do-not-disturb 22:00-07:00 holds reminders, not alarms. */
static void policy(void)
{
	struct notify_policy p;
	char conf[] = "# test\nVolume: 0\nVibro: yes\nLed: no\nDnd: 22:00-07:00\n";
	time_t night = utc(2026, 11, 16, 23, 0, 0);
	time_t morning = utc(2026, 11, 17, 6, 59, 0);
	time_t day = utc(2026, 11, 17, 7, 0, 0);

	zone("UTC0");
	notify_policy(&p, NULL);		/* defaults: no files here */
	CHECK(p.volume == 80 && p.vibro && p.led && p.dnd_from < 0,
		"policy defaults");
	notify_parse(&p, conf);
	CHECK(p.dnd_from == 1320 && p.dnd_to == 420, "Dnd: parsed");
	CHECK(notify_flags(&p, TC_REMINDER, NF_ALL, night) == 0,
		"reminder at 23:00: held back");
	CHECK(notify_flags(&p, TC_INFO, NF_ALL, morning) == 0,
		"info at 06:59: held back");
	CHECK(notify_flags(&p, TC_REMINDER, NF_ALL, day) == (NF_SHOW | NF_VIBRO),
		"reminder at 07:00: shown, no sound (Volume 0), no LED");
	CHECK(notify_flags(&p, TC_ALARM, NF_ALL, night) == (NF_SHOW | NF_ALL),
		"alarm at 23:00: rings with sound despite Volume 0");
	CHECK(notify_flags(&p, TC_ALARM, NF_VIBRO, night) == (NF_SHOW | NF_VIBRO),
		"vibro-only alarm: only vibrates");
	zone("CET-1CEST,M3.5.0,M10.5.0/3");
	CHECK(notify_flags(&p, TC_REMINDER, NF_ALL, utc(2026, 11, 16, 21, 30, 0))
		== 0, "Dnd in local time (22:30 CET)");
}

static void format(void)
{
	struct app_timer t, u;
	char buf[APP_FILE_MAX];
	static const char *const bad[] = {
		"Kind: weekly\n", "Kind: interval\nStart: 09:00\nEnd: 08:00\nEvery: 5\n",
		"Kind: interval\nStart: 09:00\nEnd: 10:00\nEvery: 0\n",
		"Kind: daily\nTime: 24:10\n", "Kind: daily\nTime: 7\n",
		"Kind: daily\nTime: 07:00\nDays: Xx\n", "Kind: once\nAt: 12ab\n",
		"Kind: once\nAt: 5\nAction: run\nProgram: ../bin/sh\n",
		"Kind: once\nAt: 5\nCategory: urgent\n", "Kind: once\nAt: 5\nSnooze: 99\n",
		"Kind: once\nAt: 5\nSound: maybe\n", "Kind: once\n",
		"Kind: once\nAt: 5\nAction: run\n",
	};
	unsigned i;

	parse(&t, water);
	t.missed = 1;
	CHECK(timer_format(&t, buf, sizeof buf) > 0, "format");
	strcpy(u.id, "t");
	CHECK(timer_parse(&u, buf) == 0 && memcmp(&t, &u, sizeof t) == 0,
		"format and parse again: the same timer");
	CHECK(t.days == 0x3E && t.every == 30 && t.start == 540 && t.end == 1020
		&& t.cat == TC_REMINDER && t.snooze == 5 && t.flags == NF_ALL,
		"fields");
	parse(&t, "Kind: once\nAt: 1790000000\nAction: run\nProgram: /usr/bin/x\n"
		  "Category: alarm\nVibro: no\nDays: weekend\n");
	CHECK(timer_format(&t, buf, sizeof buf) > 0, "format once/run");
	CHECK(timer_parse(&u, buf) == 0 && u.at == 1790000000 && u.action == TA_RUN
		&& strcmp(u.prog, "/usr/bin/x") == 0 && u.cat == TC_ALARM,
		"once/run fields");
	CHECK(timer_format(&t, buf, 40) == -1, "format: too small -> -1");
	for (i = 0; i < sizeof bad / sizeof *bad; i++) {
		strcpy(buf, bad[i]);
		if (timer_parse(&u, buf) != -1) {
			printf("FAIL: accepted:\n%s", bad[i]);
			fails++;
		}
		checks++;
	}
	CHECK(app_id_ok("water-1") && !app_id_ok(".last") && !app_id_ok("a/b")
		&& !app_id_ok("fifteen-chars-x") && !app_id_ok(""),
		"timer ids");
	CHECK(ff_hm("9:05") == 545 && ff_hm("23:59") == 1439
		&& ff_hm("12:60") == -1 && ff_hm("ab") == -1, "HH:MM");
}

static int count_cb(struct app_timer *t, void *arg)
{
	(*(int *) arg)++;
	return 0;
}

static void spool(void)
{
	struct app_timer t, u;
	char val[16];
	int n = 0;

	if (system("rm -rf " APP_ROOT " && mkdir -p " APP_TIMERS
			" " APP_DATA) != 0) fails++;
	parse(&t, water);
	strcpy(t.id, "water");
	CHECK(timer_add(&t) == 0, "timer_add");
	CHECK(timer_read("water", &u) == 0 && u.every == 30, "timer_read");
	{
		struct stat st;
		CHECK(stat(APP_TIMERS "/water", &st) == 0
			&& (st.st_mode & 0777) == 0600, "timer file mode 0600");
	}
	strcpy(t.id, "two");
	CHECK(timer_add(&t) == 0, "timer_add 2");
	CHECK(timer_list(count_cb, &n) == 2 && n == 2, "timer_list");
	CHECK(timer_remove("two") == 0 && timer_list(NULL, NULL) == 1,
		"timer_remove");
	strcpy(t.id, "../x");
	CHECK(timer_add(&t) == -1, "timer_add: bad id refused");
	CHECK(settings_set("remind", "Count", "3") == 0
		&& settings_set("remind", "Sound", "yes") == 0
		&& settings_set("remind", "Count", "4") == 0, "settings_set");
	CHECK(settings_get("remind", "Count", val, sizeof val) == 0
		&& strcmp(val, "4") == 0 && settings_get("remind", "Sound", val,
		sizeof val) == 0 && strcmp(val, "yes") == 0, "settings_get");
	CHECK(settings_get("remind", "None", val, sizeof val) == -1,
		"settings_get: missing key");
	CHECK(settings_set("remind", "A:b", "1") == -1
		&& settings_set("remind", "A", "1\nB: 2") == -1,
		"settings_set: refuses ':' in keys, newlines in values");
}

int main(void)
{
	interval_week("UTC0", 0);
	interval_week("CET-1CEST,M3.5.0,M10.5.0/3", 1);
	sweep("UTC0", water);
	sweep("CET-1CEST,M3.5.0,M10.5.0/3", water);
	sweep("CET-1CEST,M3.5.0,M10.5.0/3",
	      "Kind: interval\nStart: 01:00\nEnd: 04:00\nEvery: 20\nInclusive: yes\n");
	sweep("CET-1CEST,M3.5.0,M10.5.0/3", "Kind: daily\nTime: 02:30\nDays: Su We\n");
	daily_dst();
	missed();
	policy();
	format();
	spool();
	printf("libapp: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
