/* zones.c - the time zone table and /etc/TZ (spec I.1, K.7, R18b.2).
 *
 * Rules are POSIX TZ strings in the forms the ACK libc parses
 * (src/lib/ansi/misc.c): alphabetic names of 3 or more letters (no <+03>),
 * hours below 24 in the change times (no /24, /26).  Zones whose rule
 * needs more (Israel, Egypt, Chile, Morocco) are left out rather than
 * given a wrong one; "other" keeps whatever /etc/TZ holds.  Rules as of
 * 2026 (tzdata 2025): Mexico and Iran without summer time, Kazakhstan on
 * +05.  One const string, "label\0rule\0label\0rule\0...\0": flash, 0 B
 * RAM (an array of pointers would sit in RAM, relocated, in a PIC
 * program); a lookup walks it, 80 short entries. */
#include <string.h>
#include "zones.h"

#define EU_CET	"CET-1CEST,M3.5.0,M10.5.0/3"
#define EU_EET	"EET-2EEST,M3.5.0/3,M10.5.0/4"
#define EU_WET	"WET0WEST,M3.5.0/1,M10.5.0"
#define US(s, d)	s d ",M3.2.0,M11.1.0"

#define ZONES \
	Z("UTC", "UTC0") \
	Z("Europe/London", "GMT0BST,M3.5.0/1,M10.5.0") \
	Z("Europe/Dublin", "GMT0IST,M3.5.0/1,M10.5.0") \
	Z("Europe/Lisbon", EU_WET) \
	Z("Europe/Madrid", EU_CET) \
	Z("Europe/Paris", EU_CET) \
	Z("Europe/Brussels", EU_CET) \
	Z("Europe/Amsterdam", EU_CET) \
	Z("Europe/Berlin", EU_CET) \
	Z("Europe/Zurich", EU_CET) \
	Z("Europe/Rome", EU_CET) \
	Z("Europe/Vienna", EU_CET) \
	Z("Europe/Prague", EU_CET) \
	Z("Europe/Warsaw", EU_CET) \
	Z("Europe/Budapest", EU_CET) \
	Z("Europe/Belgrade", EU_CET) \
	Z("Europe/Copenhagen", EU_CET) \
	Z("Europe/Oslo", EU_CET) \
	Z("Europe/Stockholm", EU_CET) \
	Z("Europe/Helsinki", EU_EET) \
	Z("Europe/Tallinn", EU_EET) \
	Z("Europe/Riga", EU_EET) \
	Z("Europe/Vilnius", EU_EET) \
	Z("Europe/Kyiv", EU_EET) \
	Z("Europe/Bucharest", EU_EET) \
	Z("Europe/Sofia", EU_EET) \
	Z("Europe/Athens", EU_EET) \
	Z("Europe/Istanbul", "TRT-3") \
	Z("Europe/Minsk", "MSK-3") \
	Z("Europe/Moscow", "MSK-3") \
	Z("Atlantic/Reykjavik", "GMT0") \
	Z("Atlantic/Canary", EU_WET) \
	Z("Atlantic/Azores", "AZOT1AZOST,M3.5.0/0,M10.5.0/1") \
	Z("Africa/Lagos", "WAT-1") \
	Z("Africa/Johannesburg", "SAST-2") \
	Z("Africa/Nairobi", "EAT-3") \
	Z("Asia/Riyadh", "AST-3") \
	Z("Asia/Tehran", "IRST-3:30") \
	Z("Asia/Dubai", "GST-4") \
	Z("Asia/Tbilisi", "GET-4") \
	Z("Asia/Yerevan", "AMT-4") \
	Z("Asia/Karachi", "PKT-5") \
	Z("Asia/Tashkent", "UZT-5") \
	Z("Asia/Almaty", "ALMT-5") \
	Z("Asia/Yekaterinburg", "YEKT-5") \
	Z("Asia/Kolkata", "IST-5:30") \
	Z("Asia/Kathmandu", "NPT-5:45") \
	Z("Asia/Dhaka", "BDT-6") \
	Z("Asia/Bangkok", "ICT-7") \
	Z("Asia/Jakarta", "WIB-7") \
	Z("Asia/Novosibirsk", "NOVT-7") \
	Z("Asia/Shanghai", "CST-8") \
	Z("Asia/Hong_Kong", "HKT-8") \
	Z("Asia/Singapore", "SGT-8") \
	Z("Asia/Manila", "PHT-8") \
	Z("Australia/Perth", "AWST-8") \
	Z("Asia/Seoul", "KST-9") \
	Z("Asia/Tokyo", "JST-9") \
	Z("Australia/Darwin", "ACST-9:30") \
	Z("Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3") \
	Z("Australia/Brisbane", "AEST-10") \
	Z("Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3") \
	Z("Asia/Vladivostok", "VLAT-10") \
	Z("Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3") \
	Z("Pacific/Honolulu", "HST10") \
	Z("America/Anchorage", US("AKST9", "AKDT")) \
	Z("America/Los_Angeles", US("PST8", "PDT")) \
	Z("America/Phoenix", "MST7") \
	Z("America/Denver", US("MST7", "MDT")) \
	Z("America/Chicago", US("CST6", "CDT")) \
	Z("America/Mexico_City", "CST6") \
	Z("America/New_York", US("EST5", "EDT")) \
	Z("America/Bogota", "COT5") \
	Z("America/Lima", "PET5") \
	Z("America/Halifax", US("AST4", "ADT")) \
	Z("America/Caracas", "VET4") \
	Z("America/St_Johns", US("NST3:30", "NDT")) \
	Z("America/Sao_Paulo", "BRT3") \
	Z("America/Buenos_Aires", "ART3")

#define Z(l, r)	l "\0" r "\0"
static const char zones[] = ZONES;	/* and the final '\0': the end */
#undef Z

/* Entry i's label (rule: its next string), or NULL past the end. */
static const char *entry(int i)
{
	const char *p = zones;

	if (i < 0) return NULL;
	for (; *p != 0 && i > 0; i--) {
		p += strlen(p) + 1;
		p += strlen(p) + 1;
	}
	return *p != 0 ? p : NULL;
}

int tz_count(void)
{
	const char *p = zones;
	int n = 0;

	while (*p != 0) {
		p += strlen(p) + 1;
		p += strlen(p) + 1;
		n++;
	}
	return n;
}

const char *tz_label(int i)
{
	const char *p = entry(i);

	return p != NULL ? p : "other";
}

const char *tz_rule(int i)
{
	const char *p = entry(i);

	return p != NULL ? p + strlen(p) + 1 : "";
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
	const char *l = entry(i), *r;
	int k;

	if (l == NULL) return -1;
	r = l + strlen(l) + 1;
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

long tz_off(const char *rule, long t)
{
	const char *p = skip_name(rule), *r;
	long std, dst, sd, ss, ed, es, start, end;
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
	tz_date((t + std) / 86400 - ((t + std) % 86400 < 0), &y, &m, &d);
	p = change(p + 1, y, &sd, &ss);
	if (*p != ',') return std;
	(void) change(p + 1, y, &ed, &es);
	start = sd * 86400 + ss - std;	/* in standard time */
	end = ed * 86400 + es - dst;	/* in summer time */
	if (start < end) return t >= start && t < end ? dst : std;
	return t >= end && t < start ? std : dst;	/* the south */
}

long tz_utc(const char *rule, long local)
{
	return local - tz_off(rule, local - tz_off(rule, local));
}
