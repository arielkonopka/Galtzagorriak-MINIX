/* SPDX-License-Identifier: GPL-3.0-or-later
 * alarm - Ordua, the alarm clock (Galtzagorriak MINIX, spec 7b.2, F.4,
 * K.8): a window app.
 *
 * Usage:	alarm		the app (started by the launcher: "=" in
 *				/etc/apps, its pipes on fd 3 and 4)
 *		alarm -n	the next alarm, as text
 *
 * The desktop draws; the app only describes its windows (libgm) and
 * sleeps in read() until the person does something that needs it.  No
 * libfuri, no Canvas, no gui thread, no stdio, no malloc: an open Ordua
 * takes about 4 KB, a closed one nothing.
 *
 *	list	Desk clock, + New alarm, then the alarms (time, label, the
 *		days, "once" or "off"); OK opens one
 *	edit	time, repeat (once / on days), days, label, on, Ring
 *		settings, Save, Delete
 *	ring	mode (escalating / steady), stage, volume, beep, sound
 *		(melody / single), melody, vibro, LED blink, screen flash,
 *		snooze, ring time
 *	clock	the desktop's big clock with the next alarm in its two lines;
 *		the desktop ticks the minute, the app makes 0 wakeups
 *
 * Any alarm that is on is one `daily` (weekdays) or `once` timer in
 * /var/timers (libapp, category alarm, id alarm-N): timed and alert ring
 * it as they always did, whatever the foreground app (K.8: the alarm
 * never depends on the desktop).  An alarm that is off is kept as
 * APP_DATA/alarm/alarm-N.off in the same format, so timed never sees it.
 * The ids are alarm-1 .. alarm-AMAX, looked up by name (opendir() would
 * malloc a 1 KB DIR); a new alarm takes the lowest free one.
 *
 * The launcher starts it as the user alarm (group timers, /etc/apps
 * "Ordua:alarm:@alarm:=/usr/bin/alarm").
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "app.h"
#include "gm.h"
#include "nat.h"

#define APP	"alarm"
#define PREFIX	"alarm-"
#define ADIR	APP_DATA "/" APP
#define AMAX	14		/* alarms: one list window of 16 rows */
#define LABEL	12		/* label characters */

static const unsigned char snoozes[] = { 0, 5, 10, 15, 20, 30 };
static const unsigned char rings[] = { 1, 5, 10, 15, 30, 60 };
#define NSNOOZE	(int) sizeof snoozes
#define NRING	(int) sizeof rings
/* own melodies: any of these in APP_DATA/alarm (looked up with stat()) */
static const char *const own_tones[] = { "tone.fmf", "tone.rtttl", "tone.txt" };
#define NOWN	(int) (sizeof own_tones / sizeof *own_tones)

enum { W_LIST, W_EDIT, W_RING, W_MSG, W_CLOCK };

static struct {
	struct app_timer t;	/* the alarm in the form; scratch in get() */
	int hm, on, is_new;
	char cur;		/* W_*: the window the desktop shows */
	char from;		/* W_*: the parent of the message window */
	const char *msg;
	char extra[2][24];	/* the melody options after the built-ins */
} r;

/* ---- small helpers ---- */

static void out(const char *s)
{
	(void) write(1, s, strlen(s));
}

/* The days of a timer (bit 0 Sunday) as a days row (bit 0 Monday). */
static int to_mon(int d)
{
	return (d >> 1 | (d & 1) << 6) & 0x7F;
}

static int from_mon(int m)
{
	return (m << 1 & 0x7E) | (m >> 6 & 1);
}

static int local_min(time_t at)
{
	struct tm *tm = localtime(&at);

	return tm->tm_hour * 60 + tm->tm_min;
}

static char *alarm_id(char *id, int n)
{
	strcpy(id, PREFIX);
	nat_num(id + sizeof PREFIX - 1, (unsigned) n);
	return id;
}

/* ---- data ---- */

static int ours(const struct app_timer *t)
{
	return t->cat == TC_ALARM && (t->kind == TK_DAILY || t->kind == TK_ONCE);
}

/* An alarm that is off: DIR/<id>.off. */
static int off_path(char *path, const char *id)
{
	if (!app_id_ok(id)) return -1;
	strcpy(path, ADIR "/");
	strcat(path, id);
	strcat(path, ".off");
	return 0;
}

