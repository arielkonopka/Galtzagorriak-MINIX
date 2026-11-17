/* SPDX-License-Identifier: GPL-3.0-or-later
 * alarm - the alarm clock (Galtzagorriak MINIX, spec 7b.2, F.4).
 *
 * Usage:	alarm		the app (libfuri GUI)
 *		alarm -n	the next alarm, as text
 *
 * Any number of alarms.  An alarm that is on is one `daily` (weekdays) or
 * `once` timer in /var/timers (libapp, category alarm, id alarm-N): it
 * always rings as it says itself, whatever the foreground app, the
 * volume setting or do-not-disturb (libapp notify_flags(),
 * notify_volume()).  It carries its label, ring settings (Mode:
 * escalating or steady, Step: s per stage, Volume:, the outputs sound,
 * vibro, LED blink and screen flash, Sound: single tone or melody), tone
 * (built-in or a user RTTTL or .fmf file in APP_DATA/alarm/), snooze and
 * ring time to alert.  An alarm that is off is kept as APP_DATA/alarm/alarm-N.off in
 * the same format, so timed never sees it.  Nothing of the app is in RAM
 * while it is closed.
 *
 * Desk clock: a large clock, the date and the next alarm.  It redraws
 * once a minute: the app blocks in its queue wait until the next minute
 * starts (no polling; the RTC wake-up timer of R7b.1 will replace the
 * wait once the local lane has it).  Left/Right: the backlight (auto,
 * dim, on; kept in APP_DATA/alarm/settings); leaving it restores the
 * backlight as it was.
 *
 * The launcher starts it as root (/etc/apps "Alarm:root:..."); once the
 * GUI has the screen, the buttons and the backlight it becomes the user
 * alarm of group timers (nat_become(), spec A.3 until the launcher does
 * it).
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <gui/canvas.h>
#include "src/gui/gui_i.h"	/* the title bar text */
#include "app.h"
#include "nat.h"

#define APP	"alarm"
#define PREFIX	"alarm-"
#define ADIR	APP_DATA "/" APP

typedef struct {
	char id[APP_ID_MAX + 1];
	char label[13];
	unsigned char kind, days, on;
	unsigned short min;	/* daily: Time; once: the local time of At */
	time_t at;
} Alarm;

static const char *const labels[] = {
	"Alarm", "Wake up", "Work", "School", "Medicine", "Nap is over",
};
#define NLABELS	(int) (sizeof labels / sizeof *labels)
static const unsigned char snoozes[] = { 0, 5, 10, 15, 20, 30 };
static const unsigned char rings[] = { 1, 5, 10, 15, 30, 60 };
static const unsigned char steps[] = { 10, 20, 30, 60, 90, 120 };
static const char *const lights[] = { "auto", "dim", "on" };

enum { S_LIST, S_EDIT, S_DESK };
enum { F_HOUR, F_MIN, F_REPEAT, F_DAYS, F_LABEL, F_MODE, F_STEP, F_VOLUME,
       F_SOUND, F_STYPE, F_TONE, F_VIBRO, F_LED, F_SCREEN, F_SNOOZE, F_RING,
       F_ON, F_SAVE, F_DEL };

static struct {
	int screen;
	Alarm* list;
	int n, sel, top;
	struct app_timer t;	/* the one in the form */
	int hm, on, is_new, dcur;
	struct nat_form f;
	const char* msg;
	int light;		/* desk: 0 auto, 1 dim, 2 on */
	int bl_fd;		/* /dev/backlight, opened as root */
	unsigned char bl_old[3];
	bool bl_saved, quit;
} r;

/* ---- data ---- */

static int ours(const struct app_timer* t)
{
	return strncmp(t->id, PREFIX, sizeof PREFIX - 1) == 0
		&& t->cat == TC_ALARM && (t->kind == TK_DAILY || t->kind == TK_ONCE);
}

static int local_min(time_t at)
{
	struct tm* tm = localtime(&at);

	return tm->tm_hour * 60 + tm->tm_min;
}

