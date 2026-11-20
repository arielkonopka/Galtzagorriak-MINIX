/* Host tests for the canary cores (make test, task T35): beacon golden
 * vectors from the format specifications, malformed and truncated
 * payloads, a fuzz pass under the sanitizers, the deployment table; the
 * GPIO rules with bounce, windows and a wrapping clock. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "beacon.h"
#include "gpiorules.h"

static int fails, checks;

#define CHECK(c, what) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); } } while (0)

/* hex -> a malloc'd buffer of exactly that size (so ASan sees overruns) */
static unsigned char *unhex(const char *s, int *len)
{
	int n = (int)strlen(s) / 2, i;
	unsigned char *b = malloc(n ? n : 1);

	for (i = 0; i < n; i++) sscanf(s + 2 * i, "%2hhx", &b[i]);
	*len = n;
	return b;
}

static int parse(const char *hex, struct bc_frame *f)
{
	int n, r;
	unsigned char *b = unhex(hex, &n);

	r = bc_parse(b, n, f);
	free(b);
	return r;
}

#define FLAGS	"020106"
#define UUID	"e2c56db5dffb48d2b060d0f5a71096e0"
/* iBeacon: UUID above, major 1, minor 2, -59 dBm at 1 m */
#define IBEACON	FLAGS "1aff4c000215" UUID "00010002c5"
/* Eddystone: the 0xFEAA service list, then service data */
#define EDDY	FLAGS "0303aafe"
#define UID	EDDY "1716aafe00e7" "00010203040506070809" "0a0b0c0d0e0f" "0000"
#define UID18	EDDY "1516aafe00e7" "00010203040506070809" "0a0b0c0d0e0f"
#define URL	EDDY "0d16aafe10eb01" "676f6f676c65" "00"	/* google */
#define TLM	EDDY "1116aafe2000" "0bb8" "1880" "00000064" "00001234"
#define ETLM	EDDY "1516aafe2001" "000102030405060708090a0b" "cafe" "beef"
#define EID	EDDY "0d16aafe30e7" "0102030405060708"
/* AltBeacon, manufacturer 0x0118 (Radius Networks) */
#define ALTID	"2f234454cf6d4a0fadf2f4911ba9ffa6" "00010002"
#define ALT	FLAGS "1bff1801beac" ALTID "c5" "00"

