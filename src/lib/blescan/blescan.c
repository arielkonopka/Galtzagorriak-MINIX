/* blescan - see blescan.h */
#include <string.h>
#include "blescan.h"

static const char *const kind_names[BS_K_N] = {
	"", "Find My", "Tile", "SmartTag", "Find Hub", "Eddystone",
	"iBeacon", "Apple"
};

const char *bs_kind_name(int kind)
{
	return kind >= 0 && kind < BS_K_N ? kind_names[kind] : "";
}

void bs_init(struct bs *s)
{
	memset(s, 0, sizeof *s);
}

int bs_bars(int rssi)
{
	int b = (rssi + 100) * 8 / 60;

	return b < 0 ? 0 : b > 8 ? 8 : b;
}

/* What a service UUID (16-bit) says. */
static int uuid_kind(unsigned u)
{
	switch (u) {
	case 0xFEED: case 0xFEEC: return BS_K_TILE;
	case 0xFD5A: return BS_K_SMARTTAG;
	case 0xFEAA: return BS_K_EDDY;
	}
	return BS_K_NONE;
}

/* The AD structures into d: name, flags, company, kind.  -1 if one runs
 * past the end. */
static int parse_ad(struct bs_dev *d, const uint8_t *p, int len)
{
	int i = 0, l, t, k;

	d->kind = BS_K_NONE;
	d->company = 0;
	d->flags = 0;
	while (i < len) {
		l = p[i];
		if (l == 0) break;			/* padding: the end */
		if (i + 1 + l > len) return -1;
		t = p[i + 1];
		switch (t) {
		case 0x01:				/* flags */
			if (l >= 2) d->flags = p[i + 2];
			break;
		case 0x08: case 0x09:			/* local name */
			if (t == 0x09 || d->name[0] == 0) {
				k = l - 1 < BS_NAME - 1 ? l - 1 : BS_NAME - 1;
				memcpy(d->name, p + i + 2, k);
				d->name[k] = 0;
				for (k = 0; d->name[k]; k++)
					if (d->name[k] < 32 || d->name[k] > 126)
						d->name[k] = '?';
			}
			break;
		case 0x02: case 0x03:			/* 16-bit service UUIDs */
		case 0x16:				/* service data */
			for (k = 0; k + 2 <= (t == 0x16 ? 2 : l - 1); k += 2) {
				unsigned u = p[i + 2 + k] | p[i + 3 + k] << 8;
				int kk = uuid_kind(u);

				if (kk == BS_K_EDDY && t == 0x16 && l >= 4
				    && (p[i + 4] == 0x40 || p[i + 4] == 0x41))
					kk = BS_K_FMDN;	/* Find Hub frame */
				if (kk != BS_K_NONE && d->kind < kk
				    && (d->kind == BS_K_NONE
					|| kk == BS_K_FMDN))
					d->kind = kk;
			}
			break;
		case 0xFF:				/* manufacturer data */
			if (l >= 3) {
				d->company = p[i + 2] | p[i + 3] << 8;
				if (d->company == 0x004C && l >= 4) {
					int ty = p[i + 4];

					if (ty == 0x12 && d->kind == BS_K_NONE)
						d->kind = BS_K_FINDMY;
					else if (ty == 0x02 && l >= 5
						 && p[i + 5] == 0x15
						 && d->kind == BS_K_NONE)
						d->kind = BS_K_IBEACON;
					else if (d->kind == BS_K_NONE)
						d->kind = BS_K_APPLE;
				}
			}
			break;
		}
		i += 1 + l;
	}
	return 0;
}

int bs_report(struct bs *s, const uint8_t addr[6], int atype,
	      const uint8_t *data, int len, int rssi)
{
	struct bs_dev *d = NULL, *w;
	struct bs_dev nd;
	int i, r;

	if (rssi > 20 || rssi < -127) return -1;	/* 127: not available */
	for (i = 0; i < s->n; i++)
		if (memcmp(s->d[i].addr, addr, 6) == 0
		    && s->d[i].atype == (atype & 1)) {
			d = &s->d[i];
			break;
		}
	if (d == NULL) {
		memset(&nd, 0, sizeof nd);
		memcpy(nd.addr, addr, 6);
		nd.atype = (uint8_t) (atype & 1);
		nd.rssi = (int8_t) rssi;
		if (s->n < BS_MAX) {
			d = &s->d[s->n++];
		} else {
			w = &s->d[0];
			for (i = 1; i < s->n; i++)
				if (s->d[i].rssi < w->rssi) w = &s->d[i];
			if (w->rssi >= rssi) return 0;	/* weaker than all */
			d = w;
		}
		*d = nd;
	} else if (d->seen != 0) {
		d->rssi = (int8_t) (d->rssi + (rssi - d->rssi) / 4);
	}
	d->last = (int8_t) rssi;
	if (d->seen < 255) d->seen++;
	/* a scan response adds to the advertisement: keep what was learned */
	nd = *d;
	r = parse_ad(&nd, data, len);
	if (nd.kind == BS_K_NONE) nd.kind = d->kind;
	if (nd.company == 0) nd.company = d->company;
	if (nd.name[0] == 0) memcpy(nd.name, d->name, BS_NAME);
	*d = nd;
	return r;
}

int bs_hci(struct bs *s, const uint8_t *p, int len)
{
	int n, i, taken = 0, plen, rlen, dl = 0, pos = 0, rssi;
	const uint8_t *r, *lens, *data, *rs;

	if (len < 3 || p[0] != 0x04) return -1;
	plen = p[2];
	if (3 + plen > len) return -1;
	if (p[1] != 0x3E) return 0;		/* not an LE meta event */
	if (plen < 2) return -1;
	if (p[3] != 0x02) return 0;		/* not an advertising report */
	/* the spec's layout: each field of all the reports in turn: event
	 * types, address types, addresses, data lengths, data, RSSIs */
	n = p[4];
	r = p + 5;
	rlen = plen - 2;			/* subevent, count */
	if (n == 0 || n > 25 || rlen < 10 * n) return -1;
	lens = r + 8 * n;
	for (i = 0; i < n; i++) dl += lens[i];
	if (rlen != 10 * n + dl) return -1;
	data = r + 9 * n;
	rs = data + dl;
	for (i = 0; i < n; i++) {
		rssi = (int8_t) rs[i];
		if (bs_report(s, r + 2 * n + 6 * i, r[n + i], data + pos,
			      lens[i], rssi) >= 0
		    || (rssi <= 20 && rssi >= -127)) taken++;
		pos += lens[i];
	}
	return taken;
}

int bs_order(const struct bs *s, uint8_t idx[BS_MAX], int trackers_first)
{
	int i, j;
	uint8_t t;

	for (i = 0; i < s->n; i++) idx[i] = (uint8_t) i;
	for (i = 1; i < s->n; i++)
		for (j = i; j > 0; j--) {
			const struct bs_dev *a = &s->d[idx[j - 1]],
					    *b = &s->d[idx[j]];
			int swap = trackers_first && (a->kind != 0) != (b->kind != 0)
				? a->kind == 0 : a->rssi < b->rssi;

			if (!swap) break;
			t = idx[j];
			idx[j] = idx[j - 1];
			idx[j - 1] = t;
		}
	return s->n;
}
