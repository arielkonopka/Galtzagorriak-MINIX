/* SPDX-License-Identifier: GPL-3.0-or-later
 * remind - Kanpaia, the reminders (Galtzagorriak MINIX, spec 7b.1, F.4),
 * a window app (spec K.8): the launcher draws, the app only describes.
 *
 * Usage:	remind		the app (started by the launcher: "=" before
 *				the program in /etc/apps, fd 3 and 4)
 *		remind -s	today's count and the last 7 days, as text
 *
 * Any number of reminders (up to RMAX), each one timer in /var/timers
 * (libapp, category reminder, id remind-N, Log: remind): once at a date
 * and time, daily at a time, or every N minutes in a window (09:00-17:00)
 * on chosen weekdays; sound, vibro, LED, snooze.  Nothing of it is in RAM
 * while the app is closed: timed fires the timer and alert shows it; OK
 * on the alert ("done") appends "TIME remind-N" to APP_DATA/remind/log.
 *
 * Windows: main (Today, Upcoming, History, + New reminder); Today and
 * Upcoming list the reminders by their next time, History the log newest
 * first (paged: the app reads one page from the files, its RAM does not
 * grow with the data); OK on a reminder opens its form (text on the
 * desktop's keyboard, repeat, date, time, until, every, days, snooze,
 * sound/vibro/LED, Save, Delete).  Back on the form cancels it.
 *
 * RAM (spec K.8, ACK8.3 <= 4.0 KB open): no libfuri, no stdio, no malloc,
 * no opendir (its 1 KB DIR): /var/timers is read in 16 B entries.  The
 * one struct app_timer in .bss is the form, and the scratch of the lists
 * while no form is open.  libgm and libapp are linked statically.
 *
 * The launcher starts it as the user remind (group timers, /etc/apps
 * "Kanpaia:remind:@bell:=/usr/bin/remind"); started as root (an older
 * line, remind -s from a root shell) it becomes that user first.
 */
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "app.h"
#include "gm.h"
#include "nat.h"

#define APP	"remind"
#define PREFIX	"remind-"
#define PLEN	(sizeof PREFIX - 1)
#define RMAX	32		/* reminders the lists show (and New makes) */
#define NMAX	99		/* remind-1 .. remind-99 */
#define PAGE	8		/* rows of a page of a long list */
#define HMAX	64		/* History: the newest log lines */
#define TMAX	40		/* the text a person types */
#define NEVER	LONG_MAX	/* sorts last */

static const char snoozes[] = { 0, 5, 10, 15, 30 };
#define SNOOZE_OPTS	"off|5 min|10 min|15 min|30 min"
#define KIND_OPTS	"once|daily|interval"

static struct app_timer t;	/* the form; the lists' scratch otherwise */
static char cur, par;		/* windows: m(ain) t(oday) u(pcoming)
				 * h(istory) e(dit); par: the form's parent */
static short lfirst;		/* the first row of the list's page */
static char is_new;
static const char *warn;	/* the form's title after a failed Save */
static time_t now;

/* ---- small things ---- */

static void put(int fd, const char *s)
{
	(void) write(fd, s, strlen(s));
}

static char *two(char *p, int v)
{
	p[0] = (char) ('0' + v / 10 % 10);
	p[1] = (char) ('0' + v % 10);
	return p + 2;
}

/* when: "HH:MM" today, "Tu HH:MM" this week, "11-24 HH:MM" later */
static char *when(char *b, time_t w)
{
	static const char wd[] = "SuMoTuWeThFrSa";
	long d = (long) (w - nat_midnight(now, 0));	/* (localtime too) */
	struct tm *tm = localtime(&w);
	char *p = b;

	if (d >= 7 * 86400L || d < 0) {
		p = two(p, tm->tm_mon + 1);
		*p++ = '-';
		p = two(p, tm->tm_mday);
		*p++ = ' ';
	} else if (d >= 86400L) {
		*p++ = wd[tm->tm_wday * 2];
		*p++ = wd[tm->tm_wday * 2 + 1];
		*p++ = ' ';
	}
	p = two(p, tm->tm_hour);
	*p++ = ':';
	p = two(p, tm->tm_min);
	*p = 0;
	return b;
}