static void t_formats(void)
{
	struct bc_frame f;
	char s[80];
	int i;
	static const unsigned char uuid[16] = { 0xe2, 0xc5, 0x6d, 0xb5,
		0xdf, 0xfb, 0x48, 0xd2, 0xb0, 0x60, 0xd0, 0xf5, 0xa7, 0x10,
		0x96, 0xe0 };

	CHECK(parse(IBEACON, &f) == BC_IBEACON, "ibeacon kind");
	CHECK(f.kind == BC_IBEACON && f.idlen == 20, "ibeacon frame");
	CHECK(memcmp(f.id, uuid, 16) == 0, "ibeacon uuid");
	CHECK(BC_MAJOR(&f) == 1 && BC_MINOR(&f) == 2, "ibeacon major minor");
	CHECK(f.tx == -59 && f.mfg == 0x004C, "ibeacon power");

	CHECK(parse(UID, &f) == BC_EDDY_UID, "uid kind");
	CHECK(f.tx == -25 && f.idlen == 16 && f.id[0] == 0 && f.id[9] == 9
		&& f.id[10] == 0x0a && f.id[15] == 0x0f, "uid fields");
	CHECK(parse(UID18, &f) == BC_EDDY_UID, "uid without RFU");

	CHECK(parse(URL, &f) == BC_EDDY_URL, "url kind");
	CHECK(f.tx == -21, "url power");
	CHECK(bc_url(&f, s, sizeof(s)) == 23
		&& strcmp(s, "https://www.google.com/") == 0, "url text");
	CHECK(bc_url(&f, s, 9) == 8 && strcmp(s, "https://") == 0,
							"url text cut");
	CHECK(parse(EDDY "0816aafe10eb03" "6869", &f) == BC_EDDY_URL
		&& bc_url(&f, s, sizeof(s)) == 10
		&& strcmp(s, "https://hi") == 0, "url https://");
	CHECK(parse(EDDY "0616aafe10eb02", &f) == BC_EDDY_URL
		&& bc_url(&f, s, sizeof(s)) == 7, "url empty");

	CHECK(parse(TLM, &f) == BC_EDDY_TLM, "tlm kind");
	CHECK(f.tlmver == 0 && f.vbatt == 3000 && f.temp == 0x1880
		&& f.advcnt == 100 && f.secs == 0x1234
		&& f.tx == BC_NOTX, "tlm fields");
	CHECK(parse(EDDY "1116aafe2000" "0000" "8000" "00000001" "00000002",
		&f) == BC_EDDY_TLM && f.vbatt == 0 && f.temp == BC_TEMP_NONE,
							"tlm no sensors");
	CHECK(parse(ETLM, &f) == BC_EDDY_TLM && f.tlmver == 1
		&& f.vbatt == 0 && f.temp == BC_TEMP_NONE, "encrypted tlm");
	CHECK(bc_url(&f, s, sizeof(s)) == -1 && s[0] == 0, "url of tlm");

	CHECK(parse(EID, &f) == BC_EDDY_EID, "eid kind");
	CHECK(f.idlen == 8 && f.id[0] == 1 && f.id[7] == 8 && f.tx == -25,
							"eid fields");

	CHECK(parse(ALT, &f) == BC_ALTBEACON, "altbeacon kind");
	CHECK(f.mfg == 0x0118 && f.tx == -59 && f.rsv == 0 && f.idlen == 20
		&& f.id[0] == 0x2f && f.id[19] == 0x02, "altbeacon fields");

	/* the beacon may come after other structures, and before padding */
	CHECK(parse("0909" "4d696e6978206761" IBEACON + 0, &f) == BC_IBEACON,
							"after a name");
	CHECK(parse(IBEACON "00" "ffff", &f) == BC_IBEACON, "padding after");
	CHECK(parse(UID TLM + 0, &f) == BC_EDDY_UID, "first beacon wins");

	/* not beacons */
	CHECK(parse("", &f) == BC_NONE, "empty");
	CHECK(parse(FLAGS, &f) == BC_NONE, "flags only");
	CHECK(parse("00", &f) == BC_NONE, "padding only");
	CHECK(parse(FLAGS "1bff4c001219" "00000000000000000000000000000000"
		"000000000000000000", &f) == BC_NONE, "apple find my");
	CHECK(parse(FLAGS "0aff4c001005031c1e8c4e", &f) == BC_NONE,
							"apple nearby");
	CHECK(parse(FLAGS "04ff4c0002", &f) == BC_NONE, "apple too short");
	CHECK(parse(EDDY "0616aafe40e700", &f) == BC_NONE,
						"unknown eddystone frame");
	CHECK(parse(FLAGS "0516aaff00e7", &f) == BC_NONE, "other service");
	CHECK(parse(FLAGS "0316aafe", &f) == BC_BAD, "eddystone no frame");
	CHECK(parse("0303aafe", &f) == BC_NONE, "service list only");

	/* malformed lengths */
	CHECK(parse(FLAGS "1aff4c000214" UUID "00010002c5", &f) == BC_BAD,
							"ibeacon inner 0x14");
	CHECK(f.kind == BC_NONE, "bad clears the frame");
	CHECK(parse(FLAGS "19ff4c000215" UUID "00010002", &f) == BC_BAD,
							"ibeacon short");
	CHECK(parse(FLAGS "1bff4c000215" UUID "00010002c5ff", &f) == BC_BAD,
							"ibeacon long");
	CHECK(parse(FLAGS "1aff1801beac" ALTID "c5", &f) == BC_BAD,
							"altbeacon short");
	CHECK(parse(EDDY "1616aafe00e7" "00010203040506070809" "0a0b0c0d0e0f"
		"00", &f) == BC_BAD, "uid 19");
	CHECK(parse(EDDY "0d16aafe10eb04" "676f6f676c65" "00", &f) == BC_BAD,
							"url scheme 4");
	CHECK(parse(EDDY "0d16aafe10eb01" "676f0e676c65" "00", &f) == BC_BAD,
							"url reserved 0x0e");
	CHECK(parse(EDDY "0d16aafe10eb01" "676f20676c65" "00", &f) == BC_BAD,
							"url space");
	CHECK(parse(EDDY "0d16aafe10eb01" "676f80676c65" "00", &f) == BC_BAD,
							"url 0x80");
	CHECK(parse(EDDY "1716aafe10eb01" "41414141414141414141414141414141"
		"4141", &f) == BC_BAD, "url 18 chars");
	CHECK(parse(EDDY "0516aafe10eb", &f) == BC_BAD, "url no scheme");
	CHECK(parse(EDDY "1016aafe2000" "0bb8" "1880" "00000064" "000012",
							&f) == BC_BAD, "tlm short");
	CHECK(parse(EDDY "0416aafe20", &f) == BC_BAD, "tlm no version");
	CHECK(parse(EDDY "0c16aafe30e7" "01020304050607", &f) == BC_BAD,
							"eid short");
	CHECK(parse(IBEACON "0aff", &f) == BC_BAD, "bad after a beacon");
	CHECK(parse("ff" IBEACON, &f) == BC_BAD, "first length 255");

	/* a truncated packet: every cut of every vector */
	{
		static const char *const v[] = { IBEACON, UID, URL, TLM, EID,
			ALT, ETLM };
		unsigned char *b;
		int n, k, r, bad = 0;

		for (i = 0; i < (int)(sizeof(v) / sizeof(v[0])); i++) {
			b = unhex(v[i], &n);
			for (k = 0; k < n; k++) {
				unsigned char *c = malloc(k ? k : 1);
				memcpy(c, b, k);
				r = bc_parse(c, k, &f);
				/* a cut inside a structure is BC_BAD; at a
				 * boundary it is what came before */
				if (r != BC_BAD && r != BC_NONE) bad++;
				free(c);
			}
			free(b);
		}
		CHECK(bad == 0, "truncated packets");
		CHECK(parse(IBEACON, &f) == BC_IBEACON
			&& bc_parse((const unsigned char *)"\x02\x01\x06", 2,
					&f) == BC_BAD, "cut flags");
	}
	CHECK(bc_parse(0, 3, &f) == BC_BAD, "null");
	CHECK(bc_parse((const unsigned char *)"", -1, &f) == BC_BAD,
							"negative length");
}