static void add(const struct app_timer* t, int on)
{
	Alarm* m;

	if (!ours(t) || (m = realloc(r.list, (r.n + 1) * sizeof *m)) == NULL)
		return;
	r.list = m;
	m += r.n++;
	strcpy(m->id, t->id);
	strncpy(m->label, t->title, sizeof m->label - 1);
	m->label[sizeof m->label - 1] = 0;
	m->kind = t->kind;
	m->days = t->days;
	m->at = t->at;
	m->min = t->kind == TK_ONCE ? local_min(t->at) : t->start;
	m->on = on;
}

static int add_cb(struct app_timer* t, void* arg)
{
	(void) arg;
	add(t, 1);
	return 0;
}

/* An alarm that is off: DIR/<id>.off. */
static int off_path(char* path, const char* id)
{
	if (!app_id_ok(id)) return -1;
	strcpy(path, ADIR "/");
	strcat(path, id);
	strcat(path, ".off");
	return 0;
}

static int off_read(const char* id, struct app_timer* t)
{
	char path[64], buf[APP_FILE_MAX];

	if (off_path(path, id) < 0 || ff_load(path, buf, sizeof buf) < 0)
		return -1;
	strcpy(t->id, id);
	return timer_parse(t, buf);
}

static void load(void)
{
	struct app_timer t;
	struct dirent* e;
	DIR* d;
	char id[APP_ID_MAX + 1], *dot;

	free(r.list);
	r.list = NULL;
	r.n = 0;
	(void) timer_list(add_cb, NULL);
	if ((d = opendir(ADIR)) != NULL) {
		while ((e = readdir(d)) != NULL) {
			if ((dot = strrchr(e->d_name, '.')) == NULL
			    || strcmp(dot, ".off") != 0
			    || dot - e->d_name > APP_ID_MAX) continue;
			memcpy(id, e->d_name, dot - e->d_name);
			id[dot - e->d_name] = 0;
			if (off_read(id, &t) == 0) add(&t, 0);
		}
		closedir(d);
	}
	if (r.sel > r.n + 1) r.sel = r.n + 1;
}

/* The next alarm that is on: its index, or -1; *when gets the time. */
static int next(time_t now, time_t* when)
{
	struct app_timer t;
	time_t n;
	int i, best = -1;

	memset(&t, 0, sizeof t);
	for (i = 0; i < r.n; i++) {
		if (!r.list[i].on) continue;
		t.kind = r.list[i].kind;
		t.days = r.list[i].days;
		t.start = r.list[i].min;
		t.at = r.list[i].at;
		if ((n = timer_next(&t, now)) != 0 && (best < 0 || n < *when)) {
			best = i;
			*when = n;
		}
	}
	return best;
}

/* The lowest id alarm-N not in the list (on or off; no file lookups). */
static void new_id(char* id)
{
	int i, j;

	for (i = 1; ; i++) {
		strcpy(id, PREFIX);
		nat_num(id + sizeof PREFIX - 1, i);
		for (j = 0; j < r.n && strcmp(r.list[j].id, id) != 0; j++) ;
		if (j == r.n) return;
	}
}

/* ---- tones: the built-in ones, then the user's files in DIR ---- */

static int tone_ok(const char* name)
{
	const char* dot = strrchr(name, '.');

	return name[0] != '.' && dot != NULL && (strcmp(dot, ".fmf") == 0
		|| strcmp(dot, ".txt") == 0 || strcmp(dot, ".rtttl") == 0)
		&& strlen(ADIR "/") + strlen(name) < sizeof r.t.tone;
}