static int off_read(const char *id, struct app_timer *t)
{
	char path[64], buf[APP_FILE_MAX];

	if (off_path(path, id) < 0 || ff_load(path, buf, sizeof buf) < 0)
		return -1;
	strcpy(t->id, id);
	return timer_parse(t, buf);
}

/* Alarm alarm-N into r.t, the scratch timer when no form is open (the
 * list, the clock and -n read the files as they go: no table in RAM).
 * 1 on, 0 off, -1 none (or not an alarm of ours). */
static int get(int n)
{
	struct app_timer *t = &r.t;
	char id[APP_ID_MAX + 1];
	int on = 1;

	alarm_id(id, n);
	memset(t, 0, sizeof *t);
	if (timer_read(id, t) < 0) {
		memset(t, 0, sizeof *t);
		if (off_read(id, t) < 0) return -1;
		on = 0;
	}
	if (!ours(t)) return -1;
	t->title[LABEL] = 0;
	return on;
}

/* The minute of the day an alarm rings at (once: of its At). */
static int alarm_min(const struct app_timer *t)
{
	return t->kind == TK_ONCE ? local_min(t->at) : t->start;
}

/* The next alarm that is on: its N, or 0; *when gets the time and
 * label[LABEL + 1] its label. */
static int next(time_t now, time_t *when, char *label)
{
	time_t w;
	int i, best = 0;

	for (i = 1; i <= AMAX; i++) {
		if (get(i) != 1) continue;
		if ((w = timer_next(&r.t, now)) != 0 && (best == 0 || w < *when)) {
			best = i;
			*when = w;
			strcpy(label, r.t.title);
		}
	}
	return best;
}

/* The lowest alarm-N whose files are not there, or 0 (all AMAX are). */
static int new_n(void)
{
	char id[APP_ID_MAX + 1], path[64];
	struct stat st;
	int i;

	for (i = 1; i <= AMAX; i++) {
		alarm_id(id, i);
		if (off_path(path, id) == 0 && stat(path, &st) == 0) continue;
		strcpy(path, APP_TIMERS "/");
		strcat(path, id);
		if (stat(path, &st) == 0) continue;
		return i;
	}
	return 0;
}

/* ---- the form ---- */

/* The form for alarm-N (0: a new one in the lowest free N). */
static void edit(int n)
{
	struct app_timer *t = &r.t;
	int on = n > 0 ? get(n) : -1;

	r.is_new = on < 0;
	r.on = on;
	if (on < 0) {
		r.is_new = 1;
		memset(t, 0, sizeof *t);
		alarm_id(t->id, new_n());
		t->kind = TK_DAILY;
		t->start = 7 * 60;
		t->days = 0x3E;			/* Mo-Fr */
		t->flags = NF_OUT;
		t->snooze = 5;
		t->ring = 10;
		t->ramp = 1;
		strcpy(t->title, "Alarm");
		r.on = 1;
	}
	r.hm = t->kind == TK_ONCE ? local_min(t->at) : t->start;
	if (t->ring == 0) t->ring = 10;
	if (t->days == 0) t->days = 0x3E;
	t->title[LABEL] = 0;
	t->cat = TC_ALARM;
	t->action = TA_NOTIFY;
	t->missed = 1;			/* an alarm that was missed rings */
	t->log[0] = 0;
}

/* An alarm that is off: DIR/<id>.off (its 512 B buffer on the stack only
 * here, not under timer_add()'s).  0 or -1. */
static int __attribute__((noinline)) save_off(const struct app_timer *t)
{
	char name[APP_ID_MAX + 8], buf[APP_FILE_MAX];

	if (timer_format(t, buf, sizeof buf) < 0
	    || (mkdir(ADIR, 0700) < 0 && errno != EEXIST))
		return -1;
	strcpy(name, t->id);
	strcat(name, ".off");
	return app_save(ADIR, name, buf, 0600);
}

/* Save the form: on -> /var/timers, off -> DIR/<id>.off; the other copy
 * goes.  NULL, or what is wrong. */
