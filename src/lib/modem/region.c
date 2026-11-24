/* region.c - radio region tables (spec section Q, issue #292).
 *
 * The parser of the card's /usr/share/region/<CC>.reg files (Q.2), the
 * checks of RQ.1/ACQ.1, and the row choice of RP.6, RP.7 and RQ.6: the
 * first row, in file order, that admits a transmit (band edges with the
 * occupied bandwidth and the crystal error, bandwidth, power, kind of use)
 * and still has room (duty cycle; the per/ctl timing).  The kernel makes
 * the binding check on the flash record (T32b); the library tells the
 * user why a transmit is refused and how long to wait.  Never sends,
 * never waits (rule 0.8).
 */
#include <stdio.h>
#include <string.h>
#include "modem.h"

#define MHZ(m, k)	((unsigned long) (m) * 1000000UL + (unsigned long) (k) * 1000UL)

/* CC1101 PATABLE steps (dBm) at 315-915 MHz, datasheet table 39. */
static const signed char steps[] = { -30, -20, -15, -10, -6, 0, 5, 7, 10 };

/* Countries with a table, and the kernel's band set for each (R8.2, until
 * the flash record of T32b).  No pointers: stays in flash in PIC code. */
static const char countries[][2][3] = {
	{ "EU", "EU" }, { "PL", "EU" }, { "ES", "EU" }, { "FR", "EU" },
	{ "IE", "EU" }, { "FI", "EU" }, { "SE", "EU" }, { "NO", "EU" },
	{ "GB", "EU" }, { "AL", "EU" }, { "US", "US" }, { "CA", "US" },
	{ "AT", "EU" }, { "BE", "EU" }, { "BG", "EU" }, { "HR", "EU" },
	{ "CY", "EU" }, { "CZ", "EU" }, { "DK", "EU" }, { "EE", "EU" },
	{ "DE", "EU" }, { "GR", "EU" }, { "HU", "EU" }, { "IT", "EU" },
	{ "LV", "EU" }, { "LT", "EU" }, { "LU", "EU" }, { "MT", "EU" },
	{ "NL", "EU" }, { "PT", "EU" }, { "RO", "EU" }, { "SK", "EU" },
	{ "SI", "EU" }, { "BA", "EU" }, { "MK", "EU" },
};

const char *mdm_rg_bandset(const char *cc)
{
	unsigned i;

	for (i = 0; i < sizeof(countries) / sizeof(countries[0]); i++)
		if (strcmp(countries[i][0], cc) == 0) return countries[i][1];
	return 0;
}

int mdm_pa_step(int dbm)
{
	int i, best = steps[0];

	for (i = 0; i < (int) sizeof(steps); i++)
		if (steps[i] <= dbm) best = steps[i];
	return best;
}

void mdm_rg_init(struct mdm_region *rg)
{
	memset(rg, 0, sizeof(*rg));
}

static int fail(char *err, int max, const char *why)
{
	if (max > 0) {
		strncpy(err, why, (size_t) max - 1);
		err[max - 1] = 0;
	}
	return -1;
}

/* "433.050" -> Hz; 0 on a bad number */
static unsigned long mhz(const char *s, const char **end)
{
	unsigned long m = 0, k = 0;
	int d = 0;

	if (*s < '0' || *s > '9') return 0;
	while (*s >= '0' && *s <= '9') m = m * 10 + (unsigned long) (*s++ - '0');
	if (*s == '.') {
		s++;
		while (*s >= '0' && *s <= '9') {
			if (d++ >= 3) return 0;		/* kHz at most */
			k = k * 10 + (unsigned long) (*s++ - '0');
		}
		while (d++ < 3) k *= 10;
	}
	*end = s;
	return m > 1000 ? 0 : MHZ(m, k);
}

static int num(const char *s, const char **end, long *v)
{
	int neg = 0, any = 0;

	*v = 0;
	if (*s == '-') { neg = 1; s++; }
	while (*s >= '0' && *s <= '9') {
		*v = *v * 10 + (*s++ - '0');
		if (*v > 100000L) return -1;
		any = 1;
	}
	if (neg) *v = -*v;
	*end = s;
	return any ? 0 : -1;
}

static int cc1101_range(unsigned long lo, unsigned long hi)
{
	return (lo >= MHZ(300, 0) && hi <= MHZ(348, 0))
	    || (lo >= MHZ(387, 0) && hi <= MHZ(464, 0))
	    || (lo >= MHZ(779, 0) && hi <= MHZ(928, 0));
}

static int field(const char **p, char *buf, int max)
{
	int n = 0;

	while (**p && **p != '\t' && **p != '\n' && **p != '\r') {
		if (n < max - 1) buf[n++] = **p;
		(*p)++;
	}
	buf[n] = 0;
	if (**p == '\t') (*p)++;
	return n;
}