/* The tone after (d > 0) or before the one set now. */
static void tone_step(int d)
{
	char names[2][40], first[40], *cur = r.t.tone, *p;
	struct dirent* e;
	DIR* dir;
	int i, n = 0, found = -1, nb;

	/* built-ins: the index of the current one */
	for (nb = 0; tune_names[nb] != NULL; nb++)
		if (strcmp(cur, tune_names[nb]) == 0) found = nb;
	if (cur[0] == 0) found = 0;
	if (found >= 0 && found + d >= 0 && found + d < nb) {
		strcpy(cur, tune_names[found + d]);
		return;
	}
	/* the files: cycle through them without keeping a list */
	names[0][0] = names[1][0] = first[0] = 0;
	p = strrchr(cur, '/');
	if ((dir = opendir(ADIR)) != NULL) {
		while ((e = readdir(dir)) != NULL) {
			if (!tone_ok(e->d_name)) continue;
			if (n++ == 0 || strcmp(e->d_name, first) < 0)
				strcpy(first, e->d_name);
			/* names[0]: the greatest below cur; [1]: the least above */
			if (p != NULL && strcmp(e->d_name, p + 1) < 0
			    && (names[0][0] == 0 || strcmp(e->d_name, names[0]) > 0))
				strcpy(names[0], e->d_name);
			if (p != NULL && strcmp(e->d_name, p + 1) > 0
			    && (names[1][0] == 0 || strcmp(e->d_name, names[1]) < 0))
				strcpy(names[1], e->d_name);
			if (p == NULL && d < 0
			    && (names[0][0] == 0 || strcmp(e->d_name, names[0]) > 0))
				strcpy(names[0], e->d_name);	/* the last */
		}
		closedir(dir);
	}
	i = d > 0 ? 1 : 0;
	if (p == NULL) {		/* from the built-ins into the files */
		if (n == 0) {
			strcpy(cur, tune_names[d > 0 ? 0 : nb - 1]);
			return;
		}
		strcpy(cur, ADIR "/");
		strcat(cur, d > 0 ? first : names[0]);
	} else if (names[i][0] != 0) {
		strcpy(cur, ADIR "/");
		strcat(cur, names[i]);
	} else strcpy(cur, tune_names[d > 0 ? 0 : nb - 1]);	/* wrap */
}

/* ---- the form ---- */

static void yesno(int f, const char* label, int v)
{
	r.f.label[f] = label;
	strcpy(r.f.value[f], v ? "yes" : "no");
}

static void form_values(void)
{
	struct nat_form* f = &r.f;
	struct app_timer* t = &r.t;
	const char* p;
	char b[8];

	f->n = r.is_new ? F_SAVE + 1 : F_DEL + 1;
	f->label[F_HOUR] = "Hour";
	nat_hm(b, r.hm);
	b[2] = 0;
	strcpy(f->value[F_HOUR], b);
	f->label[F_MIN] = "Minute";
	strcpy(f->value[F_MIN], b + 3);
	f->label[F_REPEAT] = "Repeat";
	strcpy(f->value[F_REPEAT], t->kind == TK_ONCE ? "once" : "on days");
	f->label[F_DAYS] = "Days";
	if (t->kind == TK_ONCE) strcpy(f->value[F_DAYS], "-");
	else nat_days(f->value[F_DAYS], t->days);
	f->label[F_LABEL] = "Label";
	strcpy(f->value[F_LABEL], t->title);
	f->label[F_TONE] = "Tone";
	p = t->tone[0] == 0 ? "beep" : strrchr(t->tone, '/') ? strrchr(t->tone,
							'/') + 1 : t->tone;
	strncpy(f->value[F_TONE], p, sizeof f->value[0] - 1);
	f->label[F_MODE] = "Mode";
	strcpy(f->value[F_MODE], t->ramp ? "escalating" : "steady");
	f->label[F_STEP] = "Stage";
	if (!t->ramp) strcpy(f->value[F_STEP], "-");
	else strcat(strcpy(f->value[F_STEP], nat_num(b, t->step ? t->step
								: 10)), " s");
	f->label[F_VOLUME] = "Volume";
	strcat(strcpy(f->value[F_VOLUME], nat_num(b, t->volume ? t->volume
								: 100)), " %");
	yesno(F_SOUND, "Sound", t->flags & NF_SOUND);
	f->label[F_STYPE] = "Sound type";
	strcpy(f->value[F_STYPE], t->single ? "single" : "melody");
	yesno(F_VIBRO, "Vibro", t->flags & NF_VIBRO);
	yesno(F_LED, "LED blink", t->flags & NF_LED);
	yesno(F_SCREEN, "Screen flash", t->flags & NF_SCREEN);
	f->label[F_SNOOZE] = "Snooze";
	strcpy(f->value[F_SNOOZE], t->snooze ? strcat(nat_num(b, t->snooze),
								" min") : "off");
	f->label[F_RING] = "Ring for";
	strcat(strcpy(f->value[F_RING], nat_num(b, t->ring)), " min");
	yesno(F_ON, "On", r.on);
	f->label[F_SAVE] = "Save";
	f->value[F_SAVE][0] = 0;
	f->label[F_DEL] = "Delete";
	f->value[F_DEL][0] = 0;
}

