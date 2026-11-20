/* modemtest - libmodem (src/lib/modem, spec section P) on the PC:
 * ACP.1 and ACP.2.
 *
 *   frames     CRC check value, pack/unpack, every 1- and 2-bit error caught
 *   arq        100 KB through a lossy half-duplex channel (two processes,
 *              a datagram socketpair): 20 % loss, 5 % corrupted, reordering
 *              and duplicates; byte-exact
 *   carousel   all frames arrive with 30 % loss
 *   p4         RFID line code: frames -> antenna segments -> synthetic
 *              comparator edges (jitter, skew, any start, both polarities)
 *              -> decoder, both presets
 *   cc1101     the presets' register values against the datasheet formulas'
 *              known results; 433.92 MHz FREQ
 *   legal      every preset at every 25 kHz from 300 to 928 MHz: allowed
 *              exactly when the occupied band lies inside a region's band
 *   duty       a week of random traffic: no 60-minute window over the limit
 *   fuzz       10^6 random and mutated frames and edge streams (ASan/UBSan)
 *   z          compression (-z): text, random and empty data round trip,
 *              through the ARQ stream too; random input to the decoder
 *
 * Run by `make test` here, by `make -C tests/host` (CI) and `make modemtest`.
 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mdmz.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

static unsigned long rnd_state = 12345;
static unsigned long rnd(void)
{
	/* xorshift32: the same sequence on every host */
	unsigned long x = rnd_state & 0xFFFFFFFFUL;

	x ^= (x << 13) & 0xFFFFFFFFUL;
	x ^= x >> 17;
	x ^= (x << 5) & 0xFFFFFFFFUL;
	rnd_state = x;
	return x;
}
static int chance(int percent) { return (int) (rnd() % 100) < percent; }

/* ---- frames ---- */
static void test_frames(void)
{
	unsigned char buf[MDM_MAXFRAME], pay[MDM_MAXPAY], m[MDM_MAXFRAME];
	struct mdm_frame f, g;
	int n, i, j, k, bad = 0;

	CHECK(mdm_crc16((const unsigned char *) "123456789", 9, 0xFFFF) == 0x29B1,
		"CRC-16/CCITT-FALSE check value");
	for (i = 0; i < MDM_MAXPAY; i++) pay[i] = (unsigned char) (i * 7 + 3);
	f.type = MDM_T_DATA; f.flags = MDM_F_TURN; f.sid = 0x5A;
	f.a = 200; f.b = 0; f.len = 24; f.pay = pay;
	n = mdm_pack(&f, buf, sizeof(buf));
	CHECK(n == 24 + MDM_OVH, "pack length %d", n);
	CHECK(mdm_unpack(buf, n, 24, &g) == 0 && g.type == f.type
		&& g.flags == f.flags && g.sid == f.sid && g.a == f.a
		&& g.len == 24 && memcmp(g.pay, pay, 24) == 0, "round trip");
	CHECK(mdm_unpack(buf, n, 23, &g) < 0, "len over the MTU accepted");
	CHECK(mdm_unpack(buf, n - 1, 24, &g) < 0, "short frame accepted");
	CHECK(mdm_unpack(buf, n + 1, 24, &g) < 0, "long frame accepted");
	f.len = 251;
	CHECK(mdm_pack(&f, buf, sizeof(buf)) < 0, "len 251 packed");
	f.len = 24;
	n = mdm_pack(&f, buf, sizeof(buf));
	for (i = 0; i < n * 8; i++)		/* every 1- and 2-bit error */
		for (j = i; j < n * 8; j++) {
			memcpy(m, buf, n);
			m[i / 8] ^= (unsigned char) (1 << (i % 8));
			if (j != i) m[j / 8] ^= (unsigned char) (1 << (j % 8));
			if (mdm_unpack(m, n, MDM_MAXPAY, &g) == 0) bad++;
		}
	CHECK(bad == 0, "%d bit errors not caught", bad);
	for (k = 0; k < 16; k++) {
		f.type = (unsigned char) k;
		n = mdm_pack(&f, buf, sizeof(buf));
		CHECK((mdm_unpack(buf, n, 24, &g) == 0)
			== (k >= MDM_T_DATA && k <= MDM_T_DGRAM), "type %d", k);
	}
}

/* ---- a lossy half-duplex channel between two processes ---- */
struct chan {
	int fd;
	int loss, corrupt, reorder, dup;	/* percent */
	unsigned char held[MDM_MAXFRAME];
	int nheld;
};

static int ch_send(void *ctx, const unsigned char *b, int n)
{
	struct chan *c = ctx;
	unsigned char t[MDM_MAXFRAME];

	if (chance(c->loss)) return 0;
	memcpy(t, b, n);
	if (chance(c->corrupt)) t[rnd() % n] ^= (unsigned char) (1 + rnd() % 255);
	if (c->nheld == 0 && chance(c->reorder)) {	/* goes after the next */
		memcpy(c->held, t, n);
		c->nheld = n;
		return 0;
	}
	if (send(c->fd, t, n, 0) != n) return -1;
	if (chance(c->dup)) (void) send(c->fd, t, n, 0);
	if (c->nheld) {
		(void) send(c->fd, c->held, c->nheld, 0);
		c->nheld = 0;
	}
	return 0;
}

static int ch_recv(void *ctx, unsigned char *b, int max, long ms)
{
	struct chan *c = ctx;
	struct pollfd p;
	ssize_t n;

	if (c->nheld) {			/* the turn: nothing stays held */
		(void) send(c->fd, c->held, c->nheld, 0);
		c->nheld = 0;
	}
	p.fd = c->fd;
	p.events = POLLIN;
	if (poll(&p, 1, (int) ms) <= 0) return 0;
	n = recv(c->fd, b, max, 0);
	return n < 0 ? -1 : (int) n;
}

