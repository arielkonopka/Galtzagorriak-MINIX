/* beacon.c - the `beacon` canary's core (spec R16.5.7, task T35); see
 * beacon.h.  Formats: Apple "Proximity Beacon Specification" R1 (iBeacon),
 * google/eddystone protocol specification (UID, URL, TLM, EID), AltBeacon
 * protocol specification v1.0 (altbeacon.org).
 */
#include <string.h>
#include "beacon.h"

#define AD_SVCDATA16	0x16
#define AD_MFGDATA	0xFF

static const char *const schemes[4] = {
	"http://www.", "https://www.", "http://", "https://"
};
static const char *const exps[14] = {
	".com/", ".org/", ".edu/", ".net/", ".info/", ".biz/", ".gov/",
	".com", ".org", ".edu", ".net", ".info", ".biz", ".gov"
};

static unsigned u16le(const unsigned char *p)
{
	return p[0] | (unsigned)p[1] << 8;
}

static unsigned long u32be(const unsigned char *p)
{
	return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16
		| (unsigned)p[2] << 8 | p[3];
}

static void setid(struct bc_frame *f, const unsigned char *p, int n)
{
	memcpy(f->id, p, n);
	f->idlen = n;
}

/* One Eddystone service data body (after the 0xFEAA UUID), n bytes. */
static int eddystone(const unsigned char *p, int n, struct bc_frame *f)
{
	int i;

	if (n < 1) return BC_BAD;
	switch (p[0]) {
	case 0x00:			/* UID: tx, namespace 10, instance 6,
					 * 2 RFU bytes some beacons leave out */
		if (n != 18 && n != 20) return BC_BAD;
		f->tx = (signed char)p[1];
		setid(f, p + 2, 16);
		return BC_EDDY_UID;
	case 0x10:			/* URL: tx, scheme, 0-17 bytes */
		if (n < 3 || n > 20 || p[2] > 3) return BC_BAD;
		for (i = 3; i < n; i++)
			if ((p[i] > 0x0d && p[i] <= 0x20) || p[i] >= 0x7f)
				return BC_BAD;
		f->tx = (signed char)p[1];
		setid(f, p + 2, n - 2);
		return BC_EDDY_URL;
	case 0x20:			/* TLM: version, then the version's */
		if (n < 2) return BC_BAD;
		f->tlmver = p[1];
		if (p[1] == 0) {
			if (n != 14) return BC_BAD;
			f->vbatt = (unsigned short)(p[2] << 8 | p[3]);
			if (p[4] != 0x80 || p[5] != 0)
				f->temp = (short)(p[4] << 8 | p[5]);
			f->advcnt = u32be(p + 6);
			f->secs = u32be(p + 10);
		}
		return BC_EDDY_TLM;
	case 0x30:			/* EID: tx, 8 bytes */
		if (n != 10) return BC_BAD;
		f->tx = (signed char)p[1];
		setid(f, p + 2, 8);
		return BC_EDDY_EID;
	}
	return BC_NONE;			/* a frame type we do not know */
}

/* One manufacturer data body (company ID first), n bytes. */
static int mfgdata(const unsigned char *p, int n, struct bc_frame *f)
{
	if (n < 4) return BC_NONE;
	if (u16le(p) == 0x004C && p[2] == 0x02) {	/* Apple, iBeacon */
		if (p[3] != 0x15 || n != 4 + 0x15) return BC_BAD;
		f->mfg = 0x004C;
		setid(f, p + 4, 20);
		f->tx = (signed char)p[24];
		return BC_IBEACON;
	}
	if (p[2] == 0xBE && p[3] == 0xAC) {		/* AltBeacon */
		if (n != 26) return BC_BAD;
		f->mfg = (unsigned short)u16le(p);
		setid(f, p + 4, 20);
		f->tx = (signed char)p[24];
		f->rsv = p[25];
		return BC_ALTBEACON;
	}
	return BC_NONE;
}