static void edit(int i)
{
	struct app_timer* t = &r.t;
	int ok = 0;

	memset(t, 0, sizeof *t);
	r.is_new = i < 0;
	if (i >= 0) {
		ok = r.list[i].on ? timer_read(r.list[i].id, t) == 0
				  : off_read(r.list[i].id, t) == 0;
		r.on = r.list[i].on;
	}
	if (!ok) {
		r.is_new = 1;
		memset(t, 0, sizeof *t);
		new_id(t->id);
		t->kind = TK_DAILY;
		t->start = 7 * 60;
		t->days = 0x3E;			/* Mo-Fr */
		t->flags = NF_OUT;
		t->snooze = 5;
		t->ring = 10;
		t->ramp = 1;
		strcpy(t->title, labels[0]);
		r.on = 1;
	}
	r.hm = t->kind == TK_ONCE ? local_min(t->at) : t->start;
	if (t->ring == 0) t->ring = 10;
	if (t->days == 0) t->days = 0x3E;
	t->cat = TC_ALARM;
	t->action = TA_NOTIFY;
	t->missed = 1;			/* an alarm that was missed rings */
	t->log[0] = 0;
	memset(&r.f, 0, sizeof r.f);
	r.dcur = 0;
	r.msg = NULL;
	form_values();
	r.screen = S_EDIT;
}

static int cycle(const unsigned char* v, int n, int cur, int d)
{
	int i;

	for (i = 0; i < n && v[i] != cur; i++) ;
	return v[(i == n ? 0 : i + d + n) % n];
}

static void change(int d)
{
	struct app_timer* t = &r.t;
	int i;

	switch (r.f.cur) {
	case F_HOUR: r.hm = (r.hm + d * 60 + 1440) % 1440; break;
	case F_MIN: r.hm = r.hm / 60 * 60 + (r.hm % 60 + d + 60) % 60; break;
	case F_REPEAT: t->kind = t->kind == TK_ONCE ? TK_DAILY : TK_ONCE; break;
	case F_DAYS: r.dcur = (r.dcur + d + 7) % 7; break;
	case F_LABEL:
		for (i = 0; i < NLABELS && strcmp(labels[i], t->title) != 0; i++) ;
		i = i == NLABELS ? 0 : (i + d + NLABELS) % NLABELS;
		strcpy(t->title, labels[i]);
		break;
	case F_TONE: tone_step(d); break;
	case F_MODE: t->ramp ^= 1; break;
	case F_STEP:
		if (t->ramp) t->step = cycle(steps, sizeof steps,
					     t->step ? t->step : 10, d);
		break;
	case F_VOLUME:			/* 10 % steps, 10-100 */
		i = (t->volume ? t->volume : 100) / 10 + d;
		t->volume = (i < 1 ? 10 : i > 10 ? 1 : i) * 10;
		break;
	case F_SOUND: t->flags ^= NF_SOUND; break;
	case F_STYPE: t->single ^= 1; break;
	case F_VIBRO: t->flags ^= NF_VIBRO; break;
	case F_LED: t->flags ^= NF_LED; break;
	case F_SCREEN: t->flags ^= NF_SCREEN; break;
	case F_SNOOZE: t->snooze = cycle(snoozes, sizeof snoozes, t->snooze, d); break;
	case F_RING: t->ring = cycle(rings, sizeof rings, t->ring, d); break;
	case F_ON: r.on ^= 1; break;
	}
	form_values();
}

