/* pmstat.h - the Stop2 statistics since boot (T14b, R6.8), shown by
 * /dev/power and lat.  In a file of their own so that the PC test
 * flipper/tests/host/pmstattest.c checks the counting (#284).
 *
 * 12 bytes, as before #284: the u16 `other' wrapped after about 18 h (one
 * timer wake-up a second, PM_MAX); it took the 2 bytes of padding the old
 * layout (u32, u16, u16, u16) had and is 32 bits now (136 years).  `btn'
 * stays 16 bits: it needs a press for each wake-up, 65536 of them between
 * two readings is not a night (nightpower.py unwraps one wrap anyway). */
#ifndef PMSTAT_H
#define PMSTAT_H

struct pm_stat_s {
  u32_t stop;			/* time in Stop2, ms timer clocks (< 1 min) */
  u32_t other;			/* wake-ups by anything else (timer, alarm) */
  u16_t btn;			/* wake-ups by a button */
  u16_t min;			/* R6.8: and whole minutes of it (45 days) */
};

#define PM_LP_MIN	(32768UL * 60)	/* LPTIM1 clocks a minute */

/* One Stop2 sleep is over: e clocks of it, a button woke it or not. */
#define pm_stat_add(s, e, by_btn) do {					\
	(s)->stop += (e);						\
	if ((s)->stop >= PM_LP_MIN) {	/* whole minutes apart */	\
		(s)->stop -= PM_LP_MIN;					\
		(s)->min++;						\
	}								\
	if (by_btn) (s)->btn++; else (s)->other++;			\
  } while (0)

/* Seconds in Stop2 since boot. */
#define pm_stat_secs(s)	((unsigned long) (s)->min * 60 + (s)->stop / 32768)

#endif /* PMSTAT_H */