/* Random payloads under ASan/UBSan: no crash, a sane frame. */
static void t_fuzz(void)
{
	struct bc_frame f;
	unsigned char *b;
	unsigned long seed = 12345;
	char s[200];
	int i, k, n, r, odd = 0;
	static const unsigned char heads[][4] = {
		{ 0xff, 0x4c, 0x00, 0x02 }, { 0xff, 0x18, 0x01, 0xbe },
		{ 0x16, 0xaa, 0xfe, 0x00 }, { 0x16, 0xaa, 0xfe, 0x10 },
		{ 0x16, 0xaa, 0xfe, 0x20 }, { 0x16, 0xaa, 0xfe, 0x30 } };

	for (i = 0; i < 200000; i++) {
		n = (int)(((seed = seed * 1103515245 + 12345) >> 16) % 40);
		b = malloc(n ? n : 1);
		for (k = 0; k < n; k++)
			b[k] = (unsigned char)((seed = seed * 1103515245
							+ 12345) >> 16);
		if (n > 6 && (i & 1)) {	/* steer towards the parsers */
			b[0] = (unsigned char)(b[0] % (n - 1));
			memcpy(b + 1, heads[i % 6], 4);
			if (b[4] == 0xbe) b[5] = 0xac;
		}
		r = bc_parse(b, n, &f);
		if (r < BC_BAD || r > BC_ALTBEACON || f.idlen > 20
				|| (r >= 0 && f.kind != r)) odd++;
		if (r == BC_EDDY_URL && bc_url(&f, s, sizeof(s)) < 0) odd++;
		free(b);
	}
	CHECK(odd == 0, "fuzz");
}

