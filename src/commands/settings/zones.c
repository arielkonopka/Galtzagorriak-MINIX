/* zones.c - the time zones and /etc/TZ (spec I.1, K.7, R18b.2).
 *
 * The zones people choose from are a text file on the card,
 * /usr/share/zones (the user's choice, 2026-11-26: a dictionary that
 * can grow without a new build, and no table in flash): a label and a
 * POSIX TZ rule a line, '#' comments.  Rules in the forms the ACK libc
 * parses (src/lib/ansi/misc.c): alphabetic names of 3 or more letters (no
 * <+03>), hours below 24 in the change times; zones whose rule needs more
 * (Israel, Egypt, Chile, Morocco) are left out rather than given a wrong
 * one, and "other" keeps whatever /etc/TZ holds.  No file (or nothing
 * in it): UTC alone.  The file is read a line at a time, again for each
 * lookup (a page of the list: a dozen reads of 3 KB from the cache), into
 * one entry kept in 72 B, through 136 B of static buffers (not the
 * stack: setgroup has 1536 B of it). */
#include <sys/types.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "zones.h"

static const char *tz_path = TZ_FILE;	/* tz_use(): another file */
static short z_n = -1;			/* entries, counted once */
static short z_i = -1;			/* the entry in z_label, z_rule */
static char z_label[TZ_LABEL_MAX], z_rule[TZ_RULE_MAX];

/* A line of the file (no '\n'): an entry into z_label and z_rule, 1; a
 * comment, a blank line or a bad one, 0. */
static int entry_line(char *p, int n)
{
	char *e = p + n, *r;
	int ln, rn;

	while (p < e && (*p == ' ' || *p == '\t')) p++;
	while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
	if (p == e || *p == '#') return 0;
	for (r = p; r < e && *r != ' ' && *r != '\t'; r++) ;
	ln = (int) (r - p);
	while (r < e && (*r == ' ' || *r == '\t')) r++;
	rn = (int) (e - r);
	if (ln < 1 || ln >= TZ_LABEL_MAX || rn < 1 || rn >= TZ_RULE_MAX)
		return 0;
	memcpy(z_label, p, (size_t) ln);
	z_label[ln] = 0;
	memcpy(z_rule, r, (size_t) rn);
	z_rule[rn] = 0;
	return 1;
}

/* Entry i into z_label, z_rule (i < 0: none, to count them): the number
 * of entries read; i is there if it is more than i. */
static int scan(int i)
{
	/* static: setgroup has 1536 B of stack in all, and 136 B more on it
	 * overflowed it under the launcher's environment (the user's test:
	 * OK on Zone ended Time) */
	static char c[64], ln[TZ_LABEL_MAX + TZ_RULE_MAX + 2];
	int fd, r, k, n = 0, m = 0, over = 0;

	z_i = -1;
	if ((fd = open(tz_path, O_RDONLY)) < 0) return 0;
	while ((r = (int) read(fd, c, sizeof c)) > 0)
		for (k = 0; k < r; k++) {
			if (c[k] != '\n') {
				if (m < (int) sizeof ln) ln[m++] = c[k];
				else over = 1;		/* too long: skipped */
				continue;
			}
			if (!over && entry_line(ln, m) && n++ == i) {
				close(fd);
				z_i = (short) i;
				return n;
			}
			m = over = 0;
		}
	if (r == 0 && m > 0 && !over && entry_line(ln, m) && n++ == i)
		z_i = (short) i;		/* a last line without '\n' */
	close(fd);
	return n;
}

/* Entry i, from the file or, with none, the built-in UTC: 1, or 0 past
 * the end. */
static int entry(int i)
{
	if (i < 0 || i >= tz_count()) return 0;
	if (z_n == 0) {				/* no file: UTC alone */
		strcpy(z_label, "UTC");
		strcpy(z_rule, "UTC0");
		z_i = 0;
		return 1;
	}
	if (z_i != i) (void) scan(i);
	return z_i == i;
}

void tz_use(const char *path)
{
	tz_path = path;
	z_n = z_i = -1;
}

int tz_count(void)
{
	if (z_n < 0) z_n = (short) scan(-1);
	return z_n > 0 ? z_n : 1;
}

const char *tz_label(int i)
{
	return entry(i) ? z_label : "other";
}

const char *tz_rule(int i)
{
	return entry(i) ? z_rule : "";
}

/* One line of buf at p, without its '\n' and blanks around it: its start
 * and length; *next after it. */
static const char *line(const char *p, int *len, const char **next)
{
	const char *e;

	while (*p == ' ' || *p == '\t') p++;
	for (e = p; *e != 0 && *e != '\n'; e++) ;
	*next = *e == '\n' ? e + 1 : e;
	while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
	*len = (int) (e - p);
	return p;
}

int tz_parse(const char *buf, char *rule, char *label)
{
	const char *p, *next;
	int n;

	rule[0] = label[0] = 0;
	for (; *buf != 0; buf = next) {
		p = line(buf, &n, &next);
		if (n == 0) continue;
		if (*p == '#') {			/* the label: "# Name" */
			p++;
			n--;
			while (n > 0 && (*p == ' ' || *p == '\t')) p++, n--;
			if (label[0] == 0 && n > 0 && n < TZ_LABEL_MAX) {
				memcpy(label, p, (size_t) n);
				label[n] = 0;
			}
			continue;
		}
		if (n >= TZ_RULE_MAX) return -1;
		memcpy(rule, p, (size_t) n);
		rule[n] = 0;
		return 0;
	}
	return -1;
}

