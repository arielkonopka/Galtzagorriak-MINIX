/* Host tests for jamdet and follow (task T35). */
#include <stdio.h>
#include "jamdet.h"
#include "follow.h"

static int checks, fails;
#define CHECK(c, m) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s\n", m); } else printf("ok %s\n", m); } while (0)

static void t_jam(void)
{
	struct jd_rule r = JD_DEFAULT;
	struct jd_state s;
	unsigned long t;
	int ev, n = 0;

	jd_reset(&s);
	for (t = 0; t < 10000; t += 50) n += jd_sample(&r, &s, t, -90);
	CHECK(n == 0, "quiet band");
	/* a legitimate 1 s burst */
	for (t = 10000; t < 11000; t += 50) n += jd_sample(&r, &s, t, -40);
	for (; t < 14000; t += 50) n += jd_sample(&r, &s, t, -90);
	CHECK(n == 0, "short burst, no alarm");
	for (ev = 0; ev == 0 && t < 30000; t += 50)
		ev = jd_sample(&r, &s, t, -40);
	CHECK(ev == JD_ALARM && t - 50 - 14000 >= 3000 &&
	      t - 50 - 14000 < 3100, "carrier for 3 s: alarm");
	CHECK(jd_sample(&r, &s, t, -40) == JD_NONE, "alarm only once");
	/* a 100 ms dip keeps the alarm and does not clear it */
	for (n = 0; n < 2; n++, t += 50) jd_sample(&r, &s, t, -90);
	for (n = 0; n < 40; n++, t += 50) ev |= jd_sample(&r, &s, t, -40);
	CHECK(s.alarm, "dip does not clear");
	for (ev = 0; ev == 0 && t < 60000; t += 50)
		ev = jd_sample(&r, &s, t, -90);
	CHECK(ev == JD_CLEAR && !s.alarm, "quiet for 2 s clears");
	/* 150 ms gaps in the carrier (a pulsed jammer) still count */
	jd_reset(&s);
	for (ev = 0, t = 0; ev == 0 && t < 20000; t += 50)
		ev = jd_sample(&r, &s, t, (t % 1000) < 150 ? -90 : -40);
	CHECK(ev == JD_ALARM, "pulsed carrier with short gaps");
	/* a sleeping caller */
	jd_reset(&s);
	jd_sample(&r, &s, 100, -40);
	CHECK(jd_sample(&r, &s, 60000, -40) == JD_NONE, "no alarm after a sleep");
}

static void t_follow(void)
{
	struct flw_rule r = FLW_DEFAULT;
	struct flw f;
	unsigned long t;
	int hit = 0, i;

	flw_init(&f);
	for (t = 0; t < 10UL * 60000; t += 61000) hit += flw_seen(&r, &f, 7, t);
	CHECK(hit == 0, "10 minutes: not yet");
	for (; t < 20UL * 60000; t += 61000) hit += flw_seen(&r, &f, 7, t);
	CHECK(hit == 1 && flw_flagged(&f) == 1, "20 minutes: flagged once");
	for (; t < 25UL * 60000; t += 61000) hit += flw_seen(&r, &f, 7, t);
	CHECK(hit == 1, "not flagged again");
	/* many sightings in one minute count as one */
	flw_init(&f);
	for (t = 0, hit = 0; t < 30UL * 60000; t += 1000)
		hit += flw_seen(&r, &f, 9, t % 1000 == 0 && t < 5000 ? t : 5000);
	CHECK(hit == 0, "a burst is one sighting");
	/* a passer-by */
	flw_init(&f);
	for (t = 0; t < 120000; t += 5000) hit += flw_seen(&r, &f, 11, t);
	CHECK(flw_flagged(&f) == 0, "two minutes: a passer-by");
	flw_expire(&r, &f, 120000 + 31UL * 60000);
	for (i = 0; i < FLW_MAX; i++) if (f.e[i].used) break;
	CHECK(i == FLW_MAX, "expired");
	/* a full table */
	flw_init(&f);
	for (i = 0; i < FLW_MAX + 5; i++) flw_seen(&r, &f, 100 + i, i * 10UL);
	CHECK(flw_seen(&r, &f, 500, 1000) == 0, "full table does not crash");
}

int main(void)
{
	t_jam();
	t_follow();
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
