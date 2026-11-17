/* cc1101.c - the Sub-GHz modem's pure logic (spec P.4): presets, the
 * CC1101 register values for a data rate, deviation and filter, and the
 * region's bands.  The kernel's transmit check (R8.2, RP.8) compiles this
 * same file, so what the library computes and what the kernel allows can
 * never disagree.  Integer arithmetic only (no FPU context, R5.7).
 */
#include "modem.h"

#define MHZ(a, b)	((a) * 1000000UL + (b) * 1000UL)

/* Values from the CC1101's 26 MHz crystal (P.4 table); `modem info`
 * prints what the chip really gets. */
const struct mdm_preset mdm_presets[] = {
	{ "narrow", {   1200,   5200,  58000, MDM_MOD_GFSK }, "longest range, slow" },
	{ "slow",   {   4800,  11900,  68000, MDM_MOD_GFSK }, "long range" },
	{ "std",    {  38400,  20600, 102000, MDM_MOD_GFSK }, "default" },
	{ "wide",   { 100000,  47600, 325000, MDM_MOD_GFSK }, "fast, shorter range" },
	{ "max",    { 250000, 127000, 650000, MDM_MOD_GFSK }, "short range, bands >= 0.6 MHz" },
	{ "",       {      0,      0,      0, 0 }, "" }
};

/* Data bands (P.4).  First version: to be checked against the current
 * ERC/REC 70-03 Annex 1, EN 300 220, FCC 15.249 and the stock firmware's
 * region data before transmit is enabled (T26b).  dbm: the highest output
 * allowed, counting the antenna as 0 dBi and e.r.p. as conducted power
 * (the Flipper's antenna is worse, so this errs low). */
const struct mdm_band mdm_bands[] = {
	{ "EU", MHZ(433, 50),  MHZ(434, 790), 10, 100, "10 mW e.r.p., 10 %" },
	{ "EU", MHZ(868, 0),   MHZ(868, 600), 14,  10, "25 mW e.r.p., 1 %" },
	{ "EU", MHZ(868, 700), MHZ(869, 200), 14,   1, "25 mW e.r.p., 0.1 %" },
	{ "EU", MHZ(869, 400), MHZ(869, 650), 27, 100, "500 mW e.r.p., 10 %" },
	{ "EU", MHZ(869, 700), MHZ(870, 0),    6, 1000, "5 mW e.r.p., no duty limit" },
	{ "US", MHZ(902, 0),   MHZ(928, 0),   -2, 1000, "FCC 15.249, no duty limit" },
	{ "", 0, 0, 0, 0, "" }
};

/* CC1101 PATABLE steps (dBm) at 315-915 MHz, datasheet table 39. */
static const signed char pa_steps[] = { -30, -20, -15, -10, -6, 0, 5, 7, 10 };

const struct mdm_preset *mdm_preset(const char *name)
{
	const struct mdm_preset *p;
	const char *a, *b;

	for (p = mdm_presets; p->name[0]; p++) {
		for (a = p->name, b = name; *a && *a == *b; a++, b++) ;
		if (*a == 0 && *b == 0) return p;
	}
	return 0;
}

int mdm_cc1101_regs(unsigned long freq, const struct mdm_sgphy *in,
	struct mdm_cc1101 *out)
{
	unsigned long long w;
	unsigned long best, v, d;
	int e, m, be = 0, bm = 0, maxbaud;

	if (!((freq >= MHZ(300, 0) && freq <= MHZ(348, 0))
	    || (freq >= MHZ(387, 0) && freq <= MHZ(464, 0))
	    || (freq >= MHZ(779, 0) && freq <= MHZ(928, 0)))) return -1;
	maxbaud = in->mod == MDM_MOD_2FSK ? 500000 : 250000;
	if (in->baud < 600 || in->baud > (unsigned long) maxbaud) return -1;
	if (in->bw < 58000 || in->bw > 812500) return -1;
	if (in->mod != MDM_MOD_OOK && (in->dev < 1586 || in->dev > 380860))
		return -1;
	if (in->mod != MDM_MOD_2FSK && in->mod != MDM_MOD_GFSK
	    && in->mod != MDM_MOD_OOK) return -1;

