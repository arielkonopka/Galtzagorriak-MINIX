/* specmath - see specmath.h */
#include "specmath.h"

/* The stock firmware's bands (furi_hal_subghz_is_frequency_valid), the
 * ones /dev/subghz takes: 300-348, 387-464 and 779-928 MHz. */
static const unsigned long bands[3][2] = {
	{ 300000000UL, 348000000UL },
	{ 387000000UL, 464000000UL },
	{ 779000000UL, 928000000UL },
};

/* Bayer 2x2: a pixel is black when its entry is below the level's count
 * of black pixels in the 2x2 cell (0, 1, 2, 4 of 4). */
static const unsigned char bayer[2][2] = { { 0, 2 }, { 3, 1 } };
static const unsigned char black[4] = { 0, 1, 2, 4 };

unsigned long sp_step(const struct sp_view *v)
{
	return v->span / SP_N;
}

unsigned long sp_freq(const struct sp_view *v, int i)
{
	unsigned long s = sp_step(v);

	return v->f0 + s * (unsigned long) i + s / 2;
}

int sp_x(int i)
{
	return i * (128 / SP_N);
}

int sp_in_band(unsigned long hz)
{
	int i;

	for (i = 0; i < 3; i++)
		if (hz >= bands[i][0] && hz <= bands[i][1]) return 1;
	return 0;
}

int sp_height(int dbm, int h)
{
	if (dbm == SP_NODATA || dbm <= SP_DBM_MIN) return 0;
	if (dbm >= SP_DBM_MAX) return h;
	return (dbm - SP_DBM_MIN) * h / (SP_DBM_MAX - SP_DBM_MIN);
}

int sp_level(int dbm)
{
	if (dbm == SP_NODATA || dbm < -90) return 0;
	if (dbm < -70) return 1;
	if (dbm < -50) return 2;
	return 3;
}

int sp_dither(int level, int x, int y)
{
	if (level < 0) level = 0;
	if (level > 3) level = 3;
	return bayer[y & 1][x & 1] < black[level];
}

int sp_hold(int hold, int peak)
{
	int n;

	if (hold == SP_NODATA) return peak;
	n = hold - SP_HOLD_DECAY;
	if (peak != SP_NODATA && peak > n) n = peak;
	if (n < SP_DBM_MIN) n = SP_DBM_MIN;
	return n;
}

static void clamp(struct sp_view *v)
{
	if (v->span < SP_SPAN_MIN) v->span = SP_SPAN_MIN;
	if (v->span > SP_SPAN_MAX) v->span = SP_SPAN_MAX;
	if (v->f0 < SP_F_LO) v->f0 = SP_F_LO;
	if (v->f0 + v->span > SP_F_HI) v->f0 = SP_F_HI - v->span;
}

void sp_center(struct sp_view *v, unsigned long fc, unsigned long span)
{
	v->span = span;
	if (v->span < SP_SPAN_MIN) v->span = SP_SPAN_MIN;
	if (v->span > SP_SPAN_MAX) v->span = SP_SPAN_MAX;
	v->f0 = fc > v->span / 2 ? fc - v->span / 2 : 0;
	clamp(v);
}

int sp_zoom(struct sp_view *v, int cur, int in)
{
	unsigned long fc = sp_freq(v, cur), ns, s, off;

	if (in) {
		if (v->span / 2 < SP_SPAN_MIN) return 0;
		ns = v->span / 2;
	} else {
		if (v->span >= SP_SPAN_MAX) return 0;
		ns = v->span > SP_SPAN_MAX / 2 ? SP_SPAN_MAX : v->span * 2;
	}
	s = ns / SP_N;
	off = s * (unsigned long) cur + s / 2;
	v->span = ns;
	v->f0 = fc > off ? fc - off : 0;
	clamp(v);
	return 1;
}

int sp_pan(struct sp_view *v, int dir)
{
	unsigned long d = sp_step(v) * (SP_N / 4), old = v->f0;

	if (dir < 0) v->f0 = v->f0 > d ? v->f0 - d : 0;
	else v->f0 += d;
	clamp(v);
	return v->f0 != old;
}

int sp_parse(const char *line, int n, int *peak)
{
	static const char key[] = "peak=";
	int i, j, neg = 0, v = 0, digits = 0;

	for (i = 0; i + 5 <= n; i++) {
		for (j = 0; j < 5 && line[i + j] == key[j]; j++) ;
		if (j == 5) break;
	}
	if (i + 5 > n) return 0;
	i += 5;
	if (i < n && line[i] == '-') { neg = 1; i++; }
	for (; i < n && line[i] >= '0' && line[i] <= '9' && digits < 4; i++) {
		v = v * 10 + (line[i] - '0');
		digits++;
	}
	if (digits == 0) return 0;
	*peak = neg ? -v : v;
	return 1;
}

/* v in decimal at p, at least `min` digits; returns the end. */
static char *put(char *p, unsigned long v, int min)
{
	char t[12];
	int n = 0;

	do { t[n++] = (char) ('0' + v % 10); v /= 10; } while (v != 0);
	while (n < min) t[n++] = '0';
	while (n > 0) *p++ = t[--n];
	return p;
}

char *sp_mhz(char *buf, unsigned long hz)
{
	unsigned long khz = (hz + 500) / 1000;
	char *p = put(buf, khz / 1000, 1);

	*p++ = '.';
	p = put(p, khz % 1000, 3);
	*p = 0;
	return buf;
}

char *sp_spanstr(char *buf, unsigned long span)
{
	char *p;

	if (span >= 100000000UL) {		/* "128M" */
		p = put(buf, span / 1000000UL, 1);
	} else if (span >= 10000000UL) {	/* "12.5M" */
		p = put(buf, span / 1000000UL, 1);
		*p++ = '.';
		p = put(p, span / 100000UL % 10, 1);
	} else if (span >= 1000000UL) {		/* "1.74M" */
		p = put(buf, span / 1000000UL, 1);
		*p++ = '.';
		p = put(p, span / 10000UL % 100, 2);
	} else {				/* "600k" */
		p = put(buf, span / 1000UL, 1);
		*p++ = 'k';
		*p = 0;
		return buf;
	}
	*p++ = 'M';
	*p = 0;
	return buf;
}
