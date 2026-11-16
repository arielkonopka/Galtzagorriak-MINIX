/* date - Display (or set) the date and time		Author: V. Archer */

#include <sys/types.h>
#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define	MIN	60L		/* # seconds in a minute */
#define	HOUR	(60 * MIN)	/* # seconds in an hour */
#define	DAY	(24 * HOUR)	/* # seconds in a day */
#define	YEAR	(365 * DAY)	/* # seconds in a (non-leap) year */

int qflag, uflag, sflag;

/* Default output file descriptor.
 */
int outfd = 1;

_PROTOTYPE(int main, (int argc, char **argv));
_PROTOTYPE(void putchar, (int c));
_PROTOTYPE(void pstring, (char *s, int len));
_PROTOTYPE(void pldecimal, (unsigned long long d, int digits));
_PROTOTYPE(void pdecimal, (int d, int digits));
_PROTOTYPE(void fmtdate, (char *format, time_t t, struct tm *p));
_PROTOTYPE(time_t make_time, (char *t));
_PROTOTYPE(long days_from_civil, (long y, int m, int d));
_PROTOTYPE(void usage, (void));

/* Main module. Handles P1003.2 date and system administrator's date.  The
 * clock keeps UTC (spec I.1): a date entered is local time (TZ), or UTC
 * with -u or a trailing Z.
 */
int main(argc, argv)
int argc;
char **argv;
{
  time_t t;
  char *format;
  char time_buf[40];
  int n;
  int i;

  time(&t);

  i = 1;
  while (i < argc && argv[i][0] == '-') {
	char *opt = argv[i++] + 1, *end;

	if (opt[0] == '-' && opt[1] == 0) break;

	while (*opt != 0) switch (*opt++) {
	case 'q':
		qflag = 1;
		break;
	case 's':
		sflag = 1;
		break;
	case 'u':
		uflag = 1;
		break;
	case 't':
		if (*opt == 0) {
			if (i == argc) usage();
			opt = argv[i++];
		}
		t = strtoul(opt, &end, 10);
		if (*end != 0) usage();
		opt = "";
		break;
	}
  }

  if (!qflag && i < argc && ('0' <= argv[i][0] && argv[i][0] <= '9')) {
	t = make_time(argv[i++]);
	sflag = 1;
  }

  format = "%c";
  if (i < argc && argv[i][0] == '+') format = argv[i++] + 1;

  if (i != argc) usage();

  if (qflag) {
	pstring("\nPlease enter date: MMDDYYhhmmss. Then hit the RETURN key.\n", -1);
	n = read(0, time_buf, sizeof(time_buf));
	if (n > 0 && time_buf[n-1] == '\n') n--;
	if (n >= 0) time_buf[n] = 0;
	t = make_time(time_buf);
	sflag = 1;
  }

  if (sflag && stime(&t) != 0) {
	outfd = 2;
	pstring(errno == EINVAL ? "The clock cannot keep that time\n"
				: "No permission to set time\n", -1);
	return(1);
  }

  fmtdate(format, t, uflag ? gmtime(&t) : localtime(&t));
  putchar('\n');
  return(0);
}

/* Replacement for stdio putchar().
 */
void putchar(c)
int c;
{
  static char buf[1024];
  static char *bp = buf;

  if (c != 0) *bp++ = c;
  if (c == 0 || c == '\n' || bp == buf + sizeof(buf)) {
	write(outfd, buf, bp - buf);
	bp = buf;
  }
}

/* Internal function that prints a n-digits number. Replaces stdio in our
 * specific case.
 */
void pldecimal(d, digits)
unsigned long long d;
int digits;
{
  digits--;
  if (d > 9 || digits > 0) pldecimal(d / 10, digits);
  putchar('0' + (d % 10));
}

void pdecimal(d, digits)
int d, digits;
{
  pldecimal((unsigned long long) (unsigned) d, digits);
}