static long ch_air(void *ctx, int n) { (void) ctx; (void) n; return 1; }

#define ARQ_BYTES	(100 * 1024)
static unsigned char src[ARQ_BYTES], dst[ARQ_BYTES + MDM_MAXPAY];
static long spos, dpos;

static int get_src(void *u, unsigned char *b, int max)
{
	int n = (int) (rnd() % max) + 1;	/* short reads, like a pipe */

	(void) u;
	if (n > ARQ_BYTES - spos) n = (int) (ARQ_BYTES - spos);
	memcpy(b, src + spos, n);
	spos += n;
	return n;
}

static int put_dst(void *u, const unsigned char *b, int n)
{
	(void) u;
	if (dpos + n > (long) sizeof(dst)) return -1;
	memcpy(dst + dpos, b, n);
	dpos += n;
	return 0;
}

static void test_arq(int mtu, int window)
{
	int sv[2], st, r;
	pid_t pid;
	struct chan c;
	struct mdm_phy p;
	struct mdm_stat s;
	unsigned char wbuf[MDM_WMAX * (MDM_MAXPAY + 2)];
	long i;

	for (i = 0; i < ARQ_BYTES; i++) src[i] = (unsigned char) (rnd() >> 7);
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) { perror("socketpair"); exit(1); }
	memset(&c, 0, sizeof(c));
	c.loss = 20; c.corrupt = 5; c.reorder = 5; c.dup = 3;
	p.ctx = &c; p.mtu = mtu; p.turn_ms = 3;
	p.send = ch_send; p.recv = ch_recv; p.airtime = ch_air;
	fflush(stdout);
	if ((pid = fork()) == 0) {		/* the receiver */
		close(sv[0]);
		c.fd = sv[1];
		rnd_state = 777;
		r = mdm_recv(&p, window, 5000, put_dst, NULL, wbuf, &s, NULL);
		if (r != MDM_OK) { printf("recv: %d\n", r); _exit(2); }
		if (dpos != ARQ_BYTES || memcmp(dst, src, ARQ_BYTES) != 0) {
			printf("recv: %ld bytes, data differ\n", dpos);
			_exit(3);
		}
		_exit(0);
	}
	close(sv[1]);
	c.fd = sv[0];
	spos = 0;
	r = mdm_send(&p, 0xA5, window, 0, get_src, NULL, wbuf, &s, NULL);
	CHECK(r == MDM_OK, "send mtu %d window %d: %d", mtu, window, r);
	CHECK(s.bytes == ARQ_BYTES, "acked %ld bytes", s.bytes);
	waitpid(pid, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "receiver status %d", st);
	printf("arq: mtu %d window %d: %ld frames, %ld again, %ld dropped\n",
		mtu, window, s.frames, s.resent, s.bad);
	close(sv[0]);
}

/* The receiver must give up when nobody sends, and the sender when nobody
 * answers. */
static void test_arq_lost(void)
{
	int sv[2];
	struct chan c;
	struct mdm_phy p;
	struct mdm_stat s;
	unsigned char wbuf[4 * (54 + 2)];

	socketpair(AF_UNIX, SOCK_DGRAM, 0, sv);
	memset(&c, 0, sizeof(c));
	c.fd = sv[0];
	p.ctx = &c; p.mtu = 54; p.turn_ms = 1;
	p.send = ch_send; p.recv = ch_recv; p.airtime = ch_air;
	CHECK(mdm_recv(&p, 4, 50, put_dst, NULL, wbuf, &s, NULL) == MDM_EIDLE,
		"idle receiver");
	spos = 0;
	CHECK(mdm_send(&p, 1, 4, 0, get_src, NULL, wbuf, &s, NULL) == MDM_ELOST,
		"sender without a receiver");
	close(sv[0]);
	close(sv[1]);
}