static int obs(struct bc_groups *g, const char *hex, int a, int rssi)
{
	struct bc_frame f;
	unsigned char addr[6] = { 0xc0, 0, 0, 0, 0, 0 };
	int r = parse(hex, &f);

	addr[5] = (unsigned char)a;
	return bc_observe(g, r, &f, addr, rssi);
}

#define IB(min)	FLAGS "1aff4c000215" UUID "0001" min "c5"

static void t_groups(void)
{
	struct bc_dep dep[3];
	struct bc_bcn bcn[6];
	struct bc_groups g;
	struct bc_sum s;
	char t[80];
	int d;

	bc_init(&g, dep, 3, bcn, 6);
	bc_summary(&g, &s);
	CHECK(s.deps == 0 && s.beacons == 0 && s.anon == 0
		&& s.best == BC_NOTX, "empty summary");

	d = obs(&g, IB("0001"), 1, -70);
	CHECK(d == 0, "first deployment");
	CHECK(obs(&g, IB("0002"), 2, -58) == 0, "same uuid");
	CHECK(obs(&g, IB("0003"), 3, -80) == 0, "same uuid 3");
	CHECK(obs(&g, IB("0002"), 2, -90) == 0, "seen again");
	CHECK(obs(&g, IB("0002"), 9, -60) == 0, "new address, same beacon");
	bc_summary(&g, &s);
	CHECK(s.deps == 1 && s.beacons == 3 && s.best == -58,
				"1 deployment, 3 beacons, strongest -58");
	CHECK(dep[0].frames == 5, "frames");
	CHECK(bc_dep_text(&dep[0], t, sizeof(t)) == 36 && strcmp(t,
		"e2c56db5-dffb-48d2-b060-d0f5a71096e0") == 0, "uuid text");

	/* the URL host groups across schemes and paths, case folded */
	d = obs(&g, URL, 4, -65);
	CHECK(d == 1, "url deployment");
	CHECK(obs(&g, EDDY "0f16aafe10eb00" "476f4f676c65" "00" "6162", 5,
					-66) == 1, "http, caps, a path");
	CHECK(obs(&g, EDDY "0d16aafe10eb03" "676f6f676c65" "00", 6, -67) == 2,
						"no www: another host");
	CHECK(bc_dep_text(&dep[1], t, sizeof(t)) == 14
		&& strcmp(t, "www.google.com") == 0, "host text");
	CHECK(bc_dep_text(&dep[2], t, 5) == 4 && strcmp(t, "goog") == 0,
							"host text cut");

	/* TLM and EID are anonymous; others are counted only */
	CHECK(obs(&g, TLM, 7, -50) == BC_ANON, "tlm anon");
	CHECK(obs(&g, EID, 8, -55) == BC_ANON, "eid anon");
	CHECK(obs(&g, TLM, 7, -52) == BC_ANON, "tlm anon again");
	CHECK(obs(&g, FLAGS, 10, -40) == -1 && g.other == 1, "phone");
	CHECK(obs(&g, "0aff", 11, -40) == -1 && g.bad == 1, "bad");
	bc_summary(&g, &s);
	CHECK(s.deps == 3 && s.anon == 2 && g.anon == 3 && s.best == -50,
							"anonymous");

	/* full: the least recently seen deployment goes, with its beacons */
	CHECK(obs(&g, UID, 12, -75) == 0, "uid replaces the oldest");
	CHECK(dep[0].kind == BC_EDDY_UID && dep[0].beacons == 1,
							"replaced");
	CHECK(bc_dep_text(&dep[0], t, sizeof(t)) == 20
		&& strcmp(t, "00010203040506070809") == 0, "namespace text");
	bc_summary(&g, &s);
	CHECK(s.deps == 3 && s.beacons == 4, "iBeacons gone");
	CHECK(obs(&g, ALT, 13, -61) == 1, "altbeacon replaces url");
	CHECK(bc_dep_text(&dep[1], t, sizeof(t)) == 36 && strcmp(t,
		"2f234454-cf6d-4a0f-adf2-f4911ba9ffa6") == 0, "altbeacon text");

	/* beacon table full: the oldest beacon goes, its count follows */
	bc_init(&g, dep, 1, bcn, 2);
	obs(&g, IB("0001"), 1, -70);
	obs(&g, IB("0002"), 2, -70);
	obs(&g, IB("0001"), 1, -70);
	obs(&g, IB("0003"), 3, -70);	/* replaces minor 2 */
	bc_summary(&g, &s);
	CHECK(s.deps == 1 && s.beacons == 2 && dep[0].frames == 4,
							"beacon lru");
	CHECK(obs(&g, IB("0001"), 1, -70) == 0 && dep[0].beacons == 2,
							"minor 1 kept");
	CHECK(obs(&g, UID, 4, -70) == 0 && dep[0].beacons == 1, "dep lru");

	/* zero-size tables are safe */
	bc_init(&g, dep, 0, bcn, 0);
	CHECK(obs(&g, IB("0001"), 1, -70) == -1 && obs(&g, TLM, 1, -70)
						== BC_ANON, "no tables");
	bc_summary(&g, &s);
	CHECK(s.deps == 0 && s.anon == 0, "no tables summary");
	CHECK(obs(&g, IBEACON, 1, -500) == -1, "rssi clamped");
}

