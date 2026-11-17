/* tspec.c - libapp: timer files and their time rules, the notification
 * policy rules (spec F.2, R7b.4).  No I/O here: the host tests run this
 * file as it is (flipper/lib/app/test_app.c).
 *
 * Times: once timers are a UTC instant; daily and interval timers are
 * local wall-clock times (TZ, spec I.1) and go through mktime(), so a
 * daylight-saving change moves them with the clock.  An interval timer
 * fires at Start, Start + Every, ... up to End (End itself if Inclusive),
 * counted in wall-clock minutes from Start each day.
 */
#include <string.h>
#include <stdlib.h>
#include "app.h"

const char *const app_cats[3] = { "info", "reminder", "alarm" };
static const char days2[] = "SuMoTuWeThFrSa";

int app_id_ok(const char *id)
{
	int n;

	if (id == NULL || id[0] == '.' || id[0] == 0) return 0;
	for (n = 0; id[n] != 0; n++) {
		char c = id[n];

		if (n >= APP_ID_MAX) return 0;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
		    || (c >= '0' && c <= '9') || c == '.' || c == '_'
		    || c == '-')) return 0;
	}
	return 1;
}

int app_cat(const char *s)
{
	int i;

	for (i = 0; i < 3; i++) if (strcmp(s, app_cats[i]) == 0) return i;
	return -1;
}

/* The next "Key: value" line of a buffer (changed in place); NULL at the
 * end.  Blank lines and lines starting with '#' are skipped. */
char *ff_next(char **pos, char **val)
{
	char *p = *pos, *key, *e, *v;

	while (*p != 0) {
		key = p;
		while (*p != 0 && *p != '\n') p++;
		if (*p == '\n') *p++ = 0;
		*pos = p;
		if (*key == '#' || (v = strchr(key, ':')) == NULL) continue;
		*v++ = 0;
		while (*v == ' ' || *v == '\t') v++;
		for (e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\r'
						|| e[-1] == '\t'); ) *--e = 0;
		*val = v;
		return key;
	}
	*pos = p;
	return NULL;
}

int ff_hm(const char *s)
{
	int h, m;

	if (s[0] < '0' || s[0] > '9') return -1;
	h = s[0] - '0';
	s++;
	if (*s >= '0' && *s <= '9') h = h * 10 + *s++ - '0';
	if (*s++ != ':' || s[0] < '0' || s[0] > '5' || s[1] < '0' || s[1] > '9'
	    || s[2] != 0 || h > 23) return -1;
	m = (s[0] - '0') * 10 + s[1] - '0';
	return h * 60 + m;
}

static int yesno(const char *v)
{
	if (strcmp(v, "yes") == 0) return 1;
	if (strcmp(v, "no") == 0) return 0;
	return -1;
}

static int days(const char *v)
{
	int mask = 0, i;

	if (strcmp(v, "all") == 0 || strcmp(v, "daily") == 0) return 0x7F;
	if (strcmp(v, "weekdays") == 0) return 0x3E;
	if (strcmp(v, "weekend") == 0) return 0x41;
	while (*v != 0) {
		if (*v == ' ' || *v == ',') { v++; continue; }
		for (i = 0; i < 7; i++)
			if (v[0] == days2[2*i] && v[1] == days2[2*i+1]) break;
		if (i == 7) return -1;
		mask |= 1 << i;
		v += 2;
		while (*v >= 'a' && *v <= 'z') v++;	/* Mon, Monday */
	}
	return mask;
}

static void copy(char *to, const char *v, int size)
{
	strncpy(to, v, size - 1);
	to[size - 1] = 0;
}