static const char *save(void)
{
	struct app_timer *t = &r.t;
	char path[64];
	time_t now = time((time_t *) 0);
	struct tm tm;

	if (t->kind == TK_DAILY && t->days == 0) return "Choose a day.";
	t->start = (unsigned short) r.hm;
	if (t->kind == TK_ONCE) {	/* the next HH:MM from now */
		tm = *localtime(&now);
		tm.tm_hour = r.hm / 60;
		tm.tm_min = r.hm % 60;
		tm.tm_sec = 0;
		tm.tm_isdst = -1;
		if ((t->at = mktime(&tm)) <= now) {
			tm.tm_mday++;
			tm.tm_isdst = -1;
			t->at = mktime(&tm);
		}
	}
	if (t->title[0] == 0) strcpy(t->title, "Alarm");
	strcpy(t->text, t->title);
	if (off_path(path, t->id) < 0) return "Cannot save the alarm.";
	if (r.on) {
		if (timer_add(t) < 0) return "Cannot save the alarm.";
		(void) unlink(path);
		return NULL;
	}
	if (save_off(t) < 0) return "Cannot save the alarm.";
	(void) timer_remove(t->id);
	return NULL;
}

static void delete(void)
{
	char path[64];

	(void) timer_remove(r.t.id);
	if (off_path(path, r.t.id) == 0) (void) unlink(path);
}

/* ---- melodies: the built-ins, then the current file and own ones ---- */

static int nbuiltin(void)
{
	int n;

	for (n = 0; tune_names[n] != NULL; n++) ;
	return n;
}

/* r.extra: the file of the alarm (if it has one), then the first own
 * tone file that is there and not the same. */
static void extras(void)
{
	const char *cur = r.t.tone, *p = strrchr(cur, '/');
	char path[64];
	struct stat st;
	int i, n = 0;

	r.extra[0][0] = r.extra[1][0] = 0;
	if (p != NULL && strlen(p + 1) < sizeof r.extra[0])
		strcpy(r.extra[n++], p + 1);
	for (i = 0; i < NOWN && n < 2; i++) {
		if (strcmp(own_tones[i], r.extra[0]) == 0) continue;
		strcpy(path, ADIR "/");
		strcat(path, own_tones[i]);
		if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
			strcpy(r.extra[n++], own_tones[i]);
	}
}

static int tone_index(void)
{
	const char *cur = r.t.tone, *p = strrchr(cur, '/');
	int i, nb = nbuiltin();

	if (cur[0] == 0) return 0;		/* the default: beep */
	if (p != NULL) {
		for (i = 0; i < 2; i++)
			if (r.extra[i][0] && strcmp(p + 1, r.extra[i]) == 0)
				return nb + i;
		return 0;
	}
	for (i = 0; i < nb; i++)
		if (strcmp(cur, tune_names[i]) == 0) return i;
	return 0;
}

static void tone_set(int i)
{
	int nb = nbuiltin();

	if (i >= 0 && i < nb) strcpy(r.t.tone, tune_names[i]);
	else if (i >= nb && i < nb + 2 && r.extra[i - nb][0]
		 && strlen(ADIR "/") + strlen(r.extra[i - nb]) < sizeof r.t.tone) {
		strcpy(r.t.tone, ADIR "/");
		strcat(r.t.tone, r.extra[i - nb]);
	}
}

static int idx(const unsigned char *v, int n, int cur)
{
	int i;

	for (i = 0; i < n && v[i] != cur; i++) ;
	return i == n ? 0 : i;
}

/* ---- windows ---- */

static void w_list(void)
{
	char key[6], l[LABEL + 8], b[8];
	struct app_timer *t = &r.t;
	int i, on;

	gm_win("list", "list", "Ordua", 0, 0);
	gm_item("clock", "Desk clock", "");
	if (new_n() > 0) gm_item("new", "+ New alarm", "");
	for (i = 1; i <= AMAX; i++) {
		if ((on = get(i)) < 0) continue;
		key[0] = 'a';
		nat_num(key + 1, (unsigned) i);
		strcpy(l, nat_hm(b, alarm_min(t)));
		strcat(l, " ");
		strcat(l, t->title);
		gm_item(key, l, !on ? "off" : t->kind == TK_ONCE ? "once"
							 : nat_days(b, t->days));
	}
	gm_show();
	r.cur = W_LIST;
}

static void w_edit(void)
{
	struct app_timer *t = &r.t;

	gm_win("edit", "list", r.is_new ? "New alarm" : "Alarm", 0, 0);
	gm_time("at", "Time", r.hm / 60, r.hm % 60);
	gm_enum("rep", "Repeat", t->kind == TK_ONCE ? 0 : 1, "once|on days");
	gm_days("days", "Days", to_mon(t->days));
	gm_input("label", "Label", t->title, LABEL);
	gm_bool("on", "On", r.on);
	gm_item("ring", "Ring settings", "");
	gm_action("save", "Save");
	if (!r.is_new) gm_action("del", "Delete");
	gm_show();
	r.cur = W_EDIT;
}