/* Internal function that prints a fixed-size string. Replaces stdio in our
 * specific case.
 */
void pstring(s, len)
char *s;
int len;
{
  while (*s)
	if (len--)
		putchar(*s++);
	else
		break;
}

/* Format the date, using the given locale string. A special case is the
 * TZ which might be a sign followed by four digits (New format time zone).
 */
void fmtdate(format, t, p)
char *format;
time_t t;
struct tm *p;
{
  int i;
  char *s;
  static char *wday[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
		       "Thursday", "Friday", "Saturday"};
  static char *month[] = {"January", "February", "March", "April",
			"May", "June", "July", "August",
		    "September", "October", "November", "December"};

  p= uflag ? gmtime(&t) : localtime(&t);
  if (p == NULL) {
	outfd = 2;
	pstring("date: time out of range\n", -1);
	exit(1);
  }

  while (*format)
	if (*format == '%') {
		switch (*++format) {
		    case 'A':
			pstring(wday[p->tm_wday], -1);
			break;
		    case 'B':
			pstring(month[p->tm_mon], -1);
			break;
		    case 'D':
			pdecimal(p->tm_mon + 1, 2);
			putchar('/');
			pdecimal(p->tm_mday, 2);
			putchar('/');
		    case 'y':
			pdecimal(p->tm_year % 100, 2);
			break;
		    case 'H':
			pdecimal(p->tm_hour, 2);
			break;
		    case 'I':
			i = p->tm_hour % 12;
			pdecimal(i ? i : 12, 2);
			break;
		    case 'M':
			pdecimal(p->tm_min, 2);
			break;
		    case 'X':
		    case 'T':
			pdecimal(p->tm_hour, 2);
			putchar(':');
			pdecimal(p->tm_min, 2);
			putchar(':');
		    case 'S':
			pdecimal(p->tm_sec, 2);
			break;
		    case 'U':
			pdecimal((p->tm_yday - p->tm_wday + 13) / 7, 2);
			break;
		    case 'W':
			if (--(p->tm_wday) < 0) p->tm_wday = 6;
			pdecimal((p->tm_yday - p->tm_wday + 13) / 7, 2);
			if (++(p->tm_wday) > 6) p->tm_wday = 0;
			break;
		    case 'Y':
			pdecimal(p->tm_year + 1900, 4);
			break;
		    case 'Z':
			if (uflag) {
				s = "UTC";
			} else {
				s = (p->tm_isdst == 1) ? tzname[1] : tzname[0];
			}
			pstring(s, strlen(s));
			break;
		    case 'a':
			pstring(wday[p->tm_wday], 3);
			break;
		    case 'b':
		    case 'h':
			pstring(month[p->tm_mon], 3);
			break;
		    case 'c':
			if (!(s = getenv("LC_TIME")))
				s = "%a %b %e %T %Z %Y";
			fmtdate(s, t, p);
			break;
		    case 'd':
			pdecimal(p->tm_mday, 2);
			break;
		    case 'e':
			if (p->tm_mday < 10) putchar(' ');
			pdecimal(p->tm_mday, 1);
			break;
		    case 'j':
			pdecimal(p->tm_yday + 1, 3);
			break;
		    case 'm':
			pdecimal(p->tm_mon + 1, 2);
			break;
		    case 'n':	putchar('\n');	break;
		    case 'p':
			if (p->tm_hour < 12)
				putchar('A');
			else
				putchar('P');
			putchar('M');
			break;
		    case 'r':
			fmtdate("%I:%M:%S %p", t, p);
			break;
		    case 's':
			if (t < 0) putchar('-');
			pldecimal(t < 0 ? -(unsigned long long) t
					: (unsigned long long) t, 0);
			break;
		    case 't':	putchar('\t');	break;
		    case 'w':
			putchar('0' + p->tm_wday);
			break;
		    case 'x':
			fmtdate("%B %e %Y", t, p);
			break;
		    case '%':	putchar('%');	break;
		    case '\0':	format--;
		}
		format++;
	} else
		putchar(*format++);
}

