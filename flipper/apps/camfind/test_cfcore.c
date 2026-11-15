/* Host unit tests of the camfind core (task T13): make test. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "cfcore.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int fails, checks;

#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

static unsigned long rnd = 12345;
static unsigned long next(void)
{
	rnd = rnd * 1103515245UL + 12345UL;
	return (rnd >> 16) & 0x7fff;
}

static int parse(const char *s, struct cf_line *l)
{
	return cf_parse(s, strlen(s), l);
}

static void test_parser(void)
{
	static const char *good[] = {
		"AP 00:bc:99:12:34:56 -48 6 IPC_9F3A",
		"AP 24:0a:c4:00:11:22 -71 11 ",	/* trailing space: empty SSID */
		"STA 24:0a:c4:00:11:22 -60 11 00:bc:99:12:34:56",
		"RATE 24:0a:c4:00:11:22 125000 310",
		"CH 6",
		"TRACK 24:0a:c4:00:11:22",
	};
	static const int type[] = { CF_AP, CF_AP, CF_STA, CF_RATE, CF_CH,
								CF_TRACK };
	static const char *bad[] = {
		"", "AP", "AP 00:bc:99:12:34:56", "AP 00:bc:99:12:34:5 -48 6 x",
		"AP 00:bc:99:12:34:56 -128 6 x", "AP 00:bc:99:12:34:56 5 6 x",
		"AP 00:bc:99:12:34:56 -48 0 x", "AP 00:bc:99:12:34:56 -48 197 x",
		"AP 00:bc:99:12:34:56 -48 6 0123456789abcdef0123456789abcdefX",
		"AP 0g:bc:99:12:34:56 -48 6 x", "STA 24:0a:c4:00:11:22 -60 11",
		"STA 24:0a:c4:00:11:22 -60 11 00:bc:99:12:34:56 junk",
		"RATE 24:0a:c4:00:11:22 -1 3", "RATE 24:0a:c4:00:11:22 99999999999 3",
		"CH 6 7", "CH x", "TRACK 24:0a:c4:00:11", "ap 00:bc:99:12:34:56 -48 6 x",
		"XAP 00:bc:99:12:34:56 -48 6 x",
	};
	struct cf_line l;
	char buf[128];
	unsigned i, k, n;

	for (i = 0; i < sizeof(good) / sizeof(good[0]); i++)
		CHECK(parse(good[i], &l) == type[i], "good line %u: %s", i, good[i]);
	parse(good[0], &l);
	CHECK(l.rssi == -48 && l.ch == 6 && strcmp(l.ssid, "IPC_9F3A") == 0
		&& l.mac[0] == 0x00 && l.mac[1] == 0xbc && l.mac[5] == 0x56,
		"AP fields");
	parse(good[1], &l);
	CHECK(l.ssid[0] == 0, "empty SSID");
	parse(good[2], &l);
	CHECK(l.bssid[1] == 0xbc && l.rssi == -60, "STA fields");
	parse(good[3], &l);
	CHECK(l.bytes == 125000 && l.frames == 310, "RATE fields");
	CHECK(cf_parse("CH 6\r\n", 6, &l) == CF_CH && l.ch == 6, "CRLF");
	CHECK(cf_parse("CH 6", 2, &l) == CF_NONE, "length counts, not NUL");

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		CHECK(parse(bad[i], &l) == CF_NONE, "bad line %u: %s", i, bad[i]);

	/* A UART that starts mid-line: every proper suffix of a good line
	 * (its start missing) is rejected, unless it happens to be a whole
	 * line itself. */
	for (i = 0; i < sizeof(good) / sizeof(good[0]); i++)
		for (k = 1; k < strlen(good[i]); k++)
			if (strncmp(good[i] + k, "CH ", 3) != 0)
				CHECK(parse(good[i] + k, &l) == CF_NONE,
					"suffix %u of line %u", k, i);
	/* Cut before the SSID: the numbers must be whole */
	CHECK(parse("STA 24:0a:c4:00:11:22 -6", &l) == CF_NONE, "cut STA");
	/* Garbage: random bytes never crash, and almost never parse */
	for (i = 0, n = 0; i < 20000; i++) {
		k = next() % sizeof(buf);
		for (unsigned j = 0; j < k; j++) buf[j] = (char) next();
		if (i % 3 == 0 && k > 3) memcpy(buf, "AP ", 3);
		if (cf_parse(buf, k, &l) != CF_NONE) n++;
	}
	CHECK(n == 0, "%u random lines parsed", n);
}

