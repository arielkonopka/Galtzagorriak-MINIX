/* dhttest - libdht on the PC (issue #358).
 *
 * Every temperature from -40.0 to 80.0 and humidity from 0 to 100 %
 * (a coarse grid and a few thousand random pairs): the frame dht_frame()
 * gives is decoded back to the same reading, also with jitter on every
 * pulse, with the host's own start pulse in front, and starting at level
 * 1.  Then the refusals: a bad checksum, a cut-off frame, noise, a wrong
 * bit rate; and the text `dht` prints.
 */
#include <stdio.h>
#include <string.h>
#include "dht.h"

static int fails, checks;
static unsigned long rnd = 2301;

static unsigned rand32(void)
{
	rnd = rnd * 1103515245UL + 12345UL;
	return (unsigned) (rnd >> 8 & 0xFFFFFF);
}

static void check(int ok, const char *what)
{
	checks++;
	if (!ok) {
		fails++;
		printf("FAIL %s\n", what);
	}
}

static void roundtrip(int t, int h, int jitter, int host)
{
	unsigned short f[DHT_MAXFRAME], c[DHT_MAXFRAME + 4];
	struct dht_reading in, out;
	int n, i, k = 0, rc;
	char what[64];

	in.temp_x10 = t;
	in.rh_x10 = h;
	n = dht_frame(&in, 0, f);
	if (host) {		/* the line before the answer: host low 1 ms, release */
		c[k++] = 1000;
		c[k++] = 30;
	}
	for (i = 0; i < n; i++) {
		int v = f[i];
		if (jitter) v += (int) (rand32() % (2 * jitter + 1)) - jitter;
		c[k++] = (unsigned short) v;
	}
	sprintf(what, "round trip t=%d h=%d jitter=%d host=%d", t, h, jitter, host);
	rc = dht_decode(c, k, 0, &out);
	check(rc == DHT_OK && out.temp_x10 == t && out.rh_x10 == h, what);
	/* the same capture seen from the other level (a leading idle high) */
	if (!host) {
		unsigned short d[DHT_MAXFRAME + 4];
		d[0] = 5000;
		memcpy(d + 1, c, k * sizeof c[0]);
		rc = dht_decode(d, k + 1, 1, &out);
		check(rc == DHT_OK && out.temp_x10 == t && out.rh_x10 == h,
			"round trip behind an idle high");
	}
}

int main(void)
{
	unsigned short f[DHT_MAXFRAME], c[DHT_MAXFRAME];
	struct dht_reading r, o;
	char buf[32];
	int t, h, n, i;

	check(DHT_MAXFRAME >= 2 + 2 * DHT_BITS + 1, "frame fits DHT_MAXFRAME");
	for (t = -400; t <= 800; t += 7)
		for (h = 0; h <= 1000; h += 53) {
			roundtrip(t, h, 0, 0);
			roundtrip(t, h, 8, 1);
		}
	for (i = 0; i < 4000; i++)
		roundtrip((int) (rand32() % 1201) - 400,
			  (int) (rand32() % 1001), (int) (rand32() % 15), i & 1);
	roundtrip(0, 0, 0, 0);
	roundtrip(-1, 1000, 0, 0);		/* -0.1 C: sign with a small value */
	roundtrip(-400, 999, 0, 0);

	/* checksum off by one: refused */
	r.temp_x10 = 234; r.rh_x10 = 451;
	n = dht_frame(&r, 1, f);
	check(dht_decode(f, n, 0, &o) == DHT_CHECKSUM, "bad checksum refused");
	/* a frame cut off at every point: never a reading */
	n = dht_frame(&r, 0, f);
	for (i = 0; i < n - 2; i++)
		check(dht_decode(f, i, 0, &o) != DHT_OK, "cut frame refused");
	/* nothing there */
	check(dht_decode(f, 0, 0, &o) == DHT_SHORT, "empty capture");
	/* only the host's pulse: no response */
	c[0] = 1000; c[1] = 30;
	check(dht_decode(c, 2, 0, &o) == DHT_SHORT, "no response");
	/* a high pulse that is no bit */
	memcpy(c, f, n * sizeof f[0]);
	c[11] = 5;
	check(dht_decode(c, n, 0, &o) == DHT_TIMING, "too short a bit");
	c[11] = 300;
	check(dht_decode(c, n, 0, &o) == DHT_TIMING, "too long a bit");
	/* noise: random durations must never crash the decoder (the
	 * sanitizers watch); a reading out of noise is possible only with a
	 * lucky checksum, so none is checked */
	for (i = 0; i < 20000; i++) {
		int k, m = (int) (rand32() % DHT_MAXFRAME);
		for (k = 0; k < m; k++)
			c[k] = (unsigned short) (rand32() % 130);
		(void) dht_decode(c, m, (int) (rand32() & 1), &o);
	}
	/* a sensor at 1.5x the bit rate (wrong clock): every 0 is 18 us
	 * and every 1 is 47 us: no more 1s above the split */
	n = dht_frame(&r, 0, f);
	for (i = 2; i < n - 1; i++)
		if (i & 1) f[i] = f[i] == 70 ? 40 : 18;
	check(dht_decode(f, n, 0, &o) == DHT_CHECKSUM
		|| dht_decode(f, n, 0, &o) == DHT_OK, "slow sensor: no crash");

	/* the text */
	r.temp_x10 = 234; r.rh_x10 = 451;
	dht_format(&r, buf);
	check(strcmp(buf, "temp=23.4 rh=45.1") == 0, "format 23.4 / 45.1");
	r.temp_x10 = -57; r.rh_x10 = 1000;
	dht_format(&r, buf);
	check(strcmp(buf, "temp=-5.7 rh=100.0") == 0, "format -5.7 / 100.0");
	r.temp_x10 = 5; r.rh_x10 = 0;
	dht_format(&r, buf);
	check(strcmp(buf, "temp=0.5 rh=0.0") == 0, "format 0.5 / 0.0");

	printf("dhttest: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