int bc_parse(const unsigned char *adv, int len, struct bc_frame *f)
{
	struct bc_frame t;
	int i, l, r, kind = BC_NONE;

	memset(f, 0, sizeof(*f));
	if (adv == 0 || len < 0) return BC_BAD;
	for (i = 0; i < len; i += l + 1) {
		l = adv[i];
		if (l == 0) break;			/* padding */
		if (l > len - i - 1) return BC_BAD;	/* runs past the end */
		memset(&t, 0, sizeof(t));
		t.tx = BC_NOTX;
		t.temp = BC_TEMP_NONE;
		r = BC_NONE;
		if (adv[i + 1] == AD_MFGDATA)
			r = mfgdata(adv + i + 2, l - 1, &t);
		else if (adv[i + 1] == AD_SVCDATA16 && l >= 3
				&& adv[i + 2] == 0xAA && adv[i + 3] == 0xFE)
			r = eddystone(adv + i + 4, l - 3, &t);
		if (r == BC_BAD) {
			memset(f, 0, sizeof(*f));
			return BC_BAD;
		}
		if (r != BC_NONE && kind == BC_NONE) {
			t.kind = (unsigned char)r;
			*f = t;
			kind = r;
		}
	}
	return kind;
}

/* Append s to buf at *o (room n, one byte kept for the NUL). */
static void put(char *buf, int n, int *o, const char *s)
{
	while (*s && *o < n - 1) buf[(*o)++] = *s++;
}

int bc_url(const struct bc_frame *f, char *buf, int n)
{
	char c[2];
	int i, o = 0;

	if (n <= 0) return -1;
	buf[0] = 0;
	if (f->kind != BC_EDDY_URL || f->idlen < 1 || f->id[0] > 3) return -1;
	put(buf, n, &o, schemes[f->id[0]]);
	c[1] = 0;
	for (i = 1; i < f->idlen; i++) {
		if (f->id[i] < 14) put(buf, n, &o, exps[f->id[i]]);
		else { c[0] = (char)f->id[i]; put(buf, n, &o, c); }
	}
	buf[o] = 0;
	return o;
}

/* A frame's deployment key into d (kind, klen, key) and its beacon's
 * sub-ID into sub.  Returns 0 for anonymous Eddystone. */
static int keyof(const struct bc_frame *f, const unsigned char *addr,
			struct bc_dep *d, unsigned char *sub)
{
	int i, c;

	memset(sub, 0, 6);
	if (addr) memcpy(sub, addr, 6);
	d->kind = f->kind;
	switch (f->kind) {
	case BC_IBEACON:
		memcpy(d->key, f->id, 16);
		d->klen = 16;
		memcpy(sub, f->id + 16, 4);
		memset(sub + 4, 0, 2);
		return 1;
	case BC_EDDY_UID:
		memcpy(d->key, f->id, 10);
		d->klen = 10;
		memcpy(sub, f->id + 10, 6);
		return 1;
	case BC_ALTBEACON:
		memcpy(d->key, f->id, 16);
		d->klen = 16;
		memcpy(sub, f->id + 16, 4);
		memset(sub + 4, 0, 2);
		return 1;
	case BC_EDDY_URL:		/* "www." flag, then up to the path */
		d->key[0] = f->id[0] <= 1;
		d->klen = 1;
		for (i = 1; i < f->idlen && d->klen < BC_KEYLEN; i++) {
			c = f->id[i];
			if (c == '/' || c == ':' || c == '?' || c == '#')
				break;
			if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
			d->key[d->klen++] = (unsigned char)c;
			if (c <= 0x06) break;	/* ".com/" and so on */
		}
		return 1;
	}
	return 0;
}

static void see(signed char *best, int rssi)
{
	if (rssi > *best) *best = (signed char)rssi;
}

void bc_init(struct bc_groups *g, struct bc_dep *dep, int ndep,
					struct bc_bcn *bcn, int nbcn)
{
	int i;

	memset(g, 0, sizeof(*g));
	g->dep = dep;
	g->bcn = bcn;
	g->ndep = ndep > 254 ? 254 : ndep < 0 ? 0 : ndep;
	g->nbcn = nbcn > 65535 ? 65535 : nbcn < 0 ? 0 : nbcn;
	memset(dep, 0, g->ndep * sizeof(*dep));
	for (i = 0; i < g->nbcn; i++) {
		memset(&bcn[i], 0, sizeof(*bcn));
		bcn[i].dep = BC_FREE;
	}
}

/* The beacon (dep index d, sub) in the beacon table: found, or put into a
 * free or the least recently seen slot. */