/* Parse a timer file (buf is changed).  The id is not touched. */
int timer_parse(struct app_timer *t, char *buf)
{
	char *k, *v, *pos = buf;
	int n;
	time_t at;

	memset((char *) t + sizeof t->id, 0, sizeof *t - sizeof t->id);
	t->days = 0x7F;
	t->flags = NF_ALL;
	t->start = t->end = 0xFFFF;
	while ((k = ff_next(&pos, &v)) != NULL) {
		n = 0;
		if (strcmp(k, "Kind") == 0) {
			t->kind = strcmp(v, "once") == 0 ? TK_ONCE
				: strcmp(v, "daily") == 0 ? TK_DAILY
				: strcmp(v, "interval") == 0 ? TK_INTERVAL : 0;
			n = t->kind == 0;
		} else if (strcmp(k, "At") == 0) {
			for (at = 0; *v >= '0' && *v <= '9'; v++)
				if ((at = at * 10 + *v - '0') > 0x7FFFFFFFFFLL)
					break;
			t->at = at;
			n = *v != 0 || at == 0;
		} else if (strcmp(k, "Time") == 0 || strcmp(k, "Start") == 0) {
			n = (t->start = ff_hm(v)) == 0xFFFF;
		} else if (strcmp(k, "End") == 0) {
			n = (t->end = strcmp(v, "24:00") == 0 ? 1440 : ff_hm(v))
								== 0xFFFF;
		} else if (strcmp(k, "Every") == 0) {
			t->every = atoi(v);
			n = t->every < 1 || t->every > 1440;
		} else if (strcmp(k, "Inclusive") == 0) {
			n = (t->incl = yesno(v)) > 1;
		} else if (strcmp(k, "Days") == 0) {
			n = (t->days = days(v)) == 0 || t->days > 0x7F;
		} else if (strcmp(k, "Category") == 0) {
			n = (t->cat = app_cat(v)) > 2;
		} else if (strcmp(k, "Missed") == 0) {
			t->missed = strcmp(v, "fire-once") == 0;
			n = !t->missed && strcmp(v, "skip") != 0;
		} else if (strcmp(k, "Action") == 0) {
			t->action = strcmp(v, "run") == 0 ? TA_RUN : TA_NOTIFY;
			n = t->action == TA_NOTIFY && strcmp(v, "notify") != 0;
		} else if (strcmp(k, "Program") == 0) {
			n = v[0] != '/' || strstr(v, "..") != NULL
				|| strlen(v) >= sizeof t->prog;
			copy(t->prog, v, sizeof t->prog);
		} else if (strcmp(k, "Title") == 0) {
			copy(t->title, v, sizeof t->title);
		} else if (strcmp(k, "Text") == 0) {
			copy(t->text, v, sizeof t->text);
		} else if (strcmp(k, "Snooze") == 0) {
			n = (unsigned) atoi(v) > 60;
			t->snooze = atoi(v);
		} else {
			int f = strcmp(k, "Sound") == 0 ? NF_SOUND
				: strcmp(k, "Vibro") == 0 ? NF_VIBRO
				: strcmp(k, "Led") == 0 ? NF_LED : 0;

			if (f != 0) {
				if ((n = yesno(v)) == 1) t->flags |= f;
				else if (n == 0) t->flags &= ~f;
				n = n < 0;
			}
		}				/* other keys: ignored */
		if (n) return -1;
	}
	if (t->action == TA_RUN && t->prog[0] != '/') return -1;
	switch (t->kind) {
	case TK_ONCE:	return t->at != 0 ? 0 : -1;
	case TK_DAILY:	return t->start < 1440 ? 0 : -1;
	case TK_INTERVAL:
		return t->start < t->end && t->end <= 1440 && t->every != 0
			? 0 : -1;
	}
	return -1;
}

/* Append s to buf (size bytes, n used); returns the new n, or size if
 * it did not fit. */
static int put(char *buf, int size, int n, const char *s)
{
	int l = strlen(s);

	if (n >= size || n + l >= size) return size;
	memcpy(buf + n, s, l + 1);
	return n + l;
}

static char *num(char *end, unsigned long long v)
{
	*--end = 0;
	do *--end = '0' + (int) (v % 10); while ((v /= 10) != 0);
	return end;
}