/* ---- carousel ---- */
static void test_carousel(void)
{
	static unsigned char data[3000], out[3000 + 24], map[MDM_CAR_MAX / 8];
	unsigned char fr[MDM_MAXFRAME];
	struct mdm_car car;
	struct mdm_frame f;
	int n, nf, i, rounds = 0, len, r;

	for (i = 0; i < (int) sizeof(data); i++) data[i] = (unsigned char) rnd();
	nf = (int) (sizeof(data) + MDM_RFID_MTU - 1) / MDM_RFID_MTU;
	mdm_car_init(&car, map);
	while (!mdm_car_done(&car) && rounds < 100) {
		for (i = 0; i < nf; i++) {
			len = i == nf - 1 ? (int) sizeof(data) - i * MDM_RFID_MTU
				: MDM_RFID_MTU;
			n = mdm_car_pack(0x33, i, i == nf - 1 ? MDM_F_LAST : 0,
				data + i * MDM_RFID_MTU, len, fr, sizeof(fr));
			if (chance(30)) continue;
			if (chance(2)) fr[rnd() % n] ^= 0x10;
			if (mdm_unpack(fr, n, MDM_RFID_MTU, &f) < 0) continue;
			r = mdm_car_take(&car, &f, MDM_RFID_MTU);
			CHECK(r >= 0, "carousel take %d", r);
			if (r == 1)
				memcpy(out + (f.a | f.b << 8) * MDM_RFID_MTU,
					f.pay, f.len);
		}
		rounds++;
	}
	CHECK(mdm_car_done(&car) && car.size == (long) sizeof(data)
		&& memcmp(out, data, sizeof(data)) == 0,
		"carousel: %d of %d after %d rounds", car.have, nf, rounds);
	printf("carousel: %d frames in %d rounds at 30 %% loss\n", nf, rounds);
	/* a frame of another sender, and one past the end, change nothing */
	n = mdm_car_pack(0x34, 1, 0, data, MDM_RFID_MTU, fr, sizeof(fr));
	mdm_unpack(fr, n, MDM_RFID_MTU, &f);
	CHECK(mdm_car_take(&car, &f, MDM_RFID_MTU) == 0, "other sid taken");
	n = mdm_car_pack(0x33, nf + 5, 0, data, MDM_RFID_MTU, fr, sizeof(fr));
	mdm_unpack(fr, n, MDM_RFID_MTU, &f);
	CHECK(mdm_car_take(&car, &f, MDM_RFID_MTU) < 0, "index past the end");
	/* a forged frame past the end before the LAST one cannot complete it */
	mdm_car_init(&car, map);
	n = mdm_car_pack(0x33, 0, 0, data, MDM_RFID_MTU, fr, sizeof(fr));
	mdm_unpack(fr, n, MDM_RFID_MTU, &f); mdm_car_take(&car, &f, MDM_RFID_MTU);
	n = mdm_car_pack(0x33, 9, 0, data, MDM_RFID_MTU, fr, sizeof(fr));
	mdm_unpack(fr, n, MDM_RFID_MTU, &f); mdm_car_take(&car, &f, MDM_RFID_MTU);
	n = mdm_car_pack(0x33, 2, MDM_F_LAST, data, 5, fr, sizeof(fr));
	mdm_unpack(fr, n, MDM_RFID_MTU, &f); mdm_car_take(&car, &f, MDM_RFID_MTU);
	CHECK(!mdm_car_done(&car), "frame 1 missing but done");
}

/* ---- P4 ---- */
struct got { unsigned char want[MDM_MAXFRAME]; int n, hits, other; };
static void on_frame(void *u, const unsigned char *b, int n)
{
	struct got *g = u;

	if (n == g->n && memcmp(b, g->want, n) == 0) g->hits++;
	else g->other++;
}

/* The reader's view of segments played round and round: the time of each
 * level, loaded and not, with the comparator's skew (one edge kind late),
 * jitter on every edge, starting anywhere, at most 512 edges. */
static int edges(const unsigned long *dur, const unsigned long *pulse, int ns,
	unsigned long *d, int max, int skew, int jit)
{
	int i, k = 0, start = (int) (rnd() % (2 * ns)), e = 0;
	long t, prev_err = 0, err;

	for (i = 0; k < max; i++) {
		int s = (i / 2) % ns;
		t = (long) (i % 2 == 0 ? pulse[s] : dur[s] + 1 - pulse[s])
			* MDM_P4_PERIOD_US;
		/* the loaded part ends `skew` late; the edge times jitter */
		t += i % 2 == 0 ? skew : -skew;
		err = (long) (rnd() % (2 * jit + 1)) - jit;
		t += err - prev_err;
		prev_err = err;
		if (e++ < start) continue;
		d[k++] = (unsigned long) (t < 1 ? 1 : t);
	}
	return k;
}

static void test_p4(int q)
{
	unsigned char fr[MDM_MAXFRAME], pay[MDM_RFID_MTU];
	unsigned long dur[512], pulse[512], d[512];
	struct mdm_frame f;
	struct got g;
	int t, n, ns, nd, i, ok = 0;

	for (t = 0; t < 400; t++) {
		for (i = 0; i < MDM_RFID_MTU; i++) pay[i] = (unsigned char) rnd();
		f.type = MDM_T_CAR; f.flags = 0; f.sid = (unsigned char) rnd();
		f.a = (unsigned char) t; f.b = 0;
		f.len = (unsigned char) (t % 5 == 0 ? rnd() % MDM_RFID_MTU
			: MDM_RFID_MTU);
		f.pay = pay;
		n = mdm_pack(&f, fr, sizeof(fr));
		ns = mdm_p4_encode(fr, n, q, dur, pulse, 512);
		CHECK(ns == 1 + 4 * n, "segments %d", ns);
		CHECK(ns <= 125, "a frame is over 125 cycles");
		nd = edges(dur, pulse, ns, d, 512, (int) (rnd() % 81) - 40, 15);
		memcpy(g.want, fr, n);
		g.n = n; g.hits = g.other = 0;
		mdm_p4_decode(d, nd, q, MDM_RFID_MTU, on_frame, &g);
		CHECK(g.other == 0, "q %d: a wrong frame decoded", q);
		if (g.hits > 0) ok++;
	}
	CHECK(ok == 400, "q %d: %d of 400 frames decoded from one capture", q, ok);
	printf("p4: Q=%d %d/400 frames, %.1f ms a frame, %.0f bit/s\n", q, ok,
		mdm_p4_us(31, q) / 1000.0, 31 * 8 * 1e6 / mdm_p4_us(31, q));
}