/* Save the form: on -> /var/timers, off -> DIR/<id>.off; the other copy
 * goes. */
static int save(void)
{
	struct app_timer* t = &r.t;
	char path[64], buf[APP_FILE_MAX];
	time_t now = time(NULL);
	struct tm tm;

	t->start = r.hm;
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
	t->title[sizeof t->title - 1] = 0;
	strcpy(t->text, t->title);
	if (off_path(path, t->id) < 0) return -1;
	if (r.on) {
		if (timer_add(t) < 0) return -1;
		(void) unlink(path);
		return 0;
	}
	if (timer_format(t, buf, sizeof buf) < 0) return -1;
	if (mkdir(ADIR, 0700) < 0 && errno != EEXIST) return -1;
	strcpy(path, t->id);
	strcat(path, ".off");
	if (app_save(ADIR, path, buf, 0600) < 0) return -1;
	(void) timer_remove(t->id);
	return 0;
}

static void ok(void)
{
	struct app_timer* t = &r.t;
	char path[64];

	switch (r.f.cur) {
	case F_DAYS:
		if (t->kind == TK_ONCE) return;
		t->days ^= nat_daybit(r.dcur);
		form_values();
		return;
	case F_SAVE:
		if (t->kind == TK_DAILY && t->days == 0) r.msg = "Choose a day";
		else if (save() < 0) r.msg = "Cannot save the alarm";
		else break;
		return;
	case F_DEL:
		(void) timer_remove(t->id);
		if (off_path(path, t->id) == 0) (void) unlink(path);
		break;
	default:
		change(1);
		return;
	}
	load();
	r.screen = S_LIST;
}

/* ---- desk clock: backlight ---- */

static void light(int level)
{
	static const unsigned char lv[3] = { 0, 40, 255 };
	unsigned char b[4];
	unsigned s;

	if (r.bl_fd < 0) return;
	if (!r.bl_saved && lseek(r.bl_fd, 0L, SEEK_SET) == 0
	    && read(r.bl_fd, b, 4) == 4) {	/* hal.h HAL_BL_* */
		s = b[0] | b[1] << 8 | (unsigned) b[2] << 16 | (unsigned) b[3] << 24;
		r.bl_old[0] = s >> 16 & 0xFF;
		r.bl_old[1] = s & 0xFF;
		r.bl_old[2] = s >> 8 & 0x3F;
		if (r.bl_old[0] == 0) r.bl_old[0] = 255;
		r.bl_saved = true;
	}
	if (level < 0 || level == 0) {		/* back to how it was */
		if (r.bl_saved) (void) write(r.bl_fd, r.bl_old, 3);
		return;
	}
	b[0] = lv[level];
	b[1] = b[2] = 0;			/* never off by itself */
	(void) write(r.bl_fd, b, 3);
}

static void desk(int on)
{
	char v[2];

	if (on) {
		load();
		r.light = settings_get(APP, "Light", v, sizeof v) == 0
			&& v[0] >= '0' && v[0] <= '2' ? v[0] - '0' : 0;
		r.screen = S_DESK;
		light(r.light);
	} else {
		light(-1);
		r.screen = S_LIST;
	}
}

/* ---- drawing ---- */

static void row(Canvas* c, int y, bool sel, const char* left, const char* right)
{
	if (sel) {
		canvas_draw_box(c, 0, y, 128, 10);
		canvas_set_color(c, ColorWhite);
	}
	canvas_draw_str(c, 2, y + 8, left);
	if (right) canvas_draw_str_aligned(c, 126, y + 1, AlignRight, AlignTop, right);
	canvas_set_color(c, ColorBlack);
}