static void beacon(struct bc_groups *g, int d, const unsigned char *sub,
								int rssi)
{
	struct bc_bcn *b, *old = 0;
	int i;

	for (i = 0; i < g->nbcn; i++) {
		b = &g->bcn[i];
		if (b->dep == d && memcmp(b->sub, sub, 6) == 0) {
			b->last = g->clock;
			see(&b->best, rssi);
			return;
		}
		if (old == 0 || (old->dep != BC_FREE && (b->dep == BC_FREE
				|| g->clock - b->last > g->clock - old->last)))
			old = b;
	}
	if (old == 0) return;
	if (old->dep < BC_FREE) g->dep[old->dep].beacons--;
	old->dep = (unsigned char)d;
	memcpy(old->sub, sub, 6);
	old->last = g->clock;
	old->best = (signed char)rssi;
	if (d < BC_FREE) g->dep[d].beacons++;
}

int bc_observe(struct bc_groups *g, int kind, const struct bc_frame *f,
				const unsigned char *addr, int rssi)
{
	struct bc_dep k, *d, *old = 0;
	unsigned char sub[6];
	int i, j;

	if (rssi < -127) rssi = -127;
	if (rssi > 127) rssi = 127;
	g->clock++;
	if (kind == BC_BAD) { g->bad++; return -1; }
	if (kind == BC_NONE || f == 0 || f->kind != kind) {
		g->other++;
		return -1;
	}
	memset(&k, 0, sizeof(k));
	if (!keyof(f, addr, &k, sub)) {
		g->anon++;
		beacon(g, BC_ANON, sub, rssi);
		return BC_ANON;
	}
	for (i = 0; i < g->ndep; i++) {
		d = &g->dep[i];
		if (d->kind == k.kind && d->klen == k.klen
				&& memcmp(d->key, k.key, k.klen) == 0)
			break;
		if (old == 0 || (old->kind != BC_NONE && (d->kind == BC_NONE
				|| g->clock - d->last > g->clock - old->last)))
			old = d;
	}
	if (i == g->ndep) {			/* new: a free or the oldest */
		if (old == 0) return -1;
		i = (int)(old - g->dep);
		for (j = 0; j < g->nbcn; j++)	/* its beacons go with it */
			if (g->bcn[j].dep == i) g->bcn[j].dep = BC_FREE;
		*old = k;
		old->best = (signed char)rssi;
	}
	d = &g->dep[i];
	d->last = g->clock;
	d->frames++;
	see(&d->best, rssi);
	beacon(g, i, sub, rssi);
	return i;
}

void bc_summary(const struct bc_groups *g, struct bc_sum *s)
{
	int i;

	memset(s, 0, sizeof(*s));
	s->best = BC_NOTX;
	for (i = 0; i < g->ndep; i++)
		if (g->dep[i].kind != BC_NONE) {
			s->deps++;
			s->beacons += g->dep[i].beacons;
			if (g->dep[i].best > s->best) s->best = g->dep[i].best;
		}
	for (i = 0; i < g->nbcn; i++)
		if (g->bcn[i].dep == BC_ANON) {
			s->anon++;
			if (g->bcn[i].best > s->best) s->best = g->bcn[i].best;
		}
}

int bc_dep_text(const struct bc_dep *d, char *buf, int n)
{
	static const char hex[] = "0123456789abcdef";
	char c[3];
	int i, o = 0;

	if (n <= 0) return -1;
	buf[0] = 0;
	if (d->kind == BC_NONE || d->klen > BC_KEYLEN) return -1;
	c[2] = 0;
	if (d->kind == BC_EDDY_URL) {
		if (d->klen < 1) return -1;
		if (d->key[0]) put(buf, n, &o, "www.");
		c[1] = 0;
		for (i = 1; i < d->klen; i++) {
			if (d->key[i] < 14) {	/* ".com/" without the "/" */
				put(buf, n, &o, exps[d->key[i] < 7
					? d->key[i] + 7 : d->key[i]]);
				continue;
			}
			c[0] = (char)d->key[i];
			put(buf, n, &o, c);
		}
		buf[o] = 0;
		return o;
	}
	for (i = 0; i < d->klen; i++) {	/* UUID style for 16 bytes */
		if (d->klen == 16 && (i == 4 || i == 6 || i == 8 || i == 10))
			put(buf, n, &o, "-");
		c[0] = hex[d->key[i] >> 4];
		c[1] = hex[d->key[i] & 15];
		put(buf, n, &o, c);
	}
	buf[o] = 0;
	return o;
}