/* ---- GPIO ---- */

static int run(const struct gr_rule *r, struct gr_state *s,
					unsigned long t, long v)
{
	return gr_sample(r, s, t, v);
}

static void t_loop(void)
{
	struct gr_rule r;
	struct gr_state s;
	unsigned long t;
	int ev, n;

	memset(&r, 0, sizeof(r));
	r.kind = GR_LOOP;
	r.debounce = GR_DEBOUNCE;
	gr_reset(&s);
	for (t = 0; t < 100; t++)
		CHECK(run(&r, &s, t, 0) == GR_NONE, "loop closed");
	CHECK(s.stable == 0 && !s.alarm, "loop settled closed");
	/* bounce: open for 5 ms, closed 3, open 10: no alarm */
	for (n = 0, t = 100; t < 105; t++) n += run(&r, &s, t, 1);
	for (; t < 108; t++) n += run(&r, &s, t, 0);
	for (; t < 118; t++) n += run(&r, &s, t, 1);
	for (; t < 150; t++) n += run(&r, &s, t, 0);
	CHECK(n == 0 && !s.alarm, "loop bounce ignored");
	/* opened for good: the alarm comes 20 ms after the last edge */
	for (ev = GR_NONE, t = 150; ev == GR_NONE && t < 300; t++)
		ev = run(&r, &s, t, 1);
	CHECK(ev == GR_ALARM && t - 1 == 170, "loop alarm at +20 ms");
	CHECK(run(&r, &s, 171, 1) == GR_NONE, "loop alarm once");
	for (ev = GR_NONE, t = 200; ev == GR_NONE && t < 300; t++)
		ev = run(&r, &s, t, 0);
	CHECK(ev == GR_CLEAR && t - 1 == 220, "loop clear");

	/* latched: stays until gr_reset */
	r.latch = 1;
	gr_reset(&s);
	run(&r, &s, 0, 0);
	run(&r, &s, 30, 0);
	run(&r, &s, 40, 1);
	CHECK(run(&r, &s, 60, 1) == GR_ALARM, "latch alarm");
	run(&r, &s, 70, 0);
	CHECK(run(&r, &s, 100, 0) == GR_NONE && s.alarm, "latched");
	gr_reset(&s);
	run(&r, &s, 200, 0);
	CHECK(run(&r, &s, 230, 0) == GR_NONE && !s.alarm, "reset");

	/* a loop that is open from the start alarms after the debounce; a
	 * sparse sample stream works, and the clock may wrap */
	r.latch = 0;
	gr_reset(&s);
	t = (unsigned long)-10;
	CHECK(run(&r, &s, t, 1) == GR_NONE, "open at start");
	CHECK(run(&r, &s, t + 19, 1) == GR_NONE, "open 19 ms");
	CHECK(run(&r, &s, t + 20, 1) == GR_ALARM, "open 20 ms, wrapped");
	/* debounce 0 takes every level at once */
	r.debounce = 0;
	gr_reset(&s);
	CHECK(run(&r, &s, 5, 1) == GR_ALARM, "no debounce");
	CHECK(run(&r, &s, 5, 0) == GR_CLEAR, "no debounce clear");
}