/* "remind-N" with N 1..NMAX written plainly: N, else 0 */
static int idnum(const char *id)
{
	int n = 0;

	if (strncmp(id, PREFIX, PLEN) != 0 || id[PLEN] < '1' || id[PLEN] > '9')
		return 0;
	for (id += PLEN; *id >= '0' && *id <= '9' && n <= NMAX; id++)
		n = n * 10 + *id - '0';
	return *id == 0 && n <= NMAX ? n : 0;
}

static char *idof(char *b, int n)
{
	strcpy(b, PREFIX);
	nat_num(b + PLEN, (unsigned) n);
	return b;
}

/* Read reminder n into t: 0, or -1 (gone, or not ours). */
static int load(int n)
{
	char id[APP_ID_MAX + 1];

	if (timer_read(idof(id, n), &t) < 0 || strcmp(t.log, APP) != 0)
		return -1;
	return 0;
}

/* Every reminder: fn(n) for each remind-N in /var/timers, whose file is
 * then read only if fn wants (no opendir: entries of 16 B, one read
 * each).  The number of them. */
static int each(void (*fn)(int n, void *arg), void *arg)
{
	struct _v7_direct e;
	char name[sizeof e.d_name + 1];
	int fd, n, count = 0;

	if ((fd = open(APP_TIMERS, O_RDONLY)) < 0) return 0;
	while (read(fd, (char *) &e, sizeof e) == sizeof e) {
		if (e.d_ino == 0) continue;
		memcpy(name, e.d_name, sizeof e.d_name);
		name[sizeof e.d_name] = 0;
		if ((n = idnum(name)) == 0) continue;
		count++;
		if (fn != NULL) fn(n, arg);
	}
	close(fd);
	return count;
}

/* ---- the lists ---- */

struct sorted {
	int n;			/* entries */
	char which;		/* 't' today, 'u' upcoming */
	long key[RMAX];		/* the next time; NEVER - x: none */
	unsigned char num[RMAX];
};

/* Today: the reminders that ring today (next today, or rang today: those
 * last); Upcoming: all of them by their next time (none: last). */
static void sort_cb(int n, void *arg)
{
	struct sorted *s = arg;
	time_t nx, end = nat_midnight(now, 1), day = nat_midnight(now, 0);
	long k;
	int i;

	if (load(n) < 0) return;
	nx = timer_next(&t, now);
	if (s->which == 't') {
		if (nx != 0 && nx < end) k = (long) nx;
		else if (timer_prev(&t, now) >= day) k = NEVER - 1;
		else return;
	} else
		k = nx != 0 ? (long) nx : NEVER;
	for (i = s->n; i > 0 && (s->key[i - 1] > k
			|| (s->key[i - 1] == k && s->num[i - 1] > n)); i--) {
		if (i < RMAX) {
			s->key[i] = s->key[i - 1];
			s->num[i] = s->num[i - 1];
		}
	}
	if (i < RMAX) {
		s->key[i] = k;
		s->num[i] = (unsigned char) n;
		if (s->n < RMAX) s->n++;
	}
}

static void w_sorted(char which, int first)
{
	struct sorted s;
	char id[APP_ID_MAX + 1], b[16];
	int i;

	s.n = 0;
	s.which = which;
	now = time(NULL);
	(void) each(sort_cb, &s);
	if (first < 0 || first >= s.n) first = 0;
	gm_win(which == 't' ? "today" : "next", "list",
		which == 't' ? "Today" : "Upcoming", s.n > PAGE ? s.n : 0,
		s.n > PAGE ? first : 0);
	if (s.n == 0)
		gm_line(which == 't' ? "Nothing rings today." : "No reminders yet.");
	for (i = first; i < s.n && i < first + PAGE; i++) {
		if (load(s.num[i]) < 0) continue;
		if (s.key[i] == NEVER - 1) strcpy(b, "rang");
		else if (s.key[i] == NEVER) strcpy(b, "--");
		else when(b, (time_t) s.key[i]);
		gm_item(idof(id, s.num[i]), t.text, b);
	}
	gm_show();
	cur = which;
	lfirst = (short) first;
}

