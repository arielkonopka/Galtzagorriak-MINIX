/* setgroup - one group of the system settings as a window app (spec K.7,
 * K.8).
 *
 * Usage:	setgroup GROUP		(display, sound, time, about)
 *
 * Started by the launcher's SETTINGS page with its two pipes (fd 3
 * events, fd 4 records).  The group's screen is a `list` window of
 * int/enum/date/time rows (About: a `text` window); the desktop draws it
 * and changes the values itself.  The window is LIVE: the desktop sends
 * every change at once (submit), and the group applies it through its
 * driver (/dev/backlight, /dev/speaker, the RTC).  /etc/flipper/GROUP.conf
 * is written once, when the screen is left (close), and only when a
 * saved value really changed.
 *
 * RAM (ACK7.4, <= 3 KB with the shared libc's data): no malloc, no stdio,
 * no signal handler (gm_init ignores SIGPIPE), so its chmem needs no heap
 * and no MM gap (K.8); the record and event lines are 128 B on the stack.
 */
#include <sys/types.h>
#include <string.h>
#include <unistd.h>
#include "sgroup.h"
#include "gm.h"

static int grp;
static void say(const char *s)
{
	(void) write(2, s, strlen(s));
}

static unsigned char changed;		/* a saved value changed */

static int num(const char *p, int *v)	/* -1: not a number */
{
	int neg = *p == '-', n = 0;

	if (neg) p++;
	if (*p == 0) return -1;
	for (; *p != 0; p++) {
		if (*p < '0' || *p > '9' || n > 30000) return -1;
		n = n * 10 + (*p - '0');
	}
	*v = neg ? -n : n;
	return 0;
}

/* "YYYY-MM-DD" and "HH:MM" as the desktop sends them */
static int date_time(int type, const char *p, int *v)
{
	int a = 0, b = 0, c = 0, i;

	if (type == S_TIME) {
		if (strlen(p) != 5 || p[2] != ':') return -1;
		for (i = 0; i < 5; i++)
			if (i != 2 && (p[i] < '0' || p[i] > '9')) return -1;
		a = (p[0] - '0') * 10 + p[1] - '0';
		b = (p[3] - '0') * 10 + p[4] - '0';
		if (a > 23 || b > 59) return -1;
		*v = a * 60 + b;
		return 0;
	}
	if (strlen(p) != 10 || p[4] != '-' || p[7] != '-') return -1;
	for (i = 0; i < 10; i++)
		if (i != 4 && i != 7 && (p[i] < '0' || p[i] > '9')) return -1;
	for (i = 0; i < 4; i++) a = a * 10 + p[i] - '0';
	b = (p[5] - '0') * 10 + p[6] - '0';
	c = (p[8] - '0') * 10 + p[9] - '0';
	if (a < 2000 || a > 2127 || b < 1 || b > 12 || c < 1 || c > 31)
		return -1;
	*v = SG_DATE(a, b, c);
	return 0;
}

/* The group's main window (its id is the group's name). */
void sg_show(int g)
{
	const struct srow *rows, *r;
	char key[2], t[SG_TEXTMAX + 12];
	int n, i, k, text = 1, v;

	rows = sg_rows(g, &n);
	for (i = 0; i < n; i++) if (rows[i].type != S_TEXT) text = 0;
	gm_win_live(sg_name(g), text ? "text" : "list", sg_title(g), !text);
	key[1] = 0;
	for (i = 0; i < n; i++) {
		r = &rows[i];
		key[0] = (char) ('a' + i);
		v = sg_val[i];
		switch (r->type) {
		case S_INT:
			gm_int(key, r->name, r->min, r->max, r->step, v, r->unit);
			break;
		case S_ENUM:
			gm_enum(key, r->name, v - r->min, r->opts);
			break;
		case S_ACT:
			gm_action(key, r->name);
			break;
		case S_DATE:
			gm_date(key, r->name, SG_Y(v), SG_M(v), SG_D(v));
			break;
		case S_TIME:
			gm_time(key, r->name, v / 60, v % 60);
			break;
		case S_ITEM:
			t[0] = 0;
			(void) sg_call(g, SG_TEXT, i, t);
			gm_item(key, r->name, t);
			break;
		default:		/* S_TEXT: "Name     value" */
			k = (int) strlen(r->name);
			memcpy(t, r->name, (size_t) k);
			while (k < 9) t[k++] = ' ';
			t[k] = 0;
			(void) sg_call(g, SG_TEXT, i, t + k);
			gm_line(t);
		}
	}
	(void) gm_show();
}