	/* FREQ = f * 2^16 / fxosc, rounded */
	w = ((unsigned long long) freq << 16) + MDM_XOSC / 2;
	w /= MDM_XOSC;
	out->freq2 = (unsigned char) (w >> 16);
	out->freq1 = (unsigned char) (w >> 8);
	out->freq0 = (unsigned char) w;
	out->freq = (unsigned long) ((w * MDM_XOSC + (1UL << 15)) >> 16);

	/* rate = (256 + M) * 2^E * fxosc / 2^28: the nearest of all E, M */
	best = 0xFFFFFFFFUL;
	for (e = 0; e < 16; e++)
		for (m = 0; m < 256; m++) {
			w = ((unsigned long long) (256 + m) << e) * MDM_XOSC;
			v = (unsigned long) ((w + (1ULL << 27)) >> 28);
			d = v > in->baud ? v - in->baud : in->baud - v;
			if (d < best) { best = d; be = e; bm = m; out->baud = v; }
		}
	out->mdmcfg3 = (unsigned char) bm;
	out->mdmcfg4 = (unsigned char) be;

	/* filter = fxosc / (8 * (4 + M) * 2^E) */
	best = 0xFFFFFFFFUL;
	for (e = 0; e < 4; e++)
		for (m = 0; m < 4; m++) {
			v = MDM_XOSC / (8UL * (4 + m) << e);
			d = v > in->bw ? v - in->bw : in->bw - v;
			if (d < best) { best = d; be = e; bm = m; out->bw = v; }
		}
	out->mdmcfg4 |= (unsigned char) (be << 6 | bm << 4);

	/* deviation = fxosc / 2^17 * (8 + M) * 2^E */
	best = 0xFFFFFFFFUL;
	out->dev = 0;
	out->deviatn = 0;
	if (in->mod != MDM_MOD_OOK) {
		for (e = 0; e < 8; e++)
			for (m = 0; m < 8; m++) {
				w = ((unsigned long long) (8 + m) << e) * MDM_XOSC;
				v = (unsigned long) ((w + (1ULL << 16)) >> 17);
				d = v > in->dev ? v - in->dev : in->dev - v;
				if (d < best) {
					best = d;
					out->deviatn = (unsigned char) (e << 4 | m);
					out->dev = v;
				}
			}
	}
	/* MOD_FORMAT, SYNC_MODE 3 (30 of 32 sync bits) */
	out->mdmcfg2 = (unsigned char) (in->mod << 4 | 3);
	out->mod = in->mod;
	return 0;
}

unsigned long mdm_occupied(const struct mdm_cc1101 *r)
{
	if (r->mod == MDM_MOD_OOK) return 2 * r->baud;
	return 2 * r->dev + r->baud;
}

int mdm_sg_check(const char *region, const struct mdm_cc1101 *r,
	const struct mdm_band **band)
{
	const struct mdm_band *b;
	const char *x, *y;
	unsigned long half;
	int known = 0;

	*band = 0;
	/* half the signal, plus 20 ppm of crystal error each way */
	half = mdm_occupied(r) / 2 + r->freq / 50000UL + 1;
	for (b = mdm_bands; b->region[0]; b++) {
		for (x = b->region, y = region; *x && *x == *y; x++, y++) ;
		if (*x || *y) continue;
		known = 1;
		if (r->freq >= b->lo + half && r->freq + half <= b->hi) {
			*band = b;
			return MDM_SG_OK;
		}
	}
	return known ? MDM_SG_BAND : MDM_SG_REGION;
}

int mdm_sg_dbm(const struct mdm_band *b)
{
	int i, best = pa_steps[0];

	for (i = 0; i < (int) sizeof(pa_steps); i++)
		if (pa_steps[i] <= b->dbm) best = pa_steps[i];
	return best;
}

long mdm_sg_airtime(unsigned long baud, int n)
{
	/* preamble 4, sync 4 (the word twice), length 1, n, the chip's CRC 2 */
	unsigned long bits = (unsigned long) (n + 11) * 8;

	return (long) ((bits * 1000UL + baud - 1) / baud);
}