static void test_scoring(void)
{
	unsigned char mac[6] = { 0x00, 0xbc, 0x99, 1, 2, 3 };
	const unsigned char *e;
	struct cf_score s;
	int v, esp = -1, i, total = 0;

	for (i = 0; i < cf_nvendors; i++) {
		total += cf_vendors[i].count;
		if (strcmp(cf_vendors[i].name, "Espressif") == 0) esp = i;
		/* each vendor's OUIs sorted, so the binary search works */
		for (v = 1; v < cf_vendors[i].count; v++) {
			e = cf_oui + 3 * (cf_vendors[i].first + v);
			CHECK(memcmp(e - 3, e, 3) < 0, "%s sorted", cf_vendors[i].name);
		}
	}
	CHECK(total > 300 && total < 1000, "%d OUIs", total);
	CHECK(esp >= 0, "Espressif in the table");

	cf_score(mac, NULL, &s);
	CHECK(s.vendor >= 0 && strcmp(cf_vendors[s.vendor].name, "Hikvision") == 0
		&& s.level == CF_CAMERA, "Hikvision OUI: camera");
	for (i = 0; i < cf_nvendors; i++) {	/* every OUI is found */
		for (v = 0; v < cf_vendors[i].count; v++) {
			e = cf_oui + 3 * (cf_vendors[i].first + v);
			memcpy(mac, e, 3);
			CHECK(cf_vendor_of(mac) == i, "lookup %02x%02x%02x", e[0],
								e[1], e[2]);
		}
	}
	memcpy(mac, cf_oui + 3 * cf_vendors[esp].first, 3);
	cf_score(mac, "", &s);
	CHECK(s.level == CF_IOT, "Espressif alone: IoT module");
	cf_score(mac, "IPC_1234", &s);
	CHECK(s.level == CF_CAMERA, "Espressif + camera SSID: likely camera");
	mac[0] = 0x00; mac[1] = 0x11; mac[2] = 0x22;	/* not in the table */
	CHECK(cf_vendor_of(mac) == -1, "unknown OUI");
	cf_score(mac, "HomeNet", &s);
	CHECK(s.level == CF_OTHER, "plain AP: other");
	cf_score(mac, "ESP_A1B2C3", &s);
	CHECK(s.level == CF_IOT, "ESP setup SSID: IoT");
	cf_score(mac, "hd-cam-77", &s);
	CHECK(s.points == 2 && s.level == CF_IOT, "camera SSID alone: 2 points");
	mac[0] = 0x02;					/* locally administered */
	cf_score(mac, "IPCAM", &s);
	CHECK(s.level == CF_UNKNOWN, "random MAC: unknown");
	mac[0] = 0x0e;
	cf_score(mac, NULL, &s);
	CHECK(s.level == CF_UNKNOWN, "0x0e: locally administered too");
}

/* 250 ms rate samples: a camera streams more while something moves in
 * front of it; a phone's traffic does not care. */
static void trace(unsigned short *still, unsigned short *move, int camera)
{
	int i;
	unsigned long base;

	for (i = 0; i < CF_MWIN; i++) {
		base = 20000 + next() % 8000;		/* ~ 20-28 KB/s idle */
		still[i] = cf_rate_unit(camera ? base : base + (next() % 5 == 0) * 150000);
		base = camera ? 90000 + next() % 60000	/* motion: 4-6x */
			      : 20000 + next() % 8000 + (next() % 5 == 0) * 150000;
		move[i] = cf_rate_unit(base);
	}
}

static void test_motion(void)
{
	unsigned short still[CF_MWIN], move[CF_MWIN];
	struct cf_round r1, r2;
	int i, cams = 0, phones = 0;

	CHECK(cf_rate_unit(0) == 0 && cf_rate_unit(16) == 1
		&& cf_rate_unit(100000000UL) == 65535, "rate units");
	for (i = 0; i < 50; i++) {
		trace(still, move, 1);
		cf_motion_round(still, CF_MWIN, move, CF_MWIN, &r1);
		trace(still, move, 1);
		cf_motion_round(still, CF_MWIN, move, CF_MWIN, &r2);
		cams += cf_motion_watches(&r1, &r2);
		CHECK(r1.corr > 70, "camera correlation %d", r1.corr);
		trace(still, move, 0);
		cf_motion_round(still, CF_MWIN, move, CF_MWIN, &r1);
		trace(still, move, 0);
		cf_motion_round(still, CF_MWIN, move, CF_MWIN, &r2);
		phones += cf_motion_watches(&r1, &r2);
		CHECK(r1.corr < 50, "phone correlation %d", r1.corr);
	}
	CHECK(cams == 50, "camera traces: %d of 50 watch the room", cams);
	CHECK(phones == 0, "phone traces: %d of 50 watch the room", phones);
	for (i = 0; i < CF_MWIN; i++) still[i] = move[i] = 0;
	cf_motion_round(still, CF_MWIN, move, CF_MWIN, &r1);
	CHECK(!r1.pass && r1.corr == 0, "silence: no verdict");
}