/* History: the log newest first, a page at a time.  The page's times
 * and ids first, then the texts: no timer file is read from inside the
 * log reader (its 256 B and timer_read()'s 552 B never on the stack
 * together). */
struct page {
	int n;
	time_t at[PAGE];
	unsigned char num[PAGE];
};

static void hist_cb(time_t at, const char *id, void *arg)
{
	struct page *pg = arg;

	if (pg->n < PAGE) {
		pg->at[pg->n] = at;
		pg->num[pg->n++] = (unsigned char) idnum(id);
	}
}

static void w_hist(int first)
{
	struct page pg;
	char id[APP_ID_MAX + 1], b[16];
	int fd, total, i;

	now = time(NULL);
	pg.n = 0;
	fd = open(APP_DATA "/" APP "/log", O_RDONLY);
	total = fd < 0 ? 0 : nat_log_back(fd, 0, HMAX, NULL, NULL);
	if (total < 0) total = 0;
	if (first < 0 || first >= total) first = 0;
	if (total > 0) (void) nat_log_back(fd, first, PAGE, hist_cb, &pg);
	if (fd >= 0) close(fd);
	gm_win("hist", "list", "History", total > PAGE ? total : 0,
		total > PAGE ? first : 0);
	if (total == 0) gm_line("Nothing done yet.");
	for (i = 0; i < pg.n; i++) {
		when(b, pg.at[i]);
		if (pg.num[i] == 0) gm_item("x", "(another app)", b);
		else if (load(pg.num[i]) == 0)
			gm_item(idof(id, pg.num[i]), t.text, b);
		else
			gm_item(idof(id, pg.num[i]), "(deleted)", b);
	}
	gm_show();
	cur = 'h';
	lfirst = (short) first;
}

static void counts(int *c)
{
	int fd;

	memset(c, 0, 7 * sizeof *c);
	if ((fd = open(APP_DATA "/" APP "/log", O_RDONLY)) >= 0) {
		(void) nat_log_days(fd, nat_midnight(time(NULL), 0), 7, c);
		close(fd);
	}
}

static void w_main(void)
{
	char b[16];
	int c[7], i, week;

	counts(c);
	for (week = i = 0; i < 7; i++) week += c[i];
	gm_win("main", "list", "Kanpaia", 0, 0);
	strcat(nat_num(b, (unsigned) c[6]), " done");
	gm_item("today", "Today", b);
	gm_item("next", "Upcoming", nat_num(b, (unsigned) each(NULL, NULL)));
	strcat(nat_num(b, (unsigned) week), " in 7 d");
	gm_item("hist", "History", b);
	gm_item("new", "+ New reminder", "");
	gm_show();
	cur = 'm';
}

static void w_list(char w, int first)
{
	if (w == 'h') w_hist(first);
	else if (w == 't' || w == 'u') w_sorted(w, first);
	else w_main();
}

/* ---- the form ---- */

/* libapp's days (bit 0 Sunday) <-> the desktop's (bit 0 Monday) */
static int days_out(int m)
{
	return (m >> 1 & 0x3F) | (m & 1) << 6;
}

static int days_in(int m)
{
	return (m << 1 & 0x7E) | (m >> 6 & 1);
}

/* the form's date while it is open (the time is in t.start) */
static short f_y, f_m, f_d;

static void w_form(void)
{
	int i, end = t.end >= 1440 ? 1439 : t.end;

	gm_win("edit", "list", warn != NULL ? warn : is_new ? "New reminder"
		: "Reminder", 0, 0);
	gm_input("text", "Text", t.text, TMAX);
	gm_enum("kind", "Repeat", t.kind == TK_ONCE ? 0 : t.kind == TK_DAILY
		? 1 : 2, KIND_OPTS);
	gm_date("day", "Date", f_y, f_m, f_d);
	gm_time("at", "Time", t.start / 60, t.start % 60);
	gm_time("to", "Until", end / 60, end % 60);
	gm_int("every", "Every", 5, 240, 5, t.every, "min");
	gm_days("days", "Days", days_out(t.days));
	for (i = 0; i < (int) sizeof snoozes - 1 && snoozes[i] < t.snooze; i++) ;
	gm_enum("snz", "Snooze", i, SNOOZE_OPTS);
	gm_bool("snd", "Sound", (t.flags & NF_SOUND) != 0);
	gm_bool("vib", "Vibro", (t.flags & NF_VIBRO) != 0);
	gm_bool("led", "LED", (t.flags & NF_LED) != 0);
	gm_action("save", "Save");
	if (!is_new) gm_action("del", "Delete");
	gm_show();
	cur = 'e';
}