static char *hm(char *b, int m)
{
	b[0] = '0' + m / 600; b[1] = '0' + m / 60 % 10; b[2] = ':';
	b[3] = '0' + m % 60 / 10; b[4] = '0' + m % 10; b[5] = 0;
	return m == 1440 ? "24:00" : b;
}

int timer_format(const struct app_timer *t, char *buf, int size)
{
	static const char *const kinds[4] = { "", "once", "daily", "interval" };
	char nb[24], *p;
	int n = 0, i;

	buf[0] = 0;
	n = put(buf, size, n, "Filetype: Galtzagorriak Timer\nVersion: 1\n"
			      "Kind: ");
	n = put(buf, size, n, kinds[t->kind & 3]);
	if (t->kind == TK_ONCE) {
		n = put(buf, size, n, "\nAt: ");
		n = put(buf, size, n, num(nb + sizeof nb,
					(unsigned long long) t->at));
	} else {
		n = put(buf, size, n, t->kind == TK_DAILY ? "\nTime: "
							  : "\nStart: ");
		n = put(buf, size, n, hm(nb, t->start));
		if (t->kind == TK_INTERVAL) {
			n = put(buf, size, n, "\nEnd: ");
			n = put(buf, size, n, hm(nb, t->end));
			n = put(buf, size, n, "\nEvery: ");
			n = put(buf, size, n, num(nb + sizeof nb, t->every));
			n = put(buf, size, n, t->incl ? "\nInclusive: yes"
						      : "\nInclusive: no");
		}
		n = put(buf, size, n, "\nDays:");
		for (i = 0, p = nb; i < 7; i++)
			if (t->days & 1 << i) {
				*p++ = ' '; *p++ = days2[2*i]; *p++ = days2[2*i+1];
			}
		*p = 0;
		n = put(buf, size, n, nb);
	}
	n = put(buf, size, n, "\nCategory: ");
	n = put(buf, size, n, app_cats[t->cat > 2 ? 0 : t->cat]);
	n = put(buf, size, n, t->missed ? "\nMissed: fire-once\nAction: "
					: "\nMissed: skip\nAction: ");
	if (t->action == TA_RUN) {
		n = put(buf, size, n, "run\nProgram: ");
		n = put(buf, size, n, t->prog);
	} else {
		n = put(buf, size, n, "notify\nSound: ");
		n = put(buf, size, n, t->flags & NF_SOUND ? "yes" : "no");
		n = put(buf, size, n, t->flags & NF_VIBRO ? "\nVibro: yes"
							  : "\nVibro: no");
		n = put(buf, size, n, t->flags & NF_LED ? "\nLed: yes"
							: "\nLed: no");
		n = put(buf, size, n, "\nSnooze: ");
		n = put(buf, size, n, num(nb + sizeof nb, t->snooze));
	}
	n = put(buf, size, n, "\nTitle: ");
	n = put(buf, size, n, t->title);
	n = put(buf, size, n, "\nText: ");
	n = put(buf, size, n, t->text);
	n = put(buf, size, n, "\n");
	return n < size ? n : -1;
}

/* The instant of minute m (from local midnight) of the day `off` days
 * after the day in *b. */
static time_t at_min(const struct tm *b, int off, int m)
{
	struct tm tm;

	tm = *b;
	tm.tm_mday += off;
	tm.tm_hour = m / 60;
	tm.tm_min = m % 60;
	tm.tm_sec = 0;
	tm.tm_isdst = -1;
	return mktime(&tm);
}

/* The last minute of an interval timer's window that is an occurrence. */
static int last_min(const struct app_timer *t)
{
	int top = t->incl ? t->end : t->end - 1;

	return t->start + (top - t->start) / t->every * t->every;
}