static void w_ring(void)
{
	struct app_timer *t = &r.t;
	char opts[112];
	int i;

	extras();
	opts[0] = 0;
	for (i = 0; tune_names[i] != NULL; i++) {
		if (i > 0) strcat(opts, "|");
		strcat(opts, tune_names[i]);
	}
	for (i = 0; i < 2; i++)
		if (r.extra[i][0] && strlen(opts) + strlen(r.extra[i]) + 2
				     < sizeof opts) {
			strcat(opts, "|");
			strcat(opts, r.extra[i]);
		} else r.extra[i][0] = 0;
	gm_win("ring", "list", "Ring", 0, 0);
	gm_enum("mode", "Mode", t->ramp ? 0 : 1, "escalating|steady");
	gm_int("step", "Stage", 10, 120, 10, t->step ? t->step : 10, "s");
	gm_int("vol", "Volume", 10, 100, 10, t->volume ? t->volume : 100, "%");
	gm_bool("beep", "Beep", t->flags & NF_SOUND);
	gm_enum("stype", "Sound", t->single ? 1 : 0, "melody|single");
	gm_enum("tone", "Melody", tone_index(), opts);
	gm_bool("vibro", "Vibro", t->flags & NF_VIBRO);
	gm_bool("led", "LED blink", t->flags & NF_LED);
	gm_bool("flash", "Screen flash", t->flags & NF_SCREEN);
	gm_enum("snooze", "Snooze", idx(snoozes, NSNOOZE, t->snooze),
		"off|5 min|10 min|15 min|20 min|30 min");
	gm_enum("ring", "Ring for", idx(rings, NRING, t->ring),
		"1 min|5 min|10 min|15 min|30 min|60 min");
	gm_show();
	r.cur = W_RING;
}

static void w_msg(void)
{
	if (r.cur != W_MSG) r.from = r.cur;
	gm_win("msg", "text", "Alarm", 0, 0);
	gm_line(r.msg);
	gm_show();
	r.cur = W_MSG;
}

/* The desk clock: the desktop draws the time and ticks the minute; the
 * app gives the next alarm, "Next Tue 07:00" and its label. */
static void w_clock(void)
{
	static const char wd[] = "SunMonTueWedThuFriSat";
	char a[24], b[6], label[LABEL + 1];
	time_t now = time((time_t *) 0), when = 0;
	struct tm *tm;

	gm_win("clock", "clock", "Desk clock", 0, 0);
	if (next(now, &when, label) == 0) {
		gm_line("No alarm on");
		gm_line("");
	} else {
		tm = localtime(&when);
		strcpy(a, "Next ");
		if (when - now >= 86400L) {
			memcpy(a + 5, wd + 3 * tm->tm_wday, 3);
			strcpy(a + 8, " ");
		}
		strcat(a, nat_hm(b, tm->tm_hour * 60 + tm->tm_min));
		gm_line(a);
		gm_line(label);
	}
	gm_show();
	r.cur = W_CLOCK;
}

static void redraw(void)
{
	switch (r.cur) {
	case W_EDIT: w_edit(); break;
	case W_RING: w_ring(); break;
	case W_MSG: w_msg(); break;
	case W_CLOCK: w_clock(); break;
	default: w_list();
	}
}

/* ---- events ---- */

static int num(const char *s)
{
	return atoi(s);
}