static void edit(int n)
{
	struct tm *tm;
	time_t at;

	now = time(NULL);
	is_new = n == 0 || load(n) < 0;
	if (is_new) {
		char used[NMAX + 1];
		int i;

		memset(&t, 0, sizeof t);
		memset(used, 0, sizeof used);
		/* the lowest free id: one more pass over the directory */
		{
			struct _v7_direct e;
			char name[sizeof e.d_name + 1];
			int fd = open(APP_TIMERS, O_RDONLY);

			while (fd >= 0 && read(fd, (char *) &e, sizeof e)
					== sizeof e) {
				if (e.d_ino == 0) continue;
				memcpy(name, e.d_name, sizeof e.d_name);
				name[sizeof e.d_name] = 0;
				used[idnum(name)] = 1;
			}
			if (fd >= 0) close(fd);
		}
		for (i = 1; i <= NMAX && used[i]; i++) ;
		idof(t.id, i <= NMAX ? i : NMAX);
		t.kind = TK_INTERVAL;
		t.start = 9 * 60;
		t.end = 17 * 60;
		t.every = 30;
		t.days = 0x7F;
		t.flags = NF_ALL;
		t.snooze = 5;
		strcpy(t.text, "Drink a glass of water");
	}
	if (t.every < 5) t.every = 30;
	if (t.end == 0) t.end = 17 * 60;
	at = t.kind == TK_ONCE && t.at != 0 ? t.at : now;
	tm = localtime(&at);
	f_y = (short) (tm->tm_year + 1900);
	f_m = (short) (tm->tm_mon + 1);
	f_d = (short) tm->tm_mday;
	if (t.kind == TK_ONCE) t.start = (unsigned short) (tm->tm_hour * 60
							   + tm->tm_min);
	/* what the app owns, whatever the file said */
	t.cat = TC_REMINDER;
	t.action = TA_NOTIFY;
	strcpy(t.title, "Reminder");
	strcpy(t.log, APP);
	warn = NULL;
}

/* the form's t.at from f_y f_m f_d and t.start */
static time_t form_at(void)
{
	struct tm tm;

	memset(&tm, 0, sizeof tm);
	tm.tm_year = f_y - 1900;
	tm.tm_mon = f_m - 1;
	tm.tm_mday = f_d;
	tm.tm_hour = t.start / 60;
	tm.tm_min = t.start % 60;
	tm.tm_isdst = -1;
	return mktime(&tm);
}

static int hm(const char *v)
{
	int m = ff_hm(v);

	return m < 0 ? 0 : m;
}

/* submit: the rows that changed */
static void values(char **f)
{
	char *k, *v, *p;
	int i, x;

	for (i = 2; f[i] != NULL; i++) {
		k = gm_kv(f[i], &v);
		x = atoi(v);
		if (strcmp(k, "text") == 0) {
			strncpy(t.text, v, TMAX);
			t.text[TMAX] = 0;
			for (p = t.text; *p != 0; p++)
				if (*p == '\n' || *p == '\r') *p = ' ';
		} else if (strcmp(k, "kind") == 0)
			t.kind = x == 0 ? TK_ONCE : x == 1 ? TK_DAILY : TK_INTERVAL;
		else if (strcmp(k, "day") == 0 && strlen(v) == 10) {
			f_y = (short) x;
			f_m = (short) atoi(v + 5);
			f_d = (short) atoi(v + 8);
		} else if (strcmp(k, "at") == 0)
			t.start = (unsigned short) hm(v);
		else if (strcmp(k, "to") == 0)
			t.end = (unsigned short) hm(v);
		else if (strcmp(k, "every") == 0 && x >= 5 && x <= 240)
			t.every = (unsigned short) x;
		else if (strcmp(k, "days") == 0)
			t.days = (unsigned char) days_in(x & 0x7F);
		else if (strcmp(k, "snz") == 0 && x >= 0
			 && x < (int) sizeof snoozes)
			t.snooze = (unsigned char) snoozes[x];
		else if (strcmp(k, "snd") == 0 || strcmp(k, "vib") == 0
			 || strcmp(k, "led") == 0) {
			int bit = k[0] == 's' ? NF_SOUND : k[0] == 'v' ? NF_VIBRO
								: NF_LED;

			t.flags = (unsigned char) (x ? t.flags | bit
						     : t.flags & ~bit);
		}
	}
}

