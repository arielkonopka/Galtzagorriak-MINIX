/* wirefind.c - see wirefind.h. */
#include <string.h>
#include "wirefind.h"

/* 2 cos(2 pi hz / rate) in Q14 for rate 2000 */
int wf_mains_init(struct wf_mains *m, int hz, int rate)
{
	memset(m, 0, sizeof(*m));
	if (rate != 2000) return -1;
	if (hz == 50) m->coef = 32365;		/* 2 cos(0.15708 rad) * 16384 */
	else if (hz == 60) m->coef = 32188;	/* 2 cos(0.18850 rad) * 16384 */
	else return -1;
	return 0;
}

static unsigned long isqrt(unsigned long long x)
{
	unsigned long long r = 0, b = 1ULL << 62;

	while (b > x) b >>= 2;
	while (b) {
		if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
		else r >>= 1;
		b >>= 2;
	}
	return (unsigned long)r;
}

int wf_mains_put(struct wf_mains *m, int sample)
{
	long x, s0;
	long long p;
	unsigned long long pw;

	m->dc += (((long)sample << 8) - m->dc) >> 6;	/* x256, ~64 samples */
	x = sample - (m->dc >> 8);
	s0 = x + (long)(((long long)m->coef * m->s1) >> 14) - m->s2;
	m->s2 = m->s1;
	m->s1 = s0;
	if (++m->n < WF_BLOCK) return 0;
	/* |X|^2 = s1^2 + s2^2 - coef * s1 * s2 */
	p = (long long)m->s1 * m->s1 + (long long)m->s2 * m->s2 -
	    ((((long long)m->coef * m->s1) >> 14) * m->s2);
	pw = p > 0 ? (unsigned long long)p : 0;
	/* amplitude = 2 |X| / N */
	m->level = (unsigned long)((2 * isqrt(pw)) / WF_BLOCK);
	m->s1 = m->s2 = 0;
	m->n = 0;
	return 1;
}

int wf_pitch(unsigned long level, unsigned long floor, unsigned long full)
{
	int step = 0;
	unsigned long v;

	if (level <= floor || full <= floor) return 0;
	/* 0..16 half-octave steps from 200 Hz up to 4 kHz (~4.3 octaves) */
	for (v = floor; v < level && step < 16 && v < full; step++)
		v += (v + 1) / 3 + 1;
	return 200 + (3800 * step) / 16;
}

void wf_sens(int sens, unsigned long *floor, unsigned long *full)
{
	if (sens < 0) sens = 0;
	if (sens > 7) sens = 7;
	*floor = 64UL >> sens;
	if (*floor < 1) *floor = 1;
	*full = 4096UL >> sens;
	if (*full <= *floor) *full = *floor + 1;
}

void wf_detune_init(struct wf_detune *d) { memset(d, 0, sizeof(*d)); }

void wf_detune_put(struct wf_detune *d, int amp)
{
	long a = (long)amp << 4;

	d->cur = d->cur ? d->cur + ((a - d->cur) >> 2) : a;
}

void wf_detune_zero(struct wf_detune *d)
{
	d->base = d->cur;
	d->have = d->base != 0;
}

int wf_detune_delta(const struct wf_detune *d)
{
	if (!d->have) return 0;
	return (int)(((d->cur - d->base) * 1000) / d->base);
}

void wf_noise_init(struct wf_noise *n) { memset(n, 0, sizeof(*n)); }

void wf_noise_put(struct wf_noise *n, int i, int rssi)
{
	if (i < 0 || i >= WF_NF) return;
	if (rssi < -128) rssi = -128;
	if (rssi > 127) rssi = 127;
	/* smoothed: 3/4 old + 1/4 new, the first value as it comes */
	n->cur[i] = n->cur[i] ? (signed char)((3 * n->cur[i] + rssi) / 4)
			      : (signed char)rssi;
}

void wf_noise_zero(struct wf_noise *n)
{
	memcpy(n->base, n->cur, sizeof(n->base));
	n->have = 1;
}

int wf_noise_count(const struct wf_noise *n, int rise, int *mean)
{
	int i, c = 0, sum = 0;

	for (i = 0; n->have && i < WF_NF; i++)
		if (n->cur[i] - n->base[i] >= rise) {
			c++;
			sum += n->cur[i] - n->base[i];
		}
	if (mean) *mean = c ? sum / c : 0;
	return c;
}

int wf_verdict(int detune_pm, unsigned long mains_level, int noise_cnt,
	       int metal_pm, unsigned long mains_min, int noise_n)
{
	int v = 0;

	if (detune_pm >= metal_pm || -detune_pm >= metal_pm) v |= WF_METAL;
	if (mains_level >= mains_min) v |= WF_MAINS;
	if (noise_cnt >= noise_n) v |= WF_DATA;
	return v;
}