static void draw_list(Canvas* c)
{
	char l[24], b[8];
	int i, y;
	Alarm* a;

	canvas_set_font(c, FontSecondary);
	if (r.sel < r.top) r.top = r.sel;
	if (r.sel >= r.top + NAT_ROWS) r.top = r.sel - NAT_ROWS + 1;
	for (i = r.top, y = 0; i <= r.n + 1 && y < NAT_ROWS * 10; i++, y += 10) {
		if (i == 0) row(c, y, r.sel == 0, "Desk clock", NULL);
		else if (i == 1) row(c, y, r.sel == 1, "+ New alarm", NULL);
		else {
			a = &r.list[i - 2];
			strcpy(l, nat_hm(b, a->min));
			strcat(l, " ");
			strcat(l, a->label);
			row(c, y, r.sel == i, l, !a->on ? "off" : a->kind == TK_ONCE
				? "once" : nat_days(b, a->days));
		}
	}
}

static void draw_form(Canvas* c)
{
	struct nat_form* f = &r.f;
	int i, y;

	canvas_set_font(c, FontSecondary);
	for (i = f->top, y = 0; i < f->n && i < f->top + NAT_ROWS; i++, y += 10) {
		row(c, y, f->cur == i, f->label[i], f->value[i]);
		if (i == F_DAYS && f->cur == i && r.t.kind != TK_ONCE) {
			int w = canvas_string_width(c, f->value[i]);
			int x = 126 - w + r.dcur * w / 7;

			canvas_set_color(c, ColorWhite);
			canvas_draw_line(c, x, y + 9, x + w / 7 - 2, y + 9);
			canvas_set_color(c, ColorBlack);
		}
	}
	if (r.msg) {
		canvas_set_color(c, ColorWhite);
		canvas_draw_box(c, 0, 40, 128, 11);
		canvas_set_color(c, ColorBlack);
		canvas_draw_frame(c, 0, 40, 128, 11);
		canvas_draw_str_aligned(c, 64, 42, AlignCenter, AlignTop, r.msg);
	}
}

static void draw_desk(Canvas* c)
{
	static const char wd[] = "SunMonTueWedThuFriSat";
	char b[24], hm[6];
	time_t now = time(NULL), when = 0;
	struct tm tm = *localtime(&now);
	int i;

	canvas_set_font(c, FontSecondary);
	memcpy(b, wd + 3 * tm.tm_wday, 3);
	b[3] = ' ';
	nat_num(b + 4, tm.tm_mday);
	strcat(b, ".");
	nat_num(b + strlen(b), tm.tm_mon + 1);
	strcat(b, ".");
	canvas_draw_str(c, 2, 9, b);
	canvas_draw_str_aligned(c, 126, 1, AlignRight, AlignTop, lights[r.light]);
	canvas_set_font(c, FontBigNumbers);
	canvas_draw_str_aligned(c, 64, 12, AlignCenter, AlignTop,
		nat_hm(hm, tm.tm_hour * 60 + tm.tm_min));
	canvas_set_font(c, FontSecondary);
	if ((i = next(now, &when)) < 0) strcpy(b, "No alarm");
	else {
		tm = *localtime(&when);
		strcpy(b, when - now < 86400 ? "" : wd + 3 * tm.tm_wday);
		b[3] = 0;
		if (b[0]) strcat(b, " ");
		strcat(b, nat_hm(hm, tm.tm_hour * 60 + tm.tm_min));
		strcat(b, " ");
		strncat(b, r.list[i].label, sizeof b - strlen(b) - 1);
	}
	canvas_draw_str_aligned(c, 64, 41, AlignCenter, AlignTop, b);
}

static void draw(Canvas* c, void* ctx)
{
	(void) ctx;
	canvas_clear(c);
	canvas_set_color(c, ColorBlack);
	if (r.screen == S_LIST) draw_list(c);
	else if (r.screen == S_EDIT) draw_form(c);
	else draw_desk(c);
}

/* ---- input ---- */

