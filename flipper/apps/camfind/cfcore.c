/* cfcore - the hidden camera finder's core (spec F.5, 9.1, task T13).
 * See cfcore.h.  No I/O, no malloc, no libm: it runs on the Flipper as
 * part of camfind and on the PC under test_cfcore.
 */
#include "cfcore.h"

/*===========================================================================*
 *				The line protocol			     *
 *===========================================================================*/
static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* Each reader takes s[*i..n) and advances *i; 0: not well formed. */
static int get_mac(const char *s, size_t n, size_t *i, unsigned char *m)
{
	int k, h, l;

	for (k = 0; k < 6; k++) {
		if (*i + 2 > n) return 0;
		if ((h = hexval(s[*i])) < 0 || (l = hexval(s[*i + 1])) < 0)
			return 0;
		m[k] = (unsigned char) (h << 4 | l);
		*i += 2;
		if (k < 5) {
			if (*i >= n || s[*i] != ':') return 0;
			(*i)++;
		}
	}
	return 1;
}

static int get_num(const char *s, size_t n, size_t *i, long lo, long hi,
								long *v)
{
	int neg = 0, digits = 0;
	long x = 0, lim = hi > -lo ? hi : -lo;

	if (*i < n && s[*i] == '-') { neg = 1; (*i)++; }
	while (*i < n && s[*i] >= '0' && s[*i] <= '9') {
		if (x > lim / 10 + 1) return 0;		/* too long */
		x = x * 10 + (s[*i] - '0');
		(*i)++;
		digits++;
	}
	if (digits == 0) return 0;
	if (neg) x = -x;
	if (x < lo || x > hi) return 0;
	*v = x;
	return 1;
}

static int get_sp(const char *s, size_t n, size_t *i)	/* one space */
{
	if (*i >= n || s[*i] != ' ') return 0;
	(*i)++;
	return 1;
}

static int word(const char *s, size_t n, size_t *i, const char *w)
{
	size_t k = 0;

	while (w[k] != 0) {
		if (*i + k >= n || s[*i + k] != w[k]) return 0;
		k++;
	}
	*i += k;
	return 1;
}

int cf_parse(const char *s, size_t n, struct cf_line *l)
{
	size_t i = 0, k;
	long v, w;

	while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n')) n--;
	l->type = CF_NONE;
	l->ssid[0] = 0;
	if (word(s, n, &i, "AP ")) {
		if (!get_mac(s, n, &i, l->mac) || !get_sp(s, n, &i)
		    || !get_num(s, n, &i, -127, 0, &v) || !get_sp(s, n, &i)
		    || !get_num(s, n, &i, 1, 196, &w)) return CF_NONE;
		l->rssi = (int) v;
		l->ch = (int) w;
		if (i < n) {				/* the SSID, if any */
			if (!get_sp(s, n, &i) || n - i > CF_SSID_MAX)
				return CF_NONE;
			for (k = 0; i < n; i++, k++) {
				if (s[i] == 0) return CF_NONE;
				l->ssid[k] = s[i];
			}
			l->ssid[k] = 0;
		}
		return l->type = CF_AP;
	}
	if (word(s, n, &i, "STA ")) {
		if (!get_mac(s, n, &i, l->mac) || !get_sp(s, n, &i)
		    || !get_num(s, n, &i, -127, 0, &v) || !get_sp(s, n, &i)
		    || !get_num(s, n, &i, 1, 196, &w) || !get_sp(s, n, &i)
		    || !get_mac(s, n, &i, l->bssid) || i != n) return CF_NONE;
		l->rssi = (int) v;
		l->ch = (int) w;
		return l->type = CF_STA;
	}
	if (word(s, n, &i, "RATE ")) {
		if (!get_mac(s, n, &i, l->mac) || !get_sp(s, n, &i)
		    || !get_num(s, n, &i, 0, 100000000L, &v) || !get_sp(s, n, &i)
		    || !get_num(s, n, &i, 0, 1000000L, &w) || i != n)
			return CF_NONE;
		l->bytes = (unsigned long) v;
		l->frames = (unsigned long) w;
		return l->type = CF_RATE;
	}
	if (word(s, n, &i, "CH ")) {
		if (!get_num(s, n, &i, 1, 196, &v) || i != n) return CF_NONE;
		l->ch = (int) v;
		return l->type = CF_CH;
	}
	if (word(s, n, &i, "TRACK ")) {
		if (!get_mac(s, n, &i, l->mac) || i != n) return CF_NONE;
		return l->type = CF_TRACK;
	}
	return CF_NONE;
}