/* The first occurrence after `after`, 0 if there is none. */
time_t timer_next(const struct app_timer *t, time_t after)
{
	struct tm b;
	time_t c;
	int i, m, m0;

	if (t->kind == TK_ONCE) return t->at > after ? t->at : 0;
	b = *localtime(&after);
	m0 = b.tm_hour * 60 + b.tm_min;
	for (i = 0; i <= 7; i++) {
		if (!(t->days & 1 << (b.tm_wday + i) % 7)) continue;
		if (t->kind == TK_DAILY) {
			if ((c = at_min(&b, i, t->start)) > after) return c;
			continue;
		}
		m = t->start;
		if (i == 0 && m0 > m) m += (m0 - m) / t->every * t->every;
		for (; m <= last_min(t); m += t->every)
			if ((c = at_min(&b, i, m)) > after) return c;
	}
	return 0;
}

/* The last occurrence at or before `upto`, 0 if none in the past week. */
time_t timer_prev(const struct app_timer *t, time_t upto)
{
	struct tm b;
	time_t c;
	int i, m, m0;

	if (t->kind == TK_ONCE) return t->at <= upto ? t->at : 0;
	b = *localtime(&upto);
	m0 = b.tm_hour * 60 + b.tm_min;
	for (i = 0; i >= -7; i--) {
		if (!(t->days & 1 << (b.tm_wday + i + 7) % 7)) continue;
		if (t->kind == TK_DAILY) {
			if ((c = at_min(&b, i, t->start)) <= upto) return c;
			continue;
		}
		m = last_min(t);
		if (i == 0) {
			if (m0 < t->start) continue;
			if (m0 < m) m = t->start + (m0 - t->start) / t->every
								* t->every;
		}
		for (; m >= t->start; m -= t->every)
			if ((c = at_min(&b, i, m)) <= upto) return c;
	}
	return 0;
}

/* Whether the timer has an occurrence in (last, now] and what to do. */
int timer_due(const struct app_timer *t, time_t last, time_t now)
{
	time_t p = timer_prev(t, now);

	if (p == 0 || p <= last) return TD_NONE;
	if (now - p <= APP_GRACE) return TD_FIRE;
	return t->missed ? TD_LATE : TD_SKIP;
}

/* The policy file's lines (buf is changed); unknown keys are ignored. */
void notify_parse(struct notify_policy *p, char *buf)
{
	char *k, *v, *pos = buf;
	int n;

	while ((k = ff_next(&pos, &v)) != NULL) {
		if (strcmp(k, "Volume") == 0) {
			n = atoi(v);
			p->volume = n < 0 ? 0 : n > 100 ? 100 : n;
		} else if (strcmp(k, "Vibro") == 0) {
			if ((n = yesno(v)) >= 0) p->vibro = n;
		} else if (strcmp(k, "Led") == 0) {
			if ((n = yesno(v)) >= 0) p->led = n;
		} else if (strcmp(k, "Dnd") == 0) {
			char *dash = strchr(v, '-');

			p->dnd_from = p->dnd_to = -1;
			if (dash != NULL) {
				*dash = 0;
				p->dnd_from = ff_hm(v);
				p->dnd_to = ff_hm(dash + 1);
				if (p->dnd_from < 0 || p->dnd_to < 0)
					p->dnd_from = p->dnd_to = -1;
			}
		}
	}
}

/* What a notification of category cat that wants `want` (NF_*) may use
 * at `now`: 0 = do not show it (do-not-disturb), else NF_SHOW and the
 * means.  An alarm always rings as it says itself. */
int notify_flags(const struct notify_policy *p, int cat, int want, time_t now)
{
	struct tm *tm;
	int m;

	if (cat == TC_ALARM) return NF_SHOW | (want & NF_ALL);
	if (p->dnd_from >= 0 && p->dnd_from != p->dnd_to) {
		tm = localtime(&now);
		m = tm->tm_hour * 60 + tm->tm_min;
		if (p->dnd_from < p->dnd_to ? m >= p->dnd_from && m < p->dnd_to
					    : m >= p->dnd_from || m < p->dnd_to)
			return 0;
	}
	if (p->volume == 0) want &= ~NF_SOUND;
	if (!p->vibro) want &= ~NF_VIBRO;
	if (!p->led) want &= ~NF_LED;
	return NF_SHOW | (want & NF_ALL);
}