/* ---- CC1101 ---- */
static void test_cc1101(void)
{
	static const struct { const char *name; unsigned char m4, m3, dev; } want[] = {
		{ "narrow", 0xF5, 0x83, 0x15 },	/* 58 kHz, 1.2 k, 5.2 kHz */
		{ "slow",   0xE7, 0x83, 0x27 },	/* 68 kHz, 4.8 k, 11.9 kHz */
		{ "std",    0xCA, 0x83, 0x35 },	/* 102 kHz, 38.4 k, 20.6 kHz */
		{ "wide",   0x5B, 0xF8, 0x47 },	/* 325 kHz, 100 k, 47.6 kHz */
		{ "max",    0x1D, 0x3B, 0x62 },	/* 650 kHz, 250 k, 127 kHz */
	};
	struct mdm_cc1101 r;
	const struct mdm_preset *p;
	int i;

	for (i = 0; i < 5; i++) {
		p = mdm_preset(want[i].name);
		CHECK(p != NULL, "preset %s", want[i].name);
		if (!p) continue;
		CHECK(mdm_cc1101_regs(433920000UL, &p->phy, &r) == 0, "regs %s", p->name);
		CHECK(r.mdmcfg4 == want[i].m4 && r.mdmcfg3 == want[i].m3
			&& r.deviatn == want[i].dev,
			"%s: MDMCFG4 %02X MDMCFG3 %02X DEVIATN %02X", p->name,
			r.mdmcfg4, r.mdmcfg3, r.deviatn);
		CHECK(r.mdmcfg2 == 0x13, "%s MDMCFG2 %02X", p->name, r.mdmcfg2);
		printf("cc1101: %-6s %6lu Bd, dev %6lu Hz, filter %6lu Hz, "
			"occupies %6lu Hz\n", p->name, r.baud, r.dev, r.bw,
			mdm_occupied(&r));
	}
	CHECK(r.freq2 == 0x10 && r.freq1 == 0xB0 && r.freq0 == 0x71,
		"433.92 MHz: FREQ %02X %02X %02X", r.freq2, r.freq1, r.freq0);
	p = mdm_preset("std");
	CHECK(mdm_cc1101_regs(380000000UL, &p->phy, &r) < 0, "380 MHz accepted");
	CHECK(mdm_cc1101_regs(930000000UL, &p->phy, &r) < 0, "930 MHz accepted");
	CHECK(mdm_preset("nope") == NULL && mdm_preset("st") == NULL, "prefix");
	CHECK(mdm_sg_airtime(38400, 61) == 15, "airtime %ld",
		mdm_sg_airtime(38400, 61));
}

static void test_legal(void)
{
	static const char *regions[] = { "EU", "US", "JP" };
	const struct mdm_preset *p;
	const struct mdm_band *b, *x;
	struct mdm_cc1101 r;
	unsigned long f, half;
	int ri, rc, inside, n = 0, allowed = 0;

	for (ri = 0; ri < 3; ri++)
	for (p = mdm_presets; p->name[0]; p++)
	for (f = 300000000UL; f <= 928000000UL; f += 25000UL) {
		if (mdm_cc1101_regs(f, &p->phy, &r) < 0) continue;
		rc = mdm_sg_check(regions[ri], &r, &b);
		half = mdm_occupied(&r) / 2 + r.freq / 50000UL;
		inside = 0;
		for (x = mdm_bands; x->region[0]; x++)
			if (strcmp(x->region, regions[ri]) == 0
			    && r.freq - half > x->lo && r.freq + half < x->hi)
				inside = 1;
		CHECK((rc == MDM_SG_OK) == inside, "%s %s %lu Hz: %d",
			regions[ri], p->name, f, rc);
		if (rc == MDM_SG_OK) {
			allowed++;
			CHECK(b && r.freq - half > b->lo && r.freq + half < b->hi,
				"band");
		}
		if (ri == 2) CHECK(rc == MDM_SG_REGION, "JP");
		n++;
	}
	printf("legal: %d settings, %d allowed\n", n, allowed);
	p = mdm_preset("std");
	mdm_cc1101_regs(433920000UL, &p->phy, &r);
	CHECK(mdm_sg_check("EU", &r, &b) == MDM_SG_OK && b->duty_pm == 100,
		"EU 433.92 std");
	CHECK(mdm_sg_dbm(b) == 10, "433 MHz power %d", mdm_sg_dbm(b));
	CHECK(mdm_sg_check("US", &r, &b) == MDM_SG_BAND, "US 433.92");
	mdm_cc1101_regs(915000000UL, &p->phy, &r);
	CHECK(mdm_sg_check("US", &r, &b) == MDM_SG_OK && mdm_sg_dbm(b) == -6,
		"US 915");
	p = mdm_preset("max");
	mdm_cc1101_regs(868300000UL, &p->phy, &r);
	CHECK(mdm_sg_check("EU", &r, &b) == MDM_SG_OK, "EU 868.3 max");
	mdm_cc1101_regs(869850000UL, &p->phy, &r);
	CHECK(mdm_sg_check("EU", &r, &b) == MDM_SG_BAND, "EU 869.85 max");
}

/* ---- duty cycle ---- */
#define WEEK	(7 * 24 * 3600L)
static void test_duty(int duty_pm)
{
	static unsigned short air[WEEK];
	struct mdm_duty d;
	unsigned long now, sum = 0, limit = (unsigned long) duty_pm * 360UL,
		worst = 0, total = 0;
	unsigned a;
	long w, w0;
	int checked = 0;

	memset(air, 0, sizeof(air));
	mdm_duty_init(&d);
	for (now = 3600; now < WEEK; now++) {
		if (!chance(30)) continue;
		a = (unsigned) (1 + rnd() % 200);	/* 10 ms .. 2 s */
		w = mdm_duty_wait(&d, now, duty_pm, a);
		if (w == 0) {
			mdm_duty_add(&d, now, a);
			air[now] = (unsigned short) (air[now] + a);
			total += a;
		} else if (w > 0 && checked < 2000) {
			/* waiting that long is enough */
			struct mdm_duty e = d;
			w0 = mdm_duty_wait(&e, now + w, duty_pm, a);
			CHECK(w0 == 0, "waited %ld s, still %ld", w, w0);
			checked++;
		}
	}
	for (now = 0; now < WEEK; now++) {
		sum += air[now];
		if (now >= 3600) sum -= air[now - 3600];
		if (sum > worst) worst = sum;
	}
	CHECK(worst <= limit, "duty %d per mille: %lu > %lu", duty_pm, worst, limit);
	printf("duty: %d per mille: worst hour %lu of %lu (10 ms), average "
		"%.1f %%\n", duty_pm, worst, limit,
		total * 100.0 / ((WEEK - 3600) * 100.0));
	CHECK(mdm_duty_wait(&d, 0, 1000, 60000) == 0, "no limit");
	CHECK(mdm_duty_wait(&d, WEEK, 1, 400) == -1, "packet over the limit");
}