static void t_contact(void)
{
	struct gr_rule r;
	struct gr_state s;
	int n;

	memset(&r, 0, sizeof(r));
	r.kind = GR_CONTACT;
	r.debounce = GR_DEBOUNCE;
	r.when = GR_ON_OPEN;
	gr_reset(&s);
	CHECK(run(&r, &s, 0, 1) == GR_NONE && run(&r, &s, 25, 1) == GR_NONE,
					"contact open baseline: no event");
	CHECK(s.stable == 1, "baseline open");
	run(&r, &s, 30, 0);
	CHECK(run(&r, &s, 50, 0) == GR_NONE, "closed: not watched");
	run(&r, &s, 60, 1);
	CHECK(run(&r, &s, 79, 1) == GR_NONE, "open 19 ms");
	CHECK(run(&r, &s, 80, 1) == GR_ALARM && s.count == 1, "opened");

	r.when = GR_ON_CLOSE;
	gr_reset(&s);
	run(&r, &s, 0, 1);
	run(&r, &s, 20, 1);
	run(&r, &s, 30, 0);
	CHECK(run(&r, &s, 50, 0) == GR_ALARM, "closed");
	run(&r, &s, 60, 1);
	CHECK(run(&r, &s, 80, 1) == GR_NONE, "opened: not watched");

	r.when = GR_ON_ANY;
	gr_reset(&s);
	run(&r, &s, 0, 0);
	run(&r, &s, 20, 0);
	for (n = 0; n < 10; n++) {	/* a bouncing button: 10 ms pulses */
		run(&r, &s, 100 + 20 * n, 1);
		run(&r, &s, 110 + 20 * n, 0);
	}
	CHECK(run(&r, &s, 400, 0) == GR_NONE && s.count == 0,
						"bouncing button ignored");
	run(&r, &s, 500, 1);
	CHECK(run(&r, &s, 520, 1) == GR_ALARM, "any: open");
	run(&r, &s, 600, 0);
	CHECK(run(&r, &s, 620, 0) == GR_ALARM && s.count == 2, "any: close");
}

