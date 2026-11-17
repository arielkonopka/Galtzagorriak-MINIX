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

#include "modem.h"

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
	r = mdm_send(&p, 0xA5, window, get_src, NULL, wbuf, &s, NULL);
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
	CHECK(mdm_send(&p, 1, 4, get_src, NULL, wbuf, &s, NULL) == MDM_ELOST,
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
			n = mdm_car_pack(0x33, i, i == nf - 1,
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
	n = mdm_car_pack(0x33, 2, 1, data, 5, fr, sizeof(fr));
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
	test_fuzz();
	if (fails) {
		printf("modemtest: %d FAILED\n", fails);
		return 1;
	}
	printf("modemtest: all passed\n");
	return 0;
}