/* ---- fuzz (ACP.2) ---- */
static void nothing(void *u, const unsigned char *b, int n)
{
	struct mdm_frame f;

	(void) u;
	CHECK(mdm_unpack(b, n, MDM_RFID_MTU, &f) == 0, "decoder gave a bad frame");
}

static void test_fuzz(void)
{
	unsigned char b[MDM_MAXFRAME + 8], pay[MDM_MAXPAY];
	unsigned long d[512];
	struct mdm_frame f;
	struct mdm_car car;
	static unsigned char map[MDM_CAR_MAX / 8];
	long i;
	int n, k, j, ok = 0;

	mdm_car_init(&car, map);
	for (i = 0; i < 1000000L; i++) {
		if (i & 1) {			/* a valid frame, mutated */
			f.type = (unsigned char) (1 + rnd() % 4);
			f.flags = (unsigned char) rnd();
			f.sid = (unsigned char) rnd(); f.a = (unsigned char) rnd();
			f.b = (unsigned char) rnd();
			f.len = (unsigned char) (rnd() % (MDM_MAXPAY + 1));
			f.pay = pay;
			n = mdm_pack(&f, b, sizeof(b));
			for (k = (int) (rnd() % 3); k-- > 0; )
				b[rnd() % (n + 8)] ^= (unsigned char) rnd();
			n += (int) (rnd() % 5) - 2;
		} else {
			n = (int) (rnd() % sizeof(b));
			for (k = 0; k < n; k++) b[k] = (unsigned char) rnd();
		}
		if (n < 0) n = 0;
		if (mdm_unpack(b, n, (int) (rnd() % (MDM_MAXPAY + 1)), &f) == 0) {
			ok++;
			CHECK(f.pay + f.len <= b + n, "payload past the input");
			(void) mdm_car_take(&car, &f, MDM_RFID_MTU);
		}
		if (i % 1000 == 0) {		/* an edge stream */
			for (j = 0; j < 512; j++)
				d[j] = j % 7 ? rnd() % 1200 : rnd();
			mdm_p4_decode(d, 512, MDM_P4_FAST, MDM_RFID_MTU, nothing, NULL);
		}
	}
	printf("fuzz: 10^6 frames, %d accepted (valid ones untouched by the "
		"mutation)\n", ok);
}

/* ---- compression (spec P.3a) ---- */
struct mem { const unsigned char *p; long n, pos; unsigned char *o; long on, omax; };
static int mem_get(void *u, unsigned char *b, int max)
{
	struct mem *m = u;
	int n = (int) (rnd() % max) + 1;

	if (n > m->n - m->pos) n = (int) (m->n - m->pos);
	memcpy(b, m->p + m->pos, n);
	m->pos += n;
	return n;
}
static int mem_put(void *u, const unsigned char *b, int n)
{
	struct mem *m = u;

	if (m->on + n > m->omax) return -1;
	memcpy(m->o + m->on, b, n);
	m->on += n;
	return 0;
}

static long zround(const unsigned char *in, long n, unsigned char *out, long omax,
	long *zlen)
{
	static struct mdm_zsrc zs;
	static struct mdm_zsink zk;
	static unsigned char z[200000];
	struct mem a = { in, n, 0, 0, 0, 0 }, b = { 0, 0, 0, out, 0, 0 };
	int r, k;

	b.omax = omax;
	mdm_zsrc_init(&zs, mem_get, &a);
	*zlen = 0;
	while ((r = mdm_zget(&zs, z + *zlen, (int) (rnd() % 60) + 1)) > 0)
		*zlen += r;
	if (r < 0) return -1;
	mdm_zsink_init(&zk, mem_put, &b);
	for (k = 0; k < *zlen; k += r) {	/* in frame-sized pieces */
		r = (int) (rnd() % 24) + 1;
		if (r > *zlen - k) r = (int) (*zlen - k);
		if (mdm_zput(&zk, z + k, r) < 0) return -1;
	}
	if (mdm_zend(&zk) < 0) return -1;
	return b.on;
}