static void t_pulse(void)
{
	struct gr_rule r;
	struct gr_state s;
	unsigned long t;
	int ev, a = 0, c = 0;

	memset(&r, 0, sizeof(r));
	r.kind = GR_PULSE;
	r.window = 1000;
	r.lo = 5;		/* a fan: at least 5 edges a second ... */
	r.hi = 50;		/* ... and at most 50 */
	gr_reset(&s);
	/* 20 edges a second, sampled every 100 ms (2 per sample) */
	for (t = 0; t < 5000; t += 100) {
		ev = run(&r, &s, t, 2);
		if (ev == GR_ALARM) a++;
		if (ev == GR_CLEAR) c++;
	}
	CHECK(a == 0 && c == 0 && s.rate == 20, "pulse in the window");
	/* the fan stops: the alarm at the end of the first empty window */
	for (ev = GR_NONE; ev == GR_NONE && t < 9000; t += 100)
		ev = run(&r, &s, t, 0);
	CHECK(ev == GR_ALARM && t - 100 == 6000 && s.rate == 0,
							"pulse stopped");
	for (ev = GR_NONE; ev == GR_NONE && t < 9000; t += 100)
		ev = run(&r, &s, t, 2);
	CHECK(ev == GR_CLEAR && t - 100 == 7000, "pulse back");
	/* a PIR that keeps firing: 60 a second */
	for (ev = GR_NONE; ev == GR_NONE && t < 9000; t += 100)
		ev = run(&r, &s, t, 6);
	CHECK(ev == GR_ALARM && s.rate == 60, "pulse too fast");
	for (; t <= 9000; t += 100) run(&r, &s, t, 2);
	CHECK(!s.alarm, "pulse normal");
	/* a gap of 3 s with no samples: empty windows */
	CHECK(run(&r, &s, 12000, 2) == GR_ALARM && s.alarm, "pulse gap");
	/* a burst then nothing inside one late sample: fired and cleared
	 * at once is still reported */
	r.lo = 0;
	r.hi = 10;
	gr_reset(&s);
	run(&r, &s, 20000, 0);
	CHECK(run(&r, &s, 23500, 30) == GR_ALARM && !s.alarm,
						"pulse transient kept");
	CHECK(s.win == 23000, "window phase kept");
	/* window 0 is taken as 1 ms; negative deltas are ignored */
	r.window = 0;
	gr_reset(&s);
	run(&r, &s, 0, -5);
	CHECK(run(&r, &s, 1, 0) == GR_NONE && s.rate == 0, "window 0");
}

static void t_volt(void)
{
	struct gr_rule r;
	struct gr_state s;
	unsigned long t;
	int ev, n;

	memset(&r, 0, sizeof(r));
	r.kind = GR_VOLT;
	r.debounce = GR_DEBOUNCE;
	r.lo = 4750;		/* a Raspberry Pi's 5 V */
	r.hi = 5250;
	r.hyst = 50;
	gr_reset(&s);
	for (n = 0, t = 0; t < 100; t++) n += run(&r, &s, t, 5050);
	CHECK(n == 0 && s.stable == 0, "volt ok");
	for (; t < 110; t++) n += run(&r, &s, t, 4500);	/* a 10 ms dip */
	for (; t < 200; t++) n += run(&r, &s, t, 5000);
	CHECK(n == 0 && !s.alarm, "volt dip filtered");
	for (ev = GR_NONE; ev == GR_NONE && t < 400; t++)
		ev = run(&r, &s, t, 4700);
	CHECK(ev == GR_ALARM && t - 1 == 220 && s.value == 4700,
							"volt dropped");
	for (n = 0; t < 400; t++) n += run(&r, &s, t, 4770);
	CHECK(n == 0 && s.alarm, "volt inside, not past the hysteresis");
	for (ev = GR_NONE; ev == GR_NONE && t < 600; t++)
		ev = run(&r, &s, t, 4900);
	CHECK(ev == GR_CLEAR && t - 1 == 420, "volt back");
	for (ev = GR_NONE; ev == GR_NONE && t < 800; t++)
		ev = run(&r, &s, t, 5300);
	CHECK(ev == GR_ALARM, "volt too high");
	CHECK(run(&r, &s, 900, 5230) == GR_NONE, "high side hysteresis");
	run(&r, &s, 901, 5100);
	CHECK(run(&r, &s, 921, 5100) == GR_CLEAR, "high side back");
	/* out of range from the first sample */
	gr_reset(&s);
	run(&r, &s, 0, 0);
	CHECK(run(&r, &s, 20, 0) == GR_ALARM, "volt low at start");
}

int main(void)
{
	t_formats();
	t_fuzz();
	t_groups();
	t_loop();
	t_contact();
	t_pulse();
	t_volt();
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