/*===========================================================================*
 *				Scoring					     *
 *===========================================================================*/
int cf_vendor_of(const unsigned char mac[6])
{
	unsigned long key = (unsigned long) mac[0] << 16 | mac[1] << 8 | mac[2];
	const unsigned char *e;
	unsigned long o;
	int v, lo, hi, mid;

	for (v = 0; v < cf_nvendors; v++) {	/* each vendor's list is sorted */
		lo = cf_vendors[v].first;
		hi = lo + cf_vendors[v].count - 1;
		while (lo <= hi) {
			mid = (lo + hi) / 2;
			e = cf_oui + 3 * mid;
			o = (unsigned long) e[0] << 16 | e[1] << 8 | e[2];
			if (o == key) return v;
			if (o < key) lo = mid + 1; else hi = mid - 1;
		}
	}
	return -1;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int has(const char *s, const char *pat)	/* case-insensitive */
{
	const char *a, *b;

	for (; *s != 0; s++) {
		for (a = s, b = pat; *a != 0 && *b != 0
				&& lower((unsigned char) *a) == *b; a++, b++) ;
		if (*b == 0) return 1;
	}
	return 0;
}

/* SSIDs of cameras and of their setup modes (lower case). */
static const char *const cam_ssid[] = { "ipc", "cam", "hd-", "ezviz", "imou",
	"v380", "icsee", "xmeye", "yi_", "wyze", "reolink", "hikvision",
	"dahua", "jooan", "sricam", "wansview", "tapo_c", 0 };
static const char *const iot_ssid[] = { "esp_", "esp-", "esp32", "tuya",
	"smartlife", "smart life", "sl-", 0 };

void cf_score(const unsigned char mac[6], const char *ssid,
						struct cf_score *out)
{
	int i, p = 0;

	out->vendor = cf_vendor_of(mac);
	if (out->vendor >= 0)
		p += cf_vendors[out->vendor].kind == CF_CAMERA ? 3 : 1;
	if (ssid != 0 && *ssid != 0) {
		for (i = 0; cam_ssid[i] != 0; i++)
			if (has(ssid, cam_ssid[i])) { p += 2; break; }
		if (cam_ssid[i] == 0)
			for (i = 0; iot_ssid[i] != 0; i++)
				if (has(ssid, iot_ssid[i])) { p += 1; break; }
	}
	out->points = p;
	if (mac[0] & 0x02)			/* locally administered: random */
		out->level = CF_UNKNOWN;
	else
		out->level = p >= 3 ? CF_CAMERA : p >= 1 ? CF_IOT : CF_OTHER;
}

/*===========================================================================*
 *				Motion correlation			     *
 *===========================================================================*/
unsigned short cf_rate_unit(unsigned long bytes_per_s)
{
	bytes_per_s >>= 4;			/* 16-byte units: up to 1 MB/s */
	return (unsigned short) (bytes_per_s > 65535UL ? 65535UL : bytes_per_s);
}

static unsigned median(const unsigned short *x, int n)
{
	unsigned short a[CF_MWIN];
	int i, j;
	unsigned short t;

	if (n <= 0) return 0;
	if (n > CF_MWIN) n = CF_MWIN;
	for (i = 0; i < n; i++) {		/* insertion sort, n <= 40 */
		t = x[i];
		for (j = i; j > 0 && a[j - 1] > t; j--) a[j] = a[j - 1];
		a[j] = t;
	}
	return n % 2 ? a[n / 2] : (a[n / 2 - 1] + a[n / 2] + 1) / 2;
}

static unsigned long isqrt(unsigned long long v)
{
	unsigned long long r = 0, b = 1ULL << 62;

	while (b > v) b >>= 2;
	while (b != 0) {
		if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
		else r >>= 1;
		b >>= 2;
	}
	return (unsigned long) r;
}

void cf_motion_round(const unsigned short *still, int ns,
		const unsigned short *move, int nm, struct cf_round *r)
{
	unsigned ms = median(still, ns), mm = median(move, nm);
	long long sum = 0, sum2 = 0, n, sm = 0, var, num;
	int i;

	r->ratio = (int) ((mm * 100UL + (ms ? ms : 1) / 2) / (ms ? ms : 1));
	if (ms == 0 && mm == 0) r->ratio = 100;
	if (ms == 0 && mm > 0 && r->ratio < 200) r->ratio = 200;
	r->pass = r->ratio >= 200;

	/* point-biserial correlation of rate and window (still 0, move 1):
	 * (mean_move - mean_still) sqrt(ns nm) / (n sd), sd over all */
	n = ns + nm;
	for (i = 0; i < ns; i++) { sum += still[i]; sum2 += (long long) still[i] * still[i]; }
	for (i = 0; i < nm; i++) {
		sum += move[i]; sm += move[i];
		sum2 += (long long) move[i] * move[i];
	}
	r->corr = 0;
	if (ns == 0 || nm == 0) return;
	var = n * sum2 - sum * sum;		/* n^2 variance */
	if (var <= 0) return;
	/* mean_move - mean_still = (n sm - nm sum) / (nm ns) */
	num = (n * sm - nm * sum) * 100;
	/* corr = num / (nm ns) * sqrt(ns nm) / sqrt(var) */
	r->corr = (int) (num / ((long long) isqrt((unsigned long long) ns * nm)
				* (long long) isqrt((unsigned long long) var)));
}

int cf_motion_watches(const struct cf_round *r1, const struct cf_round *r2)
{
	return r1->pass && r2->pass;
}

/*===========================================================================*
 *				Locate					     *
 *===========================================================================*/
#define NO_REF	(-128 * 16)

void cf_locate_init(struct cf_locate *l)
{
	l->ema = -100 * 16;
	l->ref = NO_REF;
	l->fed = 0;
}

void cf_locate_feed(struct cf_locate *l, int rssi)
{
	if (!l->fed) { l->ema = rssi * 16; l->fed = 1; return; }
	l->ema += (rssi * 16 - l->ema) / 4;	/* about 1 s at 4 Hz */
}

void cf_locate_mark(struct cf_locate *l) { l->ref = l->ema; }

void cf_locate_out(const struct cf_locate *l, int *pitch_hz, int *period_ms,
								int *bar)
{
	int lv;

	if (l->ref == NO_REF)			/* -100..-30 dBm: 0..100 */
		lv = (l->ema + 100 * 16) * 100 / (70 * 16);
	else					/* the reference: 50, +-20 dB */
		lv = 50 + (l->ema - l->ref) * 50 / (20 * 16);
	if (lv < 0) lv = 0;
	if (lv > 100) lv = 100;
	*bar = lv;
	*pitch_hz = 200 + lv * 18;		/* 200 .. 2000 Hz */
	*period_ms = 1000 - lv * 9;		/* 1 s .. 100 ms */
}

/*===========================================================================*
 *				IR modulation (Goertzel)		     *
 *===========================================================================*/
static long cos_q14(long num, long den)		/* cos(2 pi num / den) */
{
	/* x in [0, 2 pi): fold to [0, pi], then a Taylor series in Q28 */
	long long x, x2, t, s;
	int k, neg = 0;

	num %= den;
	if (num < 0) num += den;
	if (2 * num > den) num = den - num;		/* cos(2pi - x) */
	if (4 * num > den) { num = den / 2 - num; neg = 1; }	/* pi - x */
	x = (long long) num * 1686629713LL / den;	/* 2 pi in Q28 */
	x2 = x * x >> 28;
	s = t = 1LL << 28;
	for (k = 1; k <= 7; k++) {
		t = -t * x2 >> 28;
		t /= (2 * k - 1) * (2 * k);
		s += t;
	}
	s >>= 14;
	return (long) (neg ? -s : s);
}

void cf_gz_init(struct cf_goertzel *g, int f_hz, int fs_hz)
{
	g->coeff = 2 * cos_q14(f_hz, fs_hz);
	g->s1 = g->s2 = 0;
	g->n = 0;
}

void cf_gz_feed(struct cf_goertzel *g, const unsigned short *x, int n,
								int mean)
{
	long long s0;
	int i;

	for (i = 0; i < n; i++) {
		s0 = (long long) (x[i] - mean) + (g->coeff * (long long) g->s1 >> 14)
								- g->s2;
		g->s2 = g->s1;
		g->s1 = (long) s0;
	}
	g->n += n;
}

long long cf_gz_power(const struct cf_goertzel *g)
{
	long long p;

	if (g->n == 0) return 0;
	p = (long long) g->s1 * g->s1 + (long long) g->s2 * g->s2
		- ((g->coeff * (long long) g->s1 >> 14) * g->s2);
	return p / g->n;
}

int cf_ir_modulation(const unsigned short *x, int n, int fs_hz)
{
	static const int f[4] = { 50, 60, 100, 120 };
	static const int kind[4] = { CF_IR_50, CF_IR_60, CF_IR_100, CF_IR_120 };
	struct cf_goertzel g;
	long long sum = 0, e = 0, d = 0, p, best = 0;
	int i, mean, b = -1;

	if (n < fs_hz / 10) return CF_IR_NONE;	/* 100 ms: 10 Hz apart */
	for (i = 0; i < n; i++) sum += x[i];
	mean = (int) (sum / n);
	for (i = 0; i < n; i++) {
		e += (long long) (x[i] - mean) * (x[i] - mean);
		if (i > 0) d += (long long) (x[i] - x[i - 1]) * (x[i] - x[i - 1]);
	}
	/* AC below 2 % of the level (and above a few counts of noise) */
	if (e / n <= (long long) mean * mean / 2500 + 4) return CF_IR_STEADY;
	for (i = 0; i < 4; i++) {
		cf_gz_init(&g, f[i], fs_hz);
		cf_gz_feed(&g, x, n, mean);
		p = cf_gz_power(&g);		/* a tone: |X|^2/n = A^2 n / 4 */
		if (p > best) { best = p; b = i; }
	}
	if (4 * best >= e) return kind[b];	/* >= half of the AC energy */
	return d >= e ? CF_IR_PWM : CF_IR_STEADY;	/* fast: kHz dimming */
}

/*===========================================================================*
 *				Sub-GHz					     *
 *===========================================================================*/
const struct cf_band cf_bands[] = {		/* CC1101, 100 kHz steps */
	{ 300000, 348000, 100 }, { 387000, 464000, 100 }, { 779000, 928000, 100 }
};
const int cf_nbands = sizeof(cf_bands) / sizeof(cf_bands[0]);
const struct cf_band cf_cell_bands[] = {	/* uplinks, 1 MHz steps */
	{ 832000, 862000, 1000 },		/* LTE band 20 */
	{ 880000, 915000, 1000 }		/* LTE band 8 / GSM-900 */
};
const int cf_ncell_bands = sizeof(cf_cell_bands) / sizeof(cf_cell_bands[0]);

int cf_steps(const struct cf_band *b, int nb)
{
	int i, n = 0;

	for (i = 0; i < nb; i++)
		n += (int) ((b[i].end_khz - b[i].start_khz) / b[i].step_khz);
	return n;
}

long cf_step_khz(const struct cf_band *b, int nb, int i)
{
	int k, m;

	for (k = 0; k < nb; k++) {
		m = (int) ((b[k].end_khz - b[k].start_khz) / b[k].step_khz);
		if (i < m) return b[k].start_khz + i * b[k].step_khz;
		i -= m;
	}
	return -1;
}

void cf_burst_init(struct cf_burst *b)
{
	b->floor = 0;
	b->peak = -128;
	b->above = 0;
	b->bursts = 0;
	b->n = 0;
}

void cf_burst_feed(struct cf_burst *b, int rssi)
{
	int r = rssi * 16;

	if (rssi > b->peak) b->peak = (signed char) (rssi > 127 ? 127 : rssi);
	if (b->n++ == 0) { b->floor = r; return; }
	if (!b->above && rssi >= b->floor / 16 + CF_BURST_DB) {
		b->above = 1;			/* a burst starts */
		b->bursts++;
	} else if (b->above && rssi < b->floor / 16 + CF_BURST_DB / 2)
		b->above = 0;			/* hysteresis: 5 dB */
	if (!b->above) {			/* the floor: quiet samples */
		if (r < b->floor) b->floor += (r - b->floor) / 2;
		else b->floor += (r - b->floor) / 64;
	}
}