/* A row's new value from a submit. */
static void value(char *pair)
{
	const struct srow *rows, *r;
	char *k, *val;
	int n, i, v;

	k = gm_kv(pair, &val);
	rows = sg_rows(grp, &n);
	if (k[0] < 'a' || k[0] >= 'a' + n || k[1] != 0) return;
	i = k[0] - 'a';
	r = &rows[i];
	switch (r->type) {
	case S_INT:
		if (num(val, &v) < 0 || v < r->min || v > r->max) return;
		break;
	case S_ENUM:
		if (num(val, &v) < 0 || (v += r->min) < r->min || v > r->max)
			return;
		break;
	case S_DATE: case S_TIME:
		if (date_time(r->type, val, &v) < 0) return;
		break;
	default:
		return;
	}
	if (v == sg_val[i]) return;
	sg_val[i] = (short) v;
	if (!(r->flags & S_NOSAVE)) changed = 1;
	if (sg_call(grp, SG_APPLY, i, NULL) == 1) sg_show(grp);
}

/* OK on a row of the main window */
static void ok(const char *k)
{
	const struct srow *rows;
	int n, i;

	rows = sg_rows(grp, &n);
	if (k == NULL || k[0] < 'a' || k[0] >= 'a' + n || k[1] != 0) return;
	i = k[0] - 'a';
	if (rows[i].type == S_ACT) {
		(void) sg_call(grp, SG_APPLY, i, NULL);
		(void) gm_busy(0);	/* "idle": done, nothing to show */
	} else if (rows[i].type == S_ITEM)
		(void) sg_call(grp, SG_OPEN, i, NULL);
}

int main(int argc, char **argv)
{
	char buf[128], ev[128], *f[GM_FIELDS];
	int i, t;

	if (argc != 2 || (grp = sg_find(argv[1])) < 0) {
		say("Usage: setgroup display|sound|time|about\n");
		return 1;
	}
	if (gm_init(buf, (int) sizeof buf) < 0) {
		say("setgroup: a window app: open it from the launcher's "
			"SETTINGS page\n");
		return 1;
	}
	for (i = 0; i < SG_ROWS; i++) sg_val[i] = -1;
	conf_load(grp);
	(void) sg_call(grp, SG_LOAD, -1, NULL);
	sg_show(grp);
	for (;;) {
		t = gm_event(ev, (int) sizeof ev, f, GM_FIELDS);
		if (t == GM_CLOSE) break;
		if (t == GM_REDRAW) sg_show(grp);
		if (t == GM_NONE || t == GM_REDRAW || f[1] == NULL) continue;
		if (strcmp(f[1], sg_name(grp)) != 0) {	/* the group's own */
			(void) sg_call(grp, SG_EVENT, t, f);
			continue;
		}
		if (t == GM_SUBMIT)
			for (i = 2; f[i] != NULL; i++) value(f[i]);
		else if (t == GM_OK) ok(f[2]);
	}
	(void) sg_call(grp, SG_LEAVE, -1, NULL);
	if (changed && conf_save(grp) < 0) {
		say("setgroup: cannot write /etc/flipper/");
		say(sg_name(grp));
		say(".conf\n");
		return 1;
	}
	return 0;
}