static void test_z(void)
{
	static unsigned char in[60000], out[70000];
	long n, zl, got, i;
	int w;
	static struct mdm_zsink zk;
	struct mem b = { 0, 0, 0, out, 0, sizeof(out) };

	/* text: the manual's words, repeated with numbers */
	for (n = 0; n + 80 < (long) sizeof(in); )
		n += sprintf((char *) in + n, "%05ld Kaixo, galtzagorriak! Datuak "
			"irratiz doaz, bit bat, bi bit.\n", n);
	got = zround(in, n, out, sizeof(out), &zl);
	CHECK(got == n && memcmp(in, out, n) == 0, "text round trip");
	printf("z: text %ld -> %ld bytes (%ld %%)\n", n, zl, zl * 100 / n);
	CHECK(zl < n / 2, "text compressed only to %ld of %ld", zl, n);
	for (i = 0; i < 20000; i++) in[i] = (unsigned char) rnd();
	got = zround(in, 20000, out, sizeof(out), &zl);
	CHECK(got == 20000 && memcmp(in, out, 20000) == 0, "random round trip");
	printf("z: random 20000 -> %ld bytes\n", zl);
	CHECK(zl <= 20000 + 20000 / 8 + 16, "random grew to %ld", zl);
	got = zround(in, 0, out, sizeof(out), &zl);
	CHECK(got == 0, "empty round trip %ld", got);
	for (w = 1; w < 300; w++) {		/* small sizes */
		got = zround(in, w, out, sizeof(out), &zl);
		CHECK(got == w && memcmp(in, out, w) == 0, "size %d", w);
	}
	/* hostile input: random bytes never fault and stay bounded */
	for (i = 0; i < 2000; i++) {
		unsigned char junk[300];
		int k, len = (int) (rnd() % sizeof(junk));

		for (k = 0; k < len; k++) junk[k] = (unsigned char) rnd();
		b.on = 0;
		mdm_zsink_init(&zk, mem_put, &b);
		(void) mdm_zput(&zk, junk, len);
		(void) mdm_zend(&zk);
		CHECK(b.on <= 10L * len + 16, "%d junk bytes gave %ld", len, b.on);
	}
}

/* The ARQ stream with -z: sender compresses, receiver decompresses. */
static struct mdm_zsrc azs;
static struct mdm_zsink azk;
static void test_arq_z(void)
{
	int sv[2], st, r;
	pid_t pid;
	struct chan c;
	struct mdm_phy p;
	struct mdm_stat s;
	unsigned char wbuf[4 * (54 + 2)];
	long i;

	for (i = 0; i < ARQ_BYTES; i++)	/* compressible: a few symbols */
		src[i] = (unsigned char) ("abcabd  \n"[rnd() % 9]);
	socketpair(AF_UNIX, SOCK_DGRAM, 0, sv);
	memset(&c, 0, sizeof(c));
	c.loss = 10;
	p.ctx = &c; p.mtu = 54; p.turn_ms = 3;
	p.send = ch_send; p.recv = ch_recv; p.airtime = ch_air;
	fflush(stdout);
	if ((pid = fork()) == 0) {
		close(sv[0]);
		c.fd = sv[1];
		dpos = 0;
		mdm_zsink_init(&azk, put_dst, NULL);
		r = mdm_recv(&p, 4, 5000, mdm_zput, &azk, wbuf, &s, NULL);
		if (r != MDM_OK || s.flags != MDM_F_Z || mdm_zend(&azk) < 0) _exit(2);
		_exit(dpos == ARQ_BYTES && memcmp(dst, src, ARQ_BYTES) == 0 ? 0 : 3);
	}
	close(sv[1]);
	c.fd = sv[0];
	spos = 0;
	mdm_zsrc_init(&azs, get_src, NULL);
	r = mdm_send(&p, 0x5A, 4, MDM_F_Z, mdm_zget, &azs, wbuf, &s, NULL);
	CHECK(r == MDM_OK, "send -z: %d", r);
	waitpid(pid, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "receiver -z status %d", st);
	printf("arq -z: %d KB in %ld frames (%ld B on the air)\n",
		ARQ_BYTES / 1024, s.frames, s.bytes);
	close(sv[0]);
}

/* ---- radio regions (spec section Q, issue #292) ---- */
#ifndef REGION_DIR
#define REGION_DIR "../../rootfs/usr/share/region"
#endif
static const char rg_codes[][3] = {
	"EU", "PL", "ES", "FR", "IE", "FI", "SE", "NO", "GB", "AL", "US", "CA",
	"AT", "BE", "BG", "HR", "CY", "CZ", "DK", "EE", "DE", "GR",
	"HU", "IT", "LV", "LT", "LU", "MT", "NL", "PT", "RO", "SK",
	"SI", "BA", "MK"
};

static int rg_bad(const char *text, int want_line)
{
	/* a file that must be refused, at want_line */
	struct mdm_region rg;
	char line[160], err[64];
	const char *p = text;
	int ln = 0, n;

	mdm_rg_init(&rg);
	while (*p) {
		for (n = 0; p[n] && p[n] != '\n'; n++) line[n] = p[n];
		line[n] = 0;
		p += n + (p[n] == '\n');
		ln++;
		if (mdm_rg_line(&rg, line, err, (int) sizeof(err)) < 0)
			return ln == want_line;
	}
	return 0;
}