int tz_find(const char *label, const char *rule)
{
	int i, n = tz_count();

	if (label != NULL && label[0] != 0)
		for (i = 0; i < n; i++)
			if (strcmp(tz_label(i), label) == 0
				&& (rule == NULL || rule[0] == 0
					|| strcmp(tz_rule(i), rule) == 0))
				return i;
	if (rule != NULL && rule[0] != 0)
		for (i = 0; i < n; i++)
			if (strcmp(tz_rule(i), rule) == 0) return i;
	return n;
}

int tz_format(char *buf, int n, int i)
{
	const char *l, *r;
	int k;

	if (!entry(i)) return -1;
	l = z_label;
	r = z_rule;
	k = (int) (strlen(l) + strlen(r)) + 4;
	if (k + 1 > n) return -1;
	strcpy(buf, "# ");
	strcat(buf, l);
	strcat(buf, "\n");
	strcat(buf, r);
	strcat(buf, "\n");
	return k;
}

int tz_mdays(int y, int m)
{
	static const unsigned char d[12] = { 31, 28, 31, 30, 31, 30, 31, 31,
		30, 31, 30, 31 };

	if (m < 1 || m > 12) return 0;
	if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) return 29;
	return d[m - 1];
}

/* ---- local time without the C library's time zone code (T24d) ----
 * localtime(), mktime() and putenv() are static members of the shared
 * libc (their data would be in every program using them); settings time
 * evaluates the POSIX rule itself: std offset [dst [offset]
 * [,start[/time],end[/time]]], start and end as Mm.w.d, Jn or n.  A rule
 * with a DST name but no dates uses the US rule (M3.2.0,M11.1.0), as
 * POSIX leaves it to the system. */
static int digit(char c)
{
	return c >= '0' && c <= '9';
}

static const char *skip_name(const char *p)
{
	if (*p == '<') {
		while (*p != 0 && *p != '>') p++;
		return *p == '>' ? p + 1 : p;
	}
	while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) p++;
	return p;
}

static const char *number(const char *p, long *v)
{
	*v = 0;
	while (digit(*p)) *v = *v * 10 + (*p++ - '0');
	return p;
}

/* [+-]hh[:mm[:ss]] in seconds */
static const char *hms(const char *p, long *s)
{
	long h, m = 0, sec = 0;
	int neg = 0;

	if (*p == '+' || *p == '-') neg = *p++ == '-';
	p = number(p, &h);
	if (*p == ':') p = number(p + 1, &m);
	if (*p == ':') p = number(p + 1, &sec);
	*s = h * 3600 + m * 60 + sec;
	if (neg) *s = -*s;
	return p;
}

long tz_days(int y, int m, int d)
{
	long n;

	y -= m <= 2;			/* the year from March */
	n = (long) y * 365 + y / 4 - y / 100 + y / 400;
	n += (153L * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	return n - 719468L;		/* 1970-01-01 */
}

void tz_date(long days, int *y, int *m, int *d)
{
	long z = days + 719468L, era, doe, yoe, doy, mp;

	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = z - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	*d = (int) (doy - (153 * mp + 2) / 5 + 1);
	*m = (int) (mp < 10 ? mp + 3 : mp - 9);
	*y = (int) (yoe + era * 400 + (*m <= 2));
}

/* A change date of year y: its day (days since 1970) and time of day. */
static const char *change(const char *p, int y, long *day, long *secs)
{
	long a = 0, b = 0, c = 0;

	*secs = 7200;
	if (*p == 'M') {
		p = number(p + 1, &a);
		if (*p == '.') p = number(p + 1, &b);
		if (*p == '.') p = number(p + 1, &c);
		if (a < 1 || a > 12) a = 1;
		/* the first day c (0 Sunday) of month a, then week b; 5: last */
		*day = tz_days(y, (int) a, 1);
		*day += ((c - (*day + 4) % 7) + 7) % 7;	/* 1970-01-01: Thu */
		*day += (b - 1) * 7;
		while (b >= 5 && *day >= tz_days(y, (int) a, 1) + tz_mdays(y, (int) a))
			*day -= 7;
	} else if (*p == 'J') {		/* 1..365, Feb 29 not counted */
		p = number(p + 1, &a);
		*day = tz_days(y, 1, 1) + a - 1;
		if (a >= 60 && tz_mdays(y, 2) == 29) (*day)++;
	} else {			/* 0..365 */
		p = number(p, &a);
		*day = tz_days(y, 1, 1) + a;
	}
	if (*p == '/') p = hms(p + 1, secs);
	return p;
}

long tz_off(const char *rule, time_t t)
{
	const char *p = skip_name(rule), *r;
	long std, dst, sd, ss, ed, es;
	time_t start, end, l;
	int y, m, d;

	p = hms(p, &std);
	std = -std;			/* POSIX: west positive */
	r = p;
	if ((p = skip_name(p)) == r) return std;	/* no DST */
	dst = std + 3600;
	if (*p != 0 && *p != ',') {
		p = hms(p, &dst);
		dst = -dst;
	}
	if (*p != ',') p = ",M3.2.0,M11.1.0";
	l = t + std;
	tz_date((long) (l / 86400 - (l % 86400 < 0)), &y, &m, &d);
	p = change(p + 1, y, &sd, &ss);
	if (*p != ',') return std;
	(void) change(p + 1, y, &ed, &es);
	start = (time_t) sd * 86400 + ss - std;	/* in standard time */
	end = (time_t) ed * 86400 + es - dst;	/* in summer time */
	if (start < end) return t >= start && t < end ? dst : std;
	return t >= end && t < start ? std : dst;	/* the south */
}

time_t tz_utc(const char *rule, time_t local)
{
	return local - tz_off(rule, local - tz_off(rule, local));
}