static int is_cc(const char *s)
{
	return s[0] >= 'A' && s[0] <= 'Z' && s[1] >= 'A' && s[1] <= 'Z'
		&& s[2] == 0;
}

int mdm_rg_line(struct mdm_region *rg, const char *line, char *err, int max)
{
	static const char use[][5] = { "any", "ctl", "per", "fhss" };
	struct mdm_rgrow *r;
	const char *p = line, *e;
	char f[MDM_RG_REF];
	long v;
	int i, flags = 0;

	if (strncmp(p, "#?", 2) == 0) {
		flags = MDM_RG_UNCHECKED;
		p += 2;
	} else if (*p == '#' || *p == '\n' || *p == '\r' || *p == 0) {
		return 0;			/* a comment, an empty line */
	}
	if (*p < '0' || *p > '9') {		/* a header line */
		e = strchr(p, ':');
		if (flags || e == NULL || e[1] != ' ')
			return fail(err, max, "not a row and not \"Key: value\"");
		e += 2;
		field(&e, f, (int) sizeof(f));
		if (strncmp(p, "Filetype:", 9) == 0) {
			if (strcmp(f, "Galtzagorriak region") != 0)
				return fail(err, max, "not a region file");
		} else if (strncmp(p, "Version:", 8) == 0) {
			if (strcmp(f, "1") != 0)
				return fail(err, max, "unknown version");
		} else if (strncmp(p, "Region:", 7) == 0) {
			if (!is_cc(f)) return fail(err, max, "Region: two capitals");
			memcpy(rg->code, f, 3);
		} else if (strncmp(p, "Base:", 5) == 0) {
			if (!is_cc(f)) return fail(err, max, "Base: two capitals");
			memcpy(rg->base, f, 3);
		} else if (strncmp(p, "Name:", 5) == 0) {
			for (i = 0; i < (int) sizeof(rg->name) - 1 && f[i]; i++)
				rg->name[i] = f[i];
			rg->name[i] = 0;
		}
		return 0;			/* Checked:, Source: and others */
	}
	if (rg->n >= MDM_RG_ROWS) return fail(err, max, "more than 32 rows");
	r = &rg->row[rg->n];
	memset(r, 0, sizeof(*r));
	r->flags = flags;
	if ((r->lo = mhz(p, &e)) == 0 || *e != '\t'
	    || (r->hi = mhz(e + 1, &p)) == 0 || *p++ != '\t')
		return fail(err, max, "bad band edges");
	if (r->lo >= r->hi) return fail(err, max, "lo_mhz not below hi_mhz");
	if (!cc1101_range(r->lo, r->hi))
		return fail(err, max, "band outside the CC1101's ranges");
	if (num(p, &e, &v) < 0 || *e != '\t')
		return fail(err, max, "bad dbm");
	if (v > 10) return fail(err, max, "dbm over +10");
	if (v < -30) return fail(err, max, "dbm under -30");
	r->dbm = (int) v;
	p = e + 1;
	if (strncmp(p, "none\t", 5) == 0) {
		r->duty_pm = 1000;
		p += 5;
	} else {				/* "10%", "1%", "0.1%" */
		if (num(p, &e, &v) < 0) return fail(err, max, "bad duty");
		v *= 10;
		if (*e == '.' && e[1] >= '0' && e[1] <= '9') {
			v += e[1] - '0';
			e += 2;
		}
		if (e[0] != '%' || e[1] != '\t' || v <= 0 || v >= 1000)
			return fail(err, max, "bad duty (none, or 0.1% to 99.9%)");
		r->duty_pm = (int) v;
		p = e + 2;
	}
	if (num(p, &e, &v) < 0 || *e != '\t' || v < 0)
		return fail(err, max, "bad bw_khz");
	r->bw_khz = (unsigned) v;
	p = e + 1;
	field(&p, f, (int) sizeof(f));
	for (i = 0; i < 4 && strcmp(f, use[i]) != 0; i++) ;
	if (i == 4) return fail(err, max, "unknown use (any, ctl, per, fhss)");
	r->use = i;
	field(&p, r->ref, (int) sizeof(r->ref));
	if (r->ref[0] == 0) return fail(err, max, "no ref");
	rg->n++;
	return 0;
}