static void test_region_files(void)
{
	static struct mdm_region rg;
	static const char hdr[] = "Filetype: Galtzagorriak region\nVersion: 1\n"
		"Region: XX\n";
	char t[512], err[96];
	unsigned i;
	int j, active;

	for (i = 0; i < sizeof(rg_codes) / sizeof(rg_codes[0]); i++) {
		CHECK(mdm_rg_load(REGION_DIR, rg_codes[i], &rg, err,
			(int) sizeof(err)) == 0, "%s: %s", rg_codes[i], err);
		CHECK(mdm_rg_bandset(rg_codes[i]) != NULL, "%s: no band set",
			rg_codes[i]);
		for (j = active = 0; j < rg.n; j++)
			if (!(rg.row[j].flags & MDM_RG_UNCHECKED)) active++;
		CHECK(active >= 5, "%s: %d active rows", rg_codes[i], active);
		printf("region %s (%s): %d rows, %d active\n", rg_codes[i],
			rg.name, rg.n, active);
	}
	/* PL = EU + 2 unchecked rows; FR = EU alone */
	mdm_rg_load(REGION_DIR, "EU", &rg, err, (int) sizeof(err));
	j = rg.n;
	mdm_rg_load(REGION_DIR, "PL", &rg, err, (int) sizeof(err));
	CHECK(rg.n == j + 2 && (rg.row[j].flags & MDM_RG_UNCHECKED)
		&& strcmp(rg.base, "EU") == 0, "PL rows %d", rg.n);
	mdm_rg_load(REGION_DIR, "FR", &rg, err, (int) sizeof(err));
	CHECK(rg.n == j, "FR rows %d", rg.n);
	/* unknown codes are refused (Q.6) */
	CHECK(mdm_rg_load(REGION_DIR, "XX", &rg, err, (int) sizeof(err)) < 0
		&& strstr(err, "no table for XX") != NULL, "XX: %s", err);
	CHECK(mdm_rg_load(REGION_DIR, "pl", &rg, err, (int) sizeof(err)) < 0,
		"lower case");
	CHECK(mdm_rg_bandset("XX") == NULL && mdm_rg_bandset("JP") == NULL,
		"unknown band set");
	/* ACQ.1: bad rows refused with their line */
#define BAD(row, why) do { snprintf(t, sizeof(t), "%s# c\n%s\n", hdr, row); \
	CHECK(rg_bad(t, 5), "not refused: %s", why); } while (0)
	snprintf(t, sizeof(t), "%s# c\n433.050\t434.790\t0\tnone\t0\tany\tok\n"
		"200.000\t201.000\t0\tnone\t0\tany\tx\n", hdr);
	CHECK(rg_bad(t, 6), "not refused: outside the CC1101 (line 6)");
	BAD("348.000\t390.000\t0\tnone\t0\tany\tx", "across two CC1101 ranges");
	BAD("433.050\t434.790\t11\tnone\t0\tany\tx", "dbm > 10");
	BAD("433.050\t434.790\t0\tnone\t0\tdata\tx", "unknown use");
	BAD("433.050\t434.790\t0\tnone\t0\tany", "no ref");
	BAD("433.050\t434.790\t0\tnone\t0\tany\t", "empty ref");
	BAD("434.790\t433.050\t0\tnone\t0\tany\tx", "lo > hi");
	BAD("433.050\t434.790\t0\t100%\t0\tany\tx", "duty 100 %");
	BAD("#?433.050\t434.790\t0\tnone\t0\tany", "unchecked, no ref");
	BAD("Filetype zzz", "no colon");
	snprintf(t, sizeof(t), "Filetype: Stock region\n");
	CHECK(rg_bad(t, 1), "wrong filetype");
}

/* the rows a transmit can use, for the messages of the tests */
static int rg_try(const struct mdm_region *rg, struct mdm_rgstate *st,
	double mhz, unsigned long occ, int dbm, int mode, unsigned air,
	unsigned long now, long *wait)
{
	int row;

	return mdm_rg_tx(rg, st, (unsigned long) (mhz * 1e6 + 0.5), occ, dbm,
		mode, air, now, &row, wait);
}

