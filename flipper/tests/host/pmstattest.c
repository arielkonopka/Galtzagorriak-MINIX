/* pmstattest - the Stop2 statistics of /dev/power (src/kernel/pmstat.h,
 * T14b, R6.8) on the PC: a night of timer wake-ups takes `wother' past
 * 65535 (#284: it was u16 and wrapped after about 18 h), the minutes
 * keep apart, and the record stays the 12 bytes it was (0 B of RAM). */
#include <stdio.h>
#include <string.h>
#include "stubs/kernel.h"		/* u16_t, u32_t; long is 32 bits */
#include "../../../src/kernel/pmstat.h"

static int fails, checks;

static void check(int ok, const char *what)
{
	checks++;
	if (!ok) {
		fails++;
		printf("FAIL %s\n", what);
	}
}

/* The digits as power_read (arm/flipdev.c) makes them from an unsigned
 * long, which is 32 bits on the Flipper (and here, stubs/kernel.h). */
static void fmt(char *out, unsigned long u)
{
	char d[12];
	int j = 0;

	do d[j++] = '0' + u % 10; while ((u /= 10) != 0);
	while (j > 0) *out++ = d[--j];
	*out = 0;
}

int main(void)
{
	struct pm_stat_s s;
	char buf[16];
	unsigned long i, n;
	const unsigned e = 58 * 32768 / 60;	/* a PM_MAX sleep, 0.967 s */

	check(sizeof(s) == 12, "pm_stat is 12 bytes (no RAM added)");
	check(sizeof(unsigned long) == 4, "unsigned long is 32 bits here");

	/* Started near the old limit, as a debug build would: across it. */
	memset(&s, 0, sizeof(s));
	s.other = 65530;
	for (i = 0; i < 10; i++) pm_stat_add(&s, e, 0);
	check(s.other == 65540, "wother counts past 65535");
	fmt(buf, s.other);
	check(strcmp(buf, "65540") == 0, "/dev/power prints wother=65540");

	/* The night of 2026-11-17/18 from boot: 70677 s in Stop2 in PM_MAX
	 * sleeps, 151 of them ended by a button.  The u16 read 11607. */
	memset(&s, 0, sizeof(s));
	n = 70677UL * 60 / 58 + 1;
	for (i = 0; i < n; i++) pm_stat_add(&s, e, i % 500 == 0);
	check(s.btn == (n + 499) / 500, "wbtn counted");
	check(s.btn + s.other == n, "every sleep is one wake-up");
	check(s.other > 65535, "a 20 h night does not wrap wother");
	check(pm_stat_secs(&s) >= 70670 && pm_stat_secs(&s) <= 70677,
		"stop2 seconds from minutes and clocks");
	check(s.stop < PM_LP_MIN, "the clocks stay under a minute");
	/* nightpower.py's rule holds without unwrapping: */
	check((s.btn + s.other) * 58.0 / 60 >= pm_stat_secs(&s),
		"wbtn + wother >= stop2 / 0.967");

	/* A week asleep (45 days fit the minutes; wother 10^6 per 11 d). */
	memset(&s, 0, sizeof(s));
	n = 7UL * 86400 * 60 / 58;
	for (i = 0; i < n; i++) pm_stat_add(&s, e, 0);
	check(s.other == n, "a week of wake-ups");
	check(pm_stat_secs(&s) / 3600 == 7 * 24 - 1 ||
		pm_stat_secs(&s) / 3600 == 7 * 24, "a week of Stop2");

	printf("pmstattest: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