static void test_locate(void)
{
	struct cf_locate l;
	int p, t, b, lp = 0, lb = -1, i;

	cf_locate_init(&l);
	for (i = -95; i <= -35; i += 5) {
		cf_locate_feed(&l, i);
		cf_locate_feed(&l, i);
		cf_locate_out(&l, &p, &t, &b);
		CHECK(p >= lp && b >= lb, "closer: higher (%d dBm)", i);
		lp = p; lb = b;
	}
	CHECK(p > 1500 && t < 300, "near: high and fast (%d Hz, %d ms)", p, t);
	cf_locate_mark(&l);
	cf_locate_out(&l, &p, &t, &b);
	CHECK(b == 50, "reference: the middle (%d)", b);
	for (i = 0; i < 20; i++) cf_locate_feed(&l, -55);
	cf_locate_out(&l, &p, &t, &b);
	CHECK(b < 50, "weaker than the reference: below (%d)", b);
}

static void wave(unsigned short *x, int n, double f, double amp, int pwm)
{
	int i;

	for (i = 0; i < n; i++) {
		double v = pwm ? (i % 2 ? amp : -amp)		/* 2 kHz at 4 kHz */
			       : amp * sin(2 * M_PI * f * i / 4000.0);
		x[i] = (unsigned short) (2000 + v + (double) (next() % 9) - 4);
	}
}

static void test_ir(void)
{
	unsigned short x[800];
	static const int f[4] = { 50, 60, 100, 120 };
	static const int k[4] = { CF_IR_50, CF_IR_60, CF_IR_100, CF_IR_120 };
	int i;

	for (i = 0; i < 4; i++) {
		wave(x, 400, f[i], 300, 0);
		CHECK(cf_ir_modulation(x, 400, 4000) == k[i], "%d Hz: %d", f[i],
					cf_ir_modulation(x, 400, 4000));
		wave(x, 800, f[i], 100, 0);		/* 5 %, longer */
		CHECK(cf_ir_modulation(x, 800, 4000) == k[i], "%d Hz weak", f[i]);
	}
	wave(x, 400, 0, 0, 0);
	CHECK(cf_ir_modulation(x, 400, 4000) == CF_IR_STEADY, "steady");
	wave(x, 400, 0, 500, 1);
	CHECK(cf_ir_modulation(x, 400, 4000) == CF_IR_PWM, "kHz PWM");
	CHECK(cf_ir_modulation(x, 100, 4000) == CF_IR_NONE, "too short");
	{	/* blocks: the state carries across DMA halves */
		struct cf_goertzel a, b;
		wave(x, 384, 50, 300, 0);
		cf_gz_init(&a, 50, 4000);
		cf_gz_feed(&a, x, 384, 2000);
		cf_gz_init(&b, 50, 4000);
		for (i = 0; i < 384; i += 64) cf_gz_feed(&b, x + i, 64, 2000);
		CHECK(cf_gz_power(&a) == cf_gz_power(&b) && a.n == 384,
							"blocks = one run");
	}
}

static void test_subghz(void)
{
	struct cf_burst b;
	int i, s;

	CHECK(cf_steps(cf_cell_bands, cf_ncell_bands) == 65, "cellular: 65 steps");
	CHECK(cf_step_khz(cf_cell_bands, cf_ncell_bands, 0) == 832000
		&& cf_step_khz(cf_cell_bands, cf_ncell_bands, 29) == 861000
		&& cf_step_khz(cf_cell_bands, cf_ncell_bands, 30) == 880000
		&& cf_step_khz(cf_cell_bands, cf_ncell_bands, 64) == 914000
		&& cf_step_khz(cf_cell_bands, cf_ncell_bands, 65) == -1,
		"cellular steps");
	s = cf_steps(cf_bands, cf_nbands);
	CHECK(s == 480 + 770 + 1490, "CC1101 steps %d", s);
	CHECK(cf_step_khz(cf_bands, cf_nbands, 480) == 387000, "second band");

	cf_burst_init(&b);
	for (i = 0; i < 400; i++)		/* noise, a burst every 40 */
		cf_burst_feed(&b, (i % 40) >= 20 && (i % 40) < 23 ? -55
						: -100 + (int) (next() % 5));
	CHECK(b.bursts == 10, "10 bursts, counted %u", b.bursts);
	CHECK(b.peak == -55, "peak hold %d", b.peak);
	CHECK(b.floor / 16 <= -95 && b.floor / 16 >= -101, "floor %d", b.floor / 16);
	cf_burst_init(&b);
	for (i = 0; i < 400; i++) cf_burst_feed(&b, -100 + (int) (next() % 6));
	CHECK(b.bursts == 0, "noise only: %u bursts", b.bursts);
	cf_burst_init(&b);
	for (i = 0; i < 400; i++) cf_burst_feed(&b, i < 50 ? -100 : -60);
	/* (a carrier is one burst; R9.1.3 lists it as a steady carrier) */
	CHECK(b.bursts == 1, "a carrier that stays: one burst, %u", b.bursts);
}

int main(void)
{
	test_parser();
	test_scoring();
	test_motion();
	test_locate();
	test_ir();
	test_subghz();
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
