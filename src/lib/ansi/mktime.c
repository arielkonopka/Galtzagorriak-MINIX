/*
 * mktime - convert local time into calendar time
 */
/* $Header: mktime.c,v 1.5 91/04/22 13:20:54 ceriel Exp $ */

/* Michael A. Temari <temari@ix.netcom.com>   03/01/96    */
/*   -  fixed bug is structure fixup code                 */

#include	<time.h>
#include	<limits.h>
#include	"loc_time.h"

/* Galtzagorriak (spec R2d.4): time_t is 64-bit and signed, so times before
 * 1970 and after 2038 work.  The day number comes from days-from-civil
 * (H. Hinnant, public domain), no year loop.  Years beyond about +-5 million
 * give (time_t) -1.
 */
#define	MAXYEAR	5000000L

time_t
mktime(register struct tm *timep)
{
	register long day, y, era;
	unsigned long yoe, doe, mp;
	register int tm_year;
	int yday, month;
	time_t seconds;
	unsigned dst;

	timep->tm_min += timep->tm_sec / 60;
	timep->tm_sec %= 60;
	if (timep->tm_sec < 0) {
		timep->tm_sec += 60;
		timep->tm_min--;
	}
	timep->tm_hour += timep->tm_min / 60;
	timep->tm_min = timep->tm_min % 60;
	if (timep->tm_min < 0) {
		timep->tm_min += 60;
		timep->tm_hour--;
	}
	day = timep->tm_hour / 24;
	timep->tm_hour= timep->tm_hour % 24;
	if (timep->tm_hour < 0) {
		timep->tm_hour += 24;
		day--;
	}
	timep->tm_year += timep->tm_mon / 12;
	timep->tm_mon %= 12;
	if (timep->tm_mon < 0) {
		timep->tm_mon += 12;
		timep->tm_year--;
	}
	day += (timep->tm_mday - 1);
	while (day < 0) {
		if(--timep->tm_mon < 0) {
			timep->tm_year--;
			timep->tm_mon = 11;
		}
		day += _ytab[LEAPYEAR(YEAR0 + timep->tm_year)][timep->tm_mon];
	}
	while (day >= _ytab[LEAPYEAR(YEAR0 + timep->tm_year)][timep->tm_mon]) {
		day -= _ytab[LEAPYEAR(YEAR0 + timep->tm_year)][timep->tm_mon];
		if (++(timep->tm_mon) == 12) {
			timep->tm_mon = 0;
			timep->tm_year++;
		}
	}
	timep->tm_mday = day + 1;
	_tzset();			/* set timezone and dst info  */
	if (timep->tm_year > MAXYEAR || timep->tm_year < -MAXYEAR)
		return (time_t)-1;

	/* days_from_civil, with the year starting on March 1 */
	y = (long) timep->tm_year + YEAR0 - (timep->tm_mon < 2);
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (unsigned long) (y - era * 400);			/* [0, 399] */
	mp = timep->tm_mon < 2 ? timep->tm_mon + 10 : timep->tm_mon - 2;
	doe = yoe * 365 + yoe / 4 - yoe / 100
		+ (153 * mp + 2) / 5 + timep->tm_mday - 1;	/* [0, 146096] */
	day = era * 146097 + (long) doe - 719468;	/* days since 1970 */

	tm_year = timep->tm_year + YEAR0;
	yday = month = 0;
	while (month < timep->tm_mon) {
		yday += _ytab[LEAPYEAR(tm_year)][month];
		month++;
	}
	yday += (timep->tm_mday - 1);
	timep->tm_yday = yday;
	timep->tm_wday = (int) ((day % 7 + 11) % 7);	/* day 0 was thursday */

	seconds = (time_t) day * SECS_DAY
		+ ((timep->tm_hour * 60L) + timep->tm_min) * 60L + timep->tm_sec;

	/* Now adjust according to timezone and daylight saving time */
	seconds += _timezone;

	if (timep->tm_isdst < 0)
		dst = _dstget(timep);
	else if (timep->tm_isdst)
		dst = _dst_off;
	else dst = 0;
	seconds -= dst;

	return seconds;
}