static void input(InputEvent* e, void* ctx)
{
	FuriMessageQueue* q = ctx;

	/* on the press, as flipper/gui (T3: L1 counts from the press) */
	if (e->type == InputTypePress || (e->type == InputTypeRepeat
	    && e->key != InputKeyOk && e->key != InputKeyBack))
		furi_message_queue_put(q, e, 0);
}

static void key(const InputEvent* e)
{
	char v[2];

	r.msg = NULL;
	switch (r.screen) {
	case S_LIST:
		if (e->key == InputKeyBack) r.quit = true;
		else if (e->key == InputKeyUp) r.sel = (r.sel + r.n + 1) % (r.n + 2);
		else if (e->key == InputKeyDown) r.sel = (r.sel + 1) % (r.n + 2);
		else if (e->key == InputKeyOk) {
			if (r.sel == 0) desk(1);
			else edit(r.sel - 2);
		}
		break;
	case S_EDIT:
		if (e->key == InputKeyBack) r.screen = S_LIST;
		else if (e->key == InputKeyUp) nat_form_move(&r.f, -1);
		else if (e->key == InputKeyDown) nat_form_move(&r.f, 1);
		else if (e->key == InputKeyLeft) change(-1);
		else if (e->key == InputKeyRight) change(1);
		else if (e->key == InputKeyOk) ok();
		break;
	case S_DESK:
		if (e->key == InputKeyBack) desk(0);
		else if (e->key == InputKeyLeft || e->key == InputKeyRight) {
			r.light = (r.light + (e->key == InputKeyRight ? 1 : 2)) % 3;
			light(r.light);
			v[0] = '0' + r.light;
			v[1] = 0;
			(void) settings_set(APP, "Light", v);
		}
		break;
	}
}

/* ms to the start of the next minute (and a little: never early). */
static uint32_t to_minute(void)
{
	return (uint32_t) (60 - time(NULL) % 60) * 1000U + 20U;
}

static int print_next(void)
{
	time_t now = time(NULL), when = 0;
	char b[6], w[12];
	int i;

	load();
	if ((i = next(now, &when)) < 0) {
		printf("no alarm\n");
		return 1;
	}
	printf("%s %s %s %s\n", r.list[i].id, nat_num(w, (unsigned) when),
		nat_hm(b, local_min(when)), r.list[i].label);
	return 0;
}

int main(int argc, char** argv)
{
	FuriMessageQueue* q;
	ViewPort* vp;
	Gui* gui;
	InputEvent e;
	FuriStatus st;

	if (argc == 2 && strcmp(argv[1], "-n") == 0) {
		if (nat_become(APP) < 0) return 1;
		return print_next();
	}
	if (argc != 1) {
		fprintf(stderr, "usage: alarm [-n]\n");
		return 2;
	}
	q = furi_message_queue_alloc(4, sizeof(InputEvent));
	vp = view_port_alloc();
	view_port_draw_callback_set(vp, draw, NULL);
	view_port_input_callback_set(vp, input, q);
	r.bl_fd = open("/dev/backlight", O_RDWR);	/* group gui: now */
	(void) nat_foreground();		/* as root, before the GUI */
	gui = furi_record_open(RECORD_GUI);	/* the screen, the buttons */
	if (nat_become(APP) < 0) {
		fprintf(stderr, "alarm: no user " APP " (pkg install alarm)\n");
		furi_record_close(RECORD_GUI);
		nat_background();
		return 1;
	}
	gui->title = APP;
	load();
	gui_add_view_port(gui, vp, GuiLayerWindow);
	while (!r.quit) {
		/* desk clock: sleep to the next minute, then redraw */
		st = furi_message_queue_get(q, &e, r.screen == S_DESK
					? to_minute() : FuriWaitForever);
		if (st == FuriStatusOk) key(&e);
		else if (st != FuriStatusErrorTimeout) continue;
		view_port_update(vp);
	}
	if (r.screen == S_DESK) light(-1);
	gui_remove_view_port(gui, vp);
	view_port_free(vp);
	furi_record_close(RECORD_GUI);
	nat_background();
	furi_message_queue_free(q);
	free(r.list);
	return 0;
}
