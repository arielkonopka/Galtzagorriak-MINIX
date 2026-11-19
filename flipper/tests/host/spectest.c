/* spectest - the Spectrum app's arithmetic (flipper/gui/specmath.c, issue
 * #157): steps, the dBm scale, zoom and pan, the waterfall greys, the
 * /dev/subghz line. */
#include <stdio.h>
#include <string.h>
#include "../../gui/specmath.h"

static int fails;

#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, \
		__LINE__, #c); fails++; } } while (0)

int main(void)
{
	struct sp_view v;
	char b[16];
	int i, p, n;

	/* the EU SRD preset, 433.05-434.79 MHz in 32 steps of 54375 Hz */
	v.f0 = 433050000UL;
	v.span = 1740000UL;
	T(sp_step(&v) == 54375UL);
	T(sp_freq(&v, 0) == 433050000UL + 27187UL);
	T(sp_freq(&v, SP_N - 1) == 433050000UL + 31 * 54375UL + 27187UL);
	T(sp_freq(&v, SP_N - 1) < 434790000UL);
	for (i = 0; i < SP_N; i++) T(sp_in_band(sp_freq(&v, i)));
	T(sp_x(0) == 0 && sp_x(1) == 128 / SP_N && sp_x(SP_N - 1) + 128 / SP_N == 128);

	/* bands as stock */
	T(sp_in_band(300000000UL) && sp_in_band(348000000UL));
	T(!sp_in_band(299999999UL) && !sp_in_band(348000001UL));
	T(!sp_in_band(386999999UL) && sp_in_band(387000000UL) && sp_in_band(464000000UL));
	T(!sp_in_band(500000000UL) && sp_in_band(868300000UL) && !sp_in_band(928000001UL));

	/* -110..-30 dBm over the height */
	T(sp_height(-110, 64) == 0 && sp_height(-120, 64) == 0);
	T(sp_height(-30, 64) == 64 && sp_height(-10, 64) == 64);
	T(sp_height(-70, 64) == 32 && sp_height(-70, 47) == 23);
	T(sp_height(SP_NODATA, 64) == 0);
	for (i = -110, p = 0; i <= -30; i++) {		/* never goes down */
		T(sp_height(i, 47) >= p);
		p = sp_height(i, 47);
	}

	/* four greys, 0, 1, 2 and 4 black pixels of a 2x2 cell */
	T(sp_level(-100) == 0 && sp_level(-85) == 1 && sp_level(-60) == 2);
	T(sp_level(-41) == 3 && sp_level(SP_NODATA) == 0);
	for (i = 0; i < 4; i++) {
		static const int want[4] = { 0, 1, 2, 4 };
		int x, y;

		n = 0;
		for (y = 0; y < 2; y++)
			for (x = 0; x < 2; x++) n += sp_dither(i, x + 6, y + 10);
		T(n == want[i]);
	}

	/* peak hold: up at once, down SP_HOLD_DECAY dB a sweep */
	T(sp_hold(SP_NODATA, -60) == -60);
	T(sp_hold(-60, -90) == -60 - SP_HOLD_DECAY);
	T(sp_hold(-60, -40) == -40);
	T(sp_hold(-109, -120) == SP_DBM_MIN);
	T(sp_hold(-80, SP_NODATA) == -82);

	/* zoom in keeps the cursor's frequency (within one new step) */
	v.f0 = 433050000UL;
	v.span = 1740000UL;
	{
		unsigned long f = sp_freq(&v, 10), g;

		T(sp_zoom(&v, 10, 1) && v.span == 870000UL);
		g = sp_freq(&v, 10);
		T((g > f ? g - f : f - g) <= sp_step(&v));
		T(sp_zoom(&v, 10, 0) && v.span == 1740000UL);
	}
	for (i = 0; i < 20; i++) sp_zoom(&v, 5, 1);
	T(v.span >= SP_SPAN_MIN && v.span / 2 < SP_SPAN_MIN);
	T(!sp_zoom(&v, 5, 1));
	for (i = 0; i < 20; i++) sp_zoom(&v, 5, 0);
	T(v.span == SP_SPAN_MAX && !sp_zoom(&v, 5, 0));
	T(v.f0 >= SP_F_LO && v.f0 + v.span <= SP_F_HI);

	/* the edges of the whole range hold */
	sp_center(&v, 300100000UL, 2000000UL);
	T(v.f0 == SP_F_LO && v.span == 2000000UL);
	T(!sp_pan(&v, -1));
	T(sp_pan(&v, 1) && v.f0 == SP_F_LO + 8 * sp_step(&v));
	sp_center(&v, 927900000UL, 2000000UL);
	T(v.f0 + v.span == SP_F_HI && !sp_pan(&v, 1));
	sp_center(&v, 868300000UL, 600000UL);
	T(v.f0 == 868000000UL && v.span == 600000UL);
	sp_center(&v, 1000UL, 10UL);			/* nonsense in, sane out */
	T(v.f0 == SP_F_LO && v.span == SP_SPAN_MIN);

	/* the device's line */
	{
		static const char l1[] = "rssi=-92 peak=-41 freq=433919830\n";
		static const char l2[] = "off freq=0\n";
		static const char l3[] = "rssi=-92 peak=";

		p = 0;
		T(sp_parse(l1, (int) strlen(l1), &p) == 1 && p == -41);
		T(sp_parse(l2, (int) strlen(l2), &p) == 0);
		T(sp_parse(l3, (int) strlen(l3), &p) == 0);
		T(sp_parse(l1, 0, &p) == 0);
		T(sp_parse("peak=7", 6, &p) == 1 && p == 7);
		T(sp_parse("peak=-41", 6, &p) == 0);	/* cut before a digit */
	}

	/* the texts */
	T(strcmp(sp_mhz(b, 433919830UL), "433.920") == 0);
	T(strcmp(sp_mhz(b, 868000000UL), "868.000") == 0);
	T(strcmp(sp_mhz(b, 315000499UL), "315.000") == 0);
	T(strcmp(sp_spanstr(b, 1740000UL), "1.74M") == 0);
	T(strcmp(sp_spanstr(b, 600000UL), "600k") == 0);
	T(strcmp(sp_spanstr(b, 64000UL), "64k") == 0);
	T(strcmp(sp_spanstr(b, 12500000UL), "12.5M") == 0);
	T(strcmp(sp_spanstr(b, SP_SPAN_MAX), "128M") == 0);
	T(strlen(sp_spanstr(b, 99999999UL)) <= 6);

	if (fails) return 1;
	printf("spectest: all passed\n");
	return 0;
}