/* Save: 0, or -1 with warn set */
static int save(void)
{
	now = time(NULL);
	warn = NULL;
	if (t.text[0] == 0) warn = "Type a text";
	else if (t.kind == TK_ONCE) {
		t.at = form_at();
		if (t.at <= now) warn = "Pick a time to come";
	} else if (t.days == 0)
		warn = "Choose a day";
	else if (t.kind == TK_INTERVAL && t.start >= t.end)
		warn = "Time must be before Until";
	if (warn == NULL && is_new && each(NULL, NULL) >= RMAX)
		warn = "32 reminders: delete one";
	if (warn != NULL) return -1;
	if (t.kind == TK_DAILY) t.end = 0;
	if (t.kind != TK_ONCE) t.at = 0;
	if (timer_add(&t) < 0) {
		warn = "Cannot save the timer";
		return -1;
	}
	return 0;
}

/* ---- remind -s ---- */

static int summary(void)
{
	char b[16];
	int c[7], i;

	counts(c);
	put(1, "today ");
	put(1, nat_num(b, (unsigned) c[6]));
	put(1, "\nweek");
	for (i = 0; i < 7; i++) {
		put(1, " ");
		put(1, nat_num(b, (unsigned) c[i]));
	}
	put(1, "\n");
	return 0;
}

int main(int argc, char **argv)
{
	char buf[128], ev[192], *f[GM_FIELDS];
	int n;

	if (getuid() == 0 && nat_become(APP) < 0) {
		put(2, "remind: no user " APP " (pkg install remind)\n");
		return 1;
	}
	if (argc == 2 && strcmp(argv[1], "-s") == 0) return summary();
	if (argc != 1) {
		put(2, "usage: remind [-s]\n");
		return 2;
	}
	if (gm_init(buf, sizeof buf) < 0) {
		put(2, "remind: start it from the launcher (Kanpaia)\n");
		return 1;
	}
	w_main();
	for (;;) {
		switch (gm_event(ev, sizeof ev, f, GM_FIELDS)) {
		case GM_OK:
			if (cur == 'm') {
				if (strcmp(f[2], "new") == 0) {
					edit(0);
					par = 'm';
					w_form();
				} else
					w_list(f[2][0] == 'n' ? 'u' : f[2][0], 0);
			} else if (cur == 'e') {
				if (strcmp(f[2], "save") == 0 && save() < 0)
					w_form();	/* the warning as its title */
				else if (strcmp(f[2], "del") == 0
					 && timer_remove(t.id) < 0) {
					warn = "Cannot delete it";
					w_form();
				} else if (strcmp(f[2], "save") == 0
					   || strcmp(f[2], "del") == 0)
					w_list(par, par == 'm' ? 0 : lfirst);
				else
					(void) gm_busy(0);
			} else if ((n = idnum(f[2])) != 0 && load(n) == 0) {
				edit(n);	/* a reminder in a list */
				par = cur;
				w_form();
			} else
				(void) gm_busy(0);	/* gone: nothing to open */
			break;
		case GM_SUBMIT:
			if (cur == 'e' && strcmp(f[1], "edit") == 0) values(f);
			break;
		case GM_BACK:
			/* the desktop shows the parent (or asks: redraw) */
			if (strcmp(f[1], "edit") == 0) cur = par;
			else if (cur != 'e') cur = 'm';
			break;
		case GM_PAGE:
			if (cur == 't' || cur == 'u' || cur == 'h')
				w_list(cur, atoi(f[2]));
			break;
		case GM_REDRAW:
			if (cur == 'e') w_form();
			else w_list(cur, cur == 'm' ? 0 : lfirst);
			break;
		case GM_CLOSE:
			return 0;
		}
	}
}
