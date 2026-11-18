/*
 * gmtime - convert the calendar time into broken down time
 */
/* $Header: gmtime.c,v 1.4 91/04/22 13:20:27 ceriel Exp $ */

/* Galtzagorriak (spec R2d.4): time_t is 64-bit.  The date comes from a
 * days-from-civil conversion (H. Hinnant, "chrono-compatible low-level date
 * algorithms", public domain), no year loop.  Times from 1970 to 2106 take
 * a 32-bit path; others divide in 64 bits.  Out of range (beyond about
 * +-5 million years): NULL, as POSIX (EOVERFLOW; ERANGE here).
 */

#include	<time.h>
#include	<limits.h>
#include	<errno.h>
#include	"loc_time.h"

#define	MAXDAYS	0x7F000000L	/* |days| fitting days + 719468 in a long */

struct tm *
gmtime(register const time_t *timer)
{
	static struct tm br_time;
	register struct tm *timep = &br_time;
	time_t time = *timer;
	unsigned long dayclock, doe, yoe, doy, mp;
	long days, era, y;

	if ((unsigned long long) time <= 0xFFFFFFFFULL) {
		/* 1970-2106: 32-bit division only */
		dayclock = (unsigned long) time % SECS_DAY;
		days = (long) ((unsigned long) time / SECS_DAY);
	} else {
		long long d = time / SECS_DAY;
		long long r = time % SECS_DAY;

		if (r < 0) { r += SECS_DAY; d--; }
		if (d > MAXDAYS || d < -MAXDAYS) {
			errno = ERANGE;	/* no EOVERFLOW in MINIX */
			return NULL;
		}
		days = (long) d;
		dayclock = (unsigned long) r;
	}

	timep->tm_sec = dayclock % 60;
	timep->tm_min = (dayclock % 3600) / 60;
	timep->tm_hour = dayclock / 3600;
	timep->tm_wday = (int) ((days % 7 + 11) % 7);	/* day 0 was a thursday */

	/* civil_from_days: eras of 400 years from 0000-03-01 */
	days += 719468;
	era = (days >= 0 ? days : days - 146096) / 146097;
	doe = (unsigned long) (days - era * 146097);		/* [0, 146096] */
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y = (long) yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);		/* [0, 365] */
	mp = (5 * doy + 2) / 153;				/* March = 0 */
	timep->tm_mday = (int) (doy - (153 * mp + 2) / 5 + 1);
	if (mp < 10) {
		timep->tm_mon = (int) mp + 2;
		timep->tm_yday = (int) doy + 59 + LEAPYEAR(y);
	} else {
		timep->tm_mon = (int) mp - 10;
		y++;
		timep->tm_yday = (int) doy - 306;
	}
	timep->tm_year = (int) (y - YEAR0);
	timep->tm_isdst = 0;

	return timep;
}