static void test_region_tx(void)
{
	static struct mdm_region rg;
	static struct mdm_rgstate st;
	const struct mdm_preset *std = mdm_preset("std");
	struct mdm_cc1101 c;
	unsigned long occ, half, now, sent;
	char err[96];
	unsigned i;
	long w;
	int us, rc, n;

	for (i = 0; i < sizeof(rg_codes) / sizeof(rg_codes[0]); i++) {
		const char *cc = rg_codes[i];

		us = strcmp(mdm_rg_bandset(cc), "US") == 0;
		CHECK(mdm_rg_load(REGION_DIR, cc, &rg, err, (int) sizeof(err))
			== 0, "%s: %s", cc, err);
		mdm_rg_state_init(&st);
		CHECK(mdm_cc1101_regs(433920000UL, &std->phy, &c) == 0, "regs");
		occ = mdm_occupied(&c);
		/* 1. a frequency outside every band: EPERM, with the message */
		rc = rg_try(&rg, &st, us ? 868.35 : 315.0, occ, -30, MDM_SG_CTL,
			10, 0, &w);
		CHECK(rc == MDM_SG_BAND && strstr(mdm_rg_msg(rc), "refused"),
			"%s: out of band %d", cc, rc);
		rc = rg_try(&rg, &st, 600.0, occ, -30, MDM_SG_CTL, 10, 0, &w);
		CHECK(rc == MDM_SG_BAND, "%s: 600 MHz %d", cc, rc);
		/* the 870/915 MHz rows are "#?": never used */
		if (!us) {
			rc = rg_try(&rg, &st, 871.5, occ, 0, MDM_SG_DATA, 10, 0, &w);
			CHECK(rc == MDM_SG_BAND, "%s: unchecked 871.5 MHz", cc);
		}
		/* 2. the band edge with the occupied bandwidth and 2 x 20 ppm:
		 * EU-style 433.05-434.79 (1 mW row), US/CA 902-928 (-2 dBm) */
		{
			double hi = us ? 928.0 : 434.79;
			unsigned long f = (unsigned long) (hi * 1e6 + 0.5);

			half = occ / 2 + f / 50000UL + 1;
			rc = mdm_rg_tx(&rg, &st, f - half - 1000, occ, us ? -2 : 0,
				MDM_SG_DATA, 1, 0, &n, &w);
			CHECK(rc == MDM_SG_OK, "%s: inside the edge %d", cc, rc);
			rc = mdm_rg_tx(&rg, &st, f - half + 1000, occ, us ? -2 : 0,
				MDM_SG_DATA, 1, 0, &n, &w);
			CHECK(rc == MDM_SG_BAND, "%s: over the edge %d", cc, rc);
			rc = mdm_rg_tx(&rg, &st, f - occ / 2 + 1000, occ, -30,
				MDM_SG_CTL, 1, 0, &n, &w);
			CHECK(rc == MDM_SG_BAND, "%s: crystal error ignored", cc);
		}
		/* 3. the duty cycle (RP.7) / the pause (RQ.6) refuses the next
		 * packet once its limit is reached */
		mdm_rg_state_init(&st);
		if (!us) {
			/* 433.92 MHz at +10 dBm: only the 10 % row admits it */
			sent = 0;
			for (now = 0; ; now++) {
				rc = rg_try(&rg, &st, 433.92, occ, 10, MDM_SG_DATA,
					100, now, &w);
				if (rc != MDM_SG_OK) break;
				sent += 100;
			}
			CHECK(rc == MDM_SG_WAIT && w > 0 && sent == 36000,
				"%s: 10 %% duty: %d after %lu x 10 ms (wait %ld)",
				cc, rc, sent, w);
			rc = rg_try(&rg, &st, 433.92, occ, 10, MDM_SG_DATA, 100,
				now + (unsigned long) w, &w);
			CHECK(rc == MDM_SG_OK, "%s: after the wait %d", cc, rc);
			/* at 0 dBm the 1 mW row has no limit: never waits */
			rc = rg_try(&rg, &st, 433.92, occ, 0, MDM_SG_DATA, 100,
				now, &w);
			CHECK(rc == MDM_SG_OK, "%s: 0 dBm waits", cc);
			/* 868.3 MHz +10 dBm: 1 % */
			mdm_rg_state_init(&st);
			for (now = sent = 0; ; now++) {
				rc = rg_try(&rg, &st, 868.3, occ, 10, MDM_SG_DATA,
					100, now, &w);
				if (rc != MDM_SG_OK) break;
				sent += 100;
			}
			CHECK(rc == MDM_SG_WAIT && sent == 3600,
				"%s: 1 %% duty at 868.3: %lu", cc, sent);
		} else {
			/* 433.92: data only under 15.231(e)/A.1.4, -30 dBm */
			rc = rg_try(&rg, &st, 433.92, occ, -15, MDM_SG_DATA, 50, 0, &w);
			CHECK(rc == MDM_SG_BAND, "%s: data at -15 dBm %d", cc, rc);
			rc = rg_try(&rg, &st, 433.92, occ, -30, MDM_SG_DATA, 101, 0, &w);
			CHECK(rc == MDM_SG_BAND, "%s: data burst > 1 s", cc);
			rc = rg_try(&rg, &st, 433.92, occ, -30, MDM_SG_DATA, 100, 0, &w);
			CHECK(rc == MDM_SG_OK, "%s: 1 s burst %d", cc, rc);
			rc = rg_try(&rg, &st, 433.92, occ, -30, MDM_SG_DATA, 100, 29, &w);
			CHECK(rc == MDM_SG_WAIT && w == 1, "%s: pause 30 s: %d %ld",
				cc, rc, w);
			rc = rg_try(&rg, &st, 433.92, occ, -30, MDM_SG_DATA, 100, 30, &w);
			CHECK(rc == MDM_SG_OK, "%s: after the pause %d", cc, rc);
			rc = rg_try(&rg, &st, 433.92, occ, -30, MDM_SG_DATA, 10, 31, &w);
			CHECK(rc == MDM_SG_WAIT && w == 29, "%s: >= 10 s after 1 s"
				" wait %ld", cc, w);
			/* a remote's code (ctl): -15 dBm, up to 5 s */
			rc = rg_try(&rg, &st, 433.92, occ, -15, MDM_SG_CTL, 500, 0, &w);
			CHECK(rc == MDM_SG_OK, "%s: ctl 5 s %d", cc, rc);
			rc = rg_try(&rg, &st, 433.92, occ, -15, MDM_SG_CTL, 501, 0, &w);
			CHECK(rc == MDM_SG_BAND, "%s: ctl over 5 s", cc);
			/* restricted band 322-335.4: nothing */
			rc = rg_try(&rg, &st, 330.0, occ, -30, MDM_SG_CTL, 10, 0, &w);
			CHECK(rc == MDM_SG_BAND, "%s: restricted band", cc);
		}
		/* +12 dBm, as the stock data allows: refused everywhere */
		rc = rg_try(&rg, &st, us ? 915.0 : 433.92, occ, 12, MDM_SG_CTL, 1,
			1000000, &w);
		CHECK(rc == MDM_SG_BAND, "%s: +12 dBm %d", cc, rc);
	}
	CHECK(mdm_pa_step(14) == 10 && mdm_pa_step(-2) == -6
		&& mdm_pa_step(-21) == -30 && mdm_pa_step(6) == 5, "PA steps");
}

int main(int argc, char **argv)
{
	(void) argc; (void) argv;
	signal(SIGPIPE, SIG_IGN);
	test_frames();
	test_arq(54, 4);
	test_arq(24, 1);
	test_arq(250, 8);
	test_arq_lost();
	test_carousel();
	test_p4(MDM_P4_FAST);
	test_p4(MDM_P4_ROBUST);
	test_cc1101();
	test_legal();
	test_duty(100);
	test_duty(10);
	test_duty(1);
	test_region_files();
	test_region_tx();
	test_fuzz();
	test_z();
	test_arq_z();
	if (fails) {
		printf("modemtest: %d FAILED\n", fails);
		return 1;
	}
	printf("modemtest: all passed\n");
	return 0;
}