/* submit: the rows that changed */
static void values(const char *win, char **f)
{
	struct app_timer *t = &r.t;
	char *k, *v;
	int i, x;

	for (i = 2; i < GM_FIELDS && f[i] != NULL; i++) {
		k = gm_kv(f[i], &v);
		if (v == NULL) continue;
		x = num(v);
		if (strcmp(win, "edit") == 0) {
			if (strcmp(k, "at") == 0 && strlen(v) == 5)
				r.hm = (num(v) * 60 + num(v + 3)) % 1440;
			else if (strcmp(k, "rep") == 0)
				t->kind = x == 0 ? TK_ONCE : TK_DAILY;
			else if (strcmp(k, "days") == 0)
				t->days = (unsigned char) from_mon(x);
			else if (strcmp(k, "on") == 0) r.on = x != 0;
			else if (strcmp(k, "label") == 0) {
				int j;

				strncpy(t->title, v, LABEL);
				t->title[LABEL] = 0;
				for (j = 0; t->title[j] != 0; j++)
					if ((unsigned char) t->title[j] < ' ')
						t->title[j] = ' ';
			}
		} else if (strcmp(win, "ring") == 0) {
			if (strcmp(k, "mode") == 0) t->ramp = x == 0;
			else if (strcmp(k, "step") == 0 && x >= 10 && x <= 120)
				t->step = (unsigned char) x;
			else if (strcmp(k, "vol") == 0 && x >= 10 && x <= 100)
				t->volume = (unsigned char) x;
			else if (strcmp(k, "beep") == 0)
				t->flags = (unsigned char) (x ? t->flags | NF_SOUND
							      : t->flags & ~NF_SOUND);
			else if (strcmp(k, "vibro") == 0)
				t->flags = (unsigned char) (x ? t->flags | NF_VIBRO
							      : t->flags & ~NF_VIBRO);
			else if (strcmp(k, "led") == 0)
				t->flags = (unsigned char) (x ? t->flags | NF_LED
							      : t->flags & ~NF_LED);
			else if (strcmp(k, "flash") == 0)
				t->flags = (unsigned char) (x ? t->flags | NF_SCREEN
							      : t->flags & ~NF_SCREEN);
			else if (strcmp(k, "stype") == 0) t->single = x != 0;
			else if (strcmp(k, "tone") == 0) tone_set(x);
			else if (strcmp(k, "snooze") == 0 && x >= 0 && x < NSNOOZE)
				t->snooze = snoozes[x];
			else if (strcmp(k, "ring") == 0 && x >= 0 && x < NRING)
				t->ring = rings[x];
		}
	}
}

static void ok(const char *win, const char *key)
{
	int i;

	if (strcmp(win, "list") == 0) {
		if (strcmp(key, "clock") == 0) {
			w_clock();
			return;
		}
		if (strcmp(key, "new") == 0) {
			if (new_n() == 0) {
				r.msg = "No room: delete an alarm.";
				w_msg();
				return;
			}
			i = 0;
		} else
			i = key[0] == 'a' ? num(key + 1) : 0;
		edit(i > 0 && i <= AMAX ? i : 0);
		w_edit();
	} else if (strcmp(win, "edit") == 0) {
		if (strcmp(key, "ring") == 0) w_ring();
		else if (strcmp(key, "save") == 0) {
			if ((r.msg = save()) != NULL) {
				w_msg();
				return;
			}
			w_list();	/* the first window: the stack goes */
		} else if (strcmp(key, "del") == 0) {
			delete();
			w_list();
		} else
			(void) gm_busy(0);
	} else
		(void) gm_busy(0);	/* nothing to open: "idle" */
}

static int print_next(void)
{
	time_t now = time((time_t *) 0), when = 0;
	char b[12], id[APP_ID_MAX + 1], label[LABEL + 1];
	int i;

	if ((i = next(now, &when, label)) == 0) {
		out("no alarm\n");
		return 1;
	}
	out(alarm_id(id, i));
	out(" ");
	out(nat_num(b, (unsigned) when));
	out(" ");
	out(nat_hm(b, local_min(when)));
	out(" ");
	out(label);
	out("\n");
	return 0;
}

int main(int argc, char **argv)
{
	char buf[128], ev[160], *f[GM_FIELDS];

	if (argc == 2 && strcmp(argv[1], "-n") == 0) {
		if (nat_become(APP) < 0) return 1;
		return print_next();
	}
	if (argc != 1) {
		(void) write(2, "usage: alarm [-n]\n", 18);
		return 2;
	}
	if (gm_init(buf, sizeof buf) < 0) {
		out("alarm: start Ordua from the launcher (alarm -n: the next alarm)\n");
		return 1;
	}
	if (nat_become(APP) < 0) return 1;	/* never on as root */
	w_list();
	for (;;) {
		switch (gm_event(ev, sizeof ev, f, GM_FIELDS)) {
		case GM_OK:
			if (f[1] != NULL && f[2] != NULL) ok(f[1], f[2]);
			break;
		case GM_SUBMIT:
			if (f[1] != NULL) values(f[1], f);
			break;
		case GM_BACK:		/* the desktop shows the parent */
			if (f[1] == NULL) break;
			if (strcmp(f[1], "msg") == 0) r.cur = r.from;
			else if (strcmp(f[1], "ring") == 0) r.cur = W_EDIT;
			else r.cur = W_LIST;
			break;
		case GM_PAGE:
		case GM_REDRAW:
			redraw();
			break;
		case GM_CLOSE:
			return 0;
		}
	}
}