static int load1(const char *dir, const char *cc, struct mdm_region *rg,
	char *err, int max)
{
	char path[256], line[192], why[64];
	FILE *fp;
	int ln = 0;

	if (!is_cc(cc)) return fail(err, max, "a region is two capitals");
	snprintf(path, sizeof(path), "%s/%s.reg", dir, cc);
	if ((fp = fopen(path, "r")) == NULL) {
		snprintf(why, sizeof(why), "no table for %s; transmit stays off",
			cc);
		return fail(err, max, why);
	}
	while (fgets(line, (int) sizeof(line), fp) != NULL) {
		ln++;
		if (mdm_rg_line(rg, line, why, (int) sizeof(why)) < 0) {
			fclose(fp);
			snprintf(line, sizeof(line), "%s.reg:%d: %s", cc, ln, why);
			return fail(err, max, line);
		}
	}
	fclose(fp);
	if (ln == 0 || rg->code[0] == 0) {
		snprintf(line, sizeof(line), "%s.reg: no Region: line", cc);
		return fail(err, max, line);
	}
	if (strcmp(rg->code, cc) != 0) {
		snprintf(line, sizeof(line), "%s.reg: Region: %s", cc, rg->code);
		return fail(err, max, line);
	}
	return 0;
}

int mdm_rg_load(const char *dir, const char *cc, struct mdm_region *rg,
	char *err, int max)
{
	/* No second table in memory: the file is read once for its Base:,
	 * then the base, then the file again, its rows after the base's. */
	char base[3];

	mdm_rg_init(rg);
	if (load1(dir, cc, rg, err, max) < 0) return -1;
	if (rg->base[0] == 0) return 0;
	memcpy(base, rg->base, 3);
	if (strcmp(base, cc) == 0)
		return fail(err, max, "Base: names the file itself");
	mdm_rg_init(rg);
	if (load1(dir, base, rg, err, max) < 0) return -1;
	if (rg->base[0])			/* one level only (Q.2) */
		return fail(err, max, "Base: the base has a Base");
	rg->code[0] = 0;
	if (load1(dir, cc, rg, err, max) < 0) return -1;
	return 0;
}

void mdm_rg_state_init(struct mdm_rgstate *st)
{
	int i;

	for (i = 0; i < MDM_RG_ROWS; i++) mdm_duty_init(&st->duty[i]);
	st->t_s = 0;
	st->len_10ms = 0;
}

int mdm_rg_tx(const struct mdm_region *rg, struct mdm_rgstate *st,
	unsigned long freq, unsigned long occ, int dbm, int mode,
	unsigned air_10ms, unsigned long now_s, int *row, long *wait)
{
	const struct mdm_rgrow *r;
	unsigned long half, next;
	long w, best = -1;
	int i;

	*row = -1;
	*wait = 0;
	/* half the signal, plus 20 ppm of crystal error each way (RP.6) */
	half = occ / 2 + freq / 50000UL + 1;
	for (i = 0; i < rg->n; i++) {
		r = &rg->row[i];
		if (r->flags & MDM_RG_UNCHECKED) continue;
		if (freq < r->lo + half || freq + half > r->hi) continue;
		if (r->bw_khz && occ > (unsigned long) r->bw_khz * 1000UL)
			continue;
		if (dbm > r->dbm) continue;
		/* fhss rows: the hop table is checked at arm time (R19.11) */
		if (r->use == MDM_USE_FHSS) continue;
		if (r->use == MDM_USE_CTL && mode != MDM_SG_CTL) continue;
		if (r->use == MDM_USE_CTL && air_10ms > 500) continue;
		if (r->use == MDM_USE_PER && air_10ms > 100) continue;
		w = mdm_duty_wait(&st->duty[i], now_s, r->duty_pm, air_10ms);
		if (w < 0) continue;		/* longer than the hour allows */
		if (w == 0 && r->use == MDM_USE_PER && st->len_10ms) {
			/* silence >= 30 x the last one, never < 10 s */
			next = (unsigned long) st->len_10ms * 30UL / 100UL;
			if (next < 10) next = 10;
			next += st->t_s;
			if (now_s < next) w = (long) (next - now_s);
		}
		if (w > 0) {
			if (best < 0 || w < best) best = w;
			continue;
		}
		*row = i;
		mdm_duty_add(&st->duty[i], now_s, air_10ms);
		if (r->use == MDM_USE_PER || r->use == MDM_USE_CTL) {
			st->t_s = now_s;
			st->len_10ms = air_10ms;
		}
		return MDM_SG_OK;
	}
	if (best < 0) return MDM_SG_BAND;
	*wait = best;
	return MDM_SG_WAIT;
}

const char *mdm_rg_msg(int rc)
{
	switch (rc) {
	case MDM_SG_OK: return "allowed";
	case MDM_SG_REGION: return "no table for this region; transmit stays off";
	case MDM_SG_BAND: return "refused: no band of the region admits this "
		"frequency, bandwidth, power and use";
	case MDM_SG_WAIT: return "the band's duty cycle or pause is not over";
	}
	return "refused";
}