/* Days since 1970-01-01 of a date (days-from-civil, H. Hinnant). */
long days_from_civil(y, m, d)
long y;
int m, d;
{
  long era;
  unsigned long yoe, doe;

  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned long) (y - era * 400);
  doe = yoe * 365 + yoe / 4 - yoe / 100
	+ (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  return era * 146097 + (long) doe - 719468;
}

/* Convert a date string into UTC seconds (spec R2d.8): local time, or UTC
 * with -u or a trailing Z.  [[MMDDYY]hhmm[ss]][Z] or MMDDCCYYhhmmss[Z]; a
 * two-digit year is 1970-2069. */
time_t make_time(t)
char *t;
{
  struct tm tm;				/* user specified time */
  time_t now;				/* current time */
  int utc = uflag;			/* the string is UTC */
  int i;				/* general index */
  int fld;				/* number of fields */
  int f[7];				/* time fields */
  long year;
  static int days_per_month[12] = {
	31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

/* Get current time just in case */
  now = time((time_t *) 0);
  tm  = utc ? *gmtime(&now) : *localtime(&now);
  tm.tm_sec   = 0;
  year = tm.tm_year + 1900L;

/* Parse the time */
  i = strlen(t);
  if (i > 0 && (t[i-1] == 'Z' || t[i-1] == 'z')) {
	t[i-1] = 0;
	utc = 1;
  }
  for (fld = 0; fld < sizeof(f)/sizeof(f[0]); fld++) {
	if (*t == 0) break;
	f[fld] = 0;
	for (i = 0; i < 2; i++, t++) {
		if (*t < '0' || *t > '9') usage();
		f[fld] = f[fld] * 10 + *t - '0';
	}
  }
  if (*t != 0) usage();

  switch (fld) {
  case 2:
	tm.tm_hour = f[0]; tm.tm_min  = f[1]; break;

  case 3:
	tm.tm_hour = f[0]; tm.tm_min  = f[1]; tm.tm_sec  = f[2];
	break;

  case 5:
  case 6:
	tm.tm_mon  = f[0] - 1; tm.tm_mday = f[1];
	year = f[2] < 70 ? 2000 + f[2] : 1900 + f[2];
	tm.tm_hour = f[3]; tm.tm_min  = f[4];
	tm.tm_sec = fld == 6 ? f[5] : 0;
	break;

  case 7:
	tm.tm_mon  = f[0] - 1; tm.tm_mday = f[1];
	year = f[2] * 100L + f[3];
	tm.tm_hour = f[4]; tm.tm_min  = f[5]; tm.tm_sec  = f[6];
	break;

  default:
	usage();
  }

  if (tm.tm_mon  < 0  || tm.tm_mon  > 11 ||
      tm.tm_mday < 1  || tm.tm_mday > days_per_month[tm.tm_mon] ||
      (tm.tm_mon == 1 && tm.tm_mday == 29
	&& !(year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) ||
      tm.tm_hour > 23 || tm.tm_min  > 59 || tm.tm_sec > 59) {
    outfd = 2;
    pstring("Illegal date format\n", -1);
    exit(1);
  }

  if (utc) {
	return (time_t) days_from_civil(year, tm.tm_mon + 1, tm.tm_mday) * DAY
		+ tm.tm_hour * HOUR + tm.tm_min * MIN + tm.tm_sec;
  }
  /* Local time: mktime follows TZ, daylight saving time included. */
  tm.tm_year = (int) (year - 1900);
  tm.tm_isdst = -1;
  return mktime(&tm);
}

/* (Extended) Posix prototype of date. */
void usage()
{
  outfd = 2;
  pstring("Usage: date [-qsu] [-t seconds] [[MMDDYY]hhmm[ss][Z] | MMDDCCYYhhmmss[Z]] [+format]\n", -1);
  exit(1);
}
