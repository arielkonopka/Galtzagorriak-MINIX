/* Host tests for wirefind (the cores). */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "wirefind.h"

static int checks, fails;
#define CHECK(c, m) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s\n", m); } else printf("ok %s\n", m); } while (0)

/* A signal: amp * sin(2 pi f t) + offset + noise, rate 2000 */
static unsigned long run(struct wf_mains *m, double f, double amp, int off,
			 int noise, int blocks)
{
	int i, n = blocks * WF_BLOCK;
	unsigned long last = 0;

	for (i = 0; i < n; i++) {
		double v = off + amp * sin(2 * M_PI * f * i / 2000.0) +
			   (noise ? (rand() % (2 * noise + 1)) - noise : 0);
		if (wf_mains_put(m, (int)lround(v))) last = m->level;
	}
	return last;
}

static void t_mains(void)
{
	struct wf_mains m;
	unsigned long a, b, c, d;

	CHECK(wf_mains_init(&m, 50, 1000) < 0, "only 2 kHz");
	CHECK(wf_mains_init(&m, 40, 2000) < 0, "only 50/60");
	wf_mains_init(&m, 50, 2000);
	a = run(&m, 50, 100, 2048, 0, 6);
	CHECK(a > 90 && a < 110, "50 Hz amplitude 100");
	wf_mains_init(&m, 50, 2000);
	b = run(&m, 50, 20, 2048, 5, 6);
	CHECK(b > 15 && b < 25, "50 Hz amplitude 20 with noise");
	wf_mains_init(&m, 50, 2000);
	c = run(&m, 120, 100, 2048, 0, 6);
	CHECK(c < 10, "120 Hz is rejected");
	wf_mains_init(&m, 50, 2000);
	d = run(&m, 60, 100, 2048, 0, 6);
	CHECK(d < 25, "60 Hz at the 50 Hz filter");
	wf_mains_init(&m, 60, 2000);
	d = run(&m, 60, 100, 2048, 0, 6);
	CHECK(d > 90 && d < 110, "60 Hz filter on 60 Hz");
	wf_mains_init(&m, 50, 2000);
	a = run(&m, 1, 0, 3000, 3, 6);
	CHECK(a < 8, "silence with noise: low level");
}

static void t_pitch(void)
{
	int p0 = wf_pitch(10, 10, 1000), p1 = wf_pitch(30, 10, 1000);
	int p2 = wf_pitch(300, 10, 1000), p3 = wf_pitch(5000, 10, 1000);

	CHECK(p0 == 0, "below floor: silence");
	CHECK(p1 >= 200 && p1 < p2 && p2 < p3, "louder is higher");
	CHECK(p3 <= 4000, "at most 4 kHz");
}

static void t_sens(void)
{
	unsigned long f0, u0, f7, u7;

	wf_sens(0, &f0, &u0);
	wf_sens(7, &f7, &u7);
	CHECK(f0 == 64 && u0 == 4096 && f7 == 1 && u7 == 32, "sensitivity ends");
	wf_sens(-3, &f0, &u0);
	wf_sens(99, &f7, &u7);
	CHECK(f0 == 64 && f7 == 1, "sensitivity clamps");
	CHECK(wf_pitch(3, 64, 4096) == 0 && wf_pitch(3, 1, 32) > 0,
					"weak field is heard at 7 only");
}

static void t_detune(void)
{
	struct wf_detune d;
	int i;

	wf_detune_init(&d);
	for (i = 0; i < 20; i++) wf_detune_put(&d, 1000);
	CHECK(wf_detune_delta(&d) == 0, "no baseline: 0");
	wf_detune_zero(&d);
	CHECK(wf_detune_delta(&d) == 0, "baseline: 0");
	for (i = 0; i < 20; i++) wf_detune_put(&d, 900);
	CHECK(wf_detune_delta(&d) < -90 && wf_detune_delta(&d) > -110,
							"metal: -10 %");
	for (i = 0; i < 20; i++) wf_detune_put(&d, 1050);
	CHECK(wf_detune_delta(&d) > 40 && wf_detune_delta(&d) < 60, "+5 %");
	for (i = 0; i < 20; i++) wf_detune_put(&d, 1000);
	CHECK(abs(wf_detune_delta(&d)) < 10, "away again: back to 0");
}

static void t_noise(void)
{
	struct wf_noise n;
	int i, k, mean;

	wf_noise_init(&n);
	for (k = 0; k < 8; k++) for (i = 0; i < WF_NF; i++)
		wf_noise_put(&n, i, -95);
	CHECK(wf_noise_count(&n, 6, &mean) == 0, "no baseline: 0");
	wf_noise_zero(&n);
	for (k = 0; k < 8; k++) for (i = 0; i < WF_NF; i++)
		wf_noise_put(&n, i, -94);
	CHECK(wf_noise_count(&n, 6, &mean) == 0, "1 dB is no rise");
	for (k = 0; k < 12; k++) for (i = 0; i < 4; i++)
		wf_noise_put(&n, i, -80);
	CHECK(wf_noise_count(&n, 6, &mean) == 4 && mean >= 13 && mean <= 15,
							"4 frequencies +14 dB");
	wf_noise_put(&n, 99, 0);
	CHECK(1, "bad index ignored");
}

static void t_verdict(void)
{
	CHECK(wf_verdict(0, 3, 0, 50, 20, 3) == 0, "nothing");
	CHECK(wf_verdict(-80, 3, 0, 50, 20, 3) == WF_METAL, "metal");
	CHECK(wf_verdict(0, 40, 0, 50, 20, 3) == WF_MAINS, "mains");
	CHECK(wf_verdict(0, 3, 4, 50, 20, 3) == WF_DATA, "data cable");
	CHECK(wf_verdict(60, 40, 4, 50, 20, 3) == 7, "all three");
}

int main(void)
{
	t_mains();
	t_pitch();
	t_sens();
	t_detune();
	t_noise();
	t_verdict();
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
