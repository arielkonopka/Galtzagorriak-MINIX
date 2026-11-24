/* aid_scan.c - the read-only service scanner (spec Phase 14, issue #451).
 * See aid_scan.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "aid_scan.h"
#include "iso14443_3a.h"
#include <string.h>

int aid_dict_open(aid_dict_t *d, const char *path)
{
	memset(d, 0, sizeof *d);
	d->fp = fopen(path, "r");
	return d->fp ? 0 : -1;
}

void aid_dict_close(aid_dict_t *d)
{
	if(d->fp) fclose(d->fp);
	d->fp = NULL;
}

static int hexval(int c)
{
	if(c >= '0' && c <= '9') return c - '0';
	if(c >= 'a' && c <= 'f') return c - 'a' + 10;
	if(c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static char *skipb(char *p)
{
	while(*p == ' ' || *p == '\t') p++;
	return p;
}

/* Copy up to the end of the line (no CR/LF, no trailing blanks). */
static void copyl(char *out, size_t n, const char *p)
{
	size_t i = 0;

	while(p[i] && p[i] != '\n' && p[i] != '\r' && i < n - 1) {
		out[i] = p[i];
		i++;
	}
	while(i > 0 && (out[i - 1] == ' ' || out[i - 1] == '\t')) i--;
	out[i] = 0;
}

/* Read one whole line into buf; a longer line is cut and its rest
 * dropped.  0 at the end of the file. */
static int getl(FILE *fp, char *buf, size_t n)
{
	size_t l;

	if(!fgets(buf, (int)n, fp)) return 0;
	l = strlen(buf);
	if(l == n - 1 && buf[l - 1] != '\n') {
		int c;

		while((c = getc(fp)) != EOF && c != '\n') ;
	}
	return 1;
}

/* "[Group] desc": the group name into g, a pointer to desc in *desc. */
static int group_line(char *p, char *g, size_t gn, char **desc)
{
	char *e = strchr(p, ']');
	size_t l;

	if(*p != '[' || !e) return -1;
	l = (size_t)(e - p - 1);
	if(l == 0) return -1;
	if(l > gn - 1) l = gn - 1;
	memcpy(g, p + 1, l);
	g[l] = 0;
	*desc = skipb(e + 1);
	return 0;
}

int aid_dict_next(aid_dict_t *d, aid_entry_t *e)
{
	char buf[128], *p, *desc;
	int i;

	if(!d->fp) return 0;
	while(getl(d->fp, buf, sizeof buf)) {
		d->line++;
		p = skipb(buf);
		if(*p == '#' || *p == '\n' || *p == '\r' || *p == 0) continue;
		if(*p == '[') {
			if(group_line(p, d->group, sizeof d->group, &desc) < 0)
				d->bad++;
			continue;
		}
		for(i = 0; hexval((unsigned char)p[i]) >= 0; i++) ;
		if(i % 2 || i / 2 < AID_MIN || i / 2 > AID_MAX
		   || (p[i] != ' ' && p[i] != '\t' && p[i] != '\n'
		       && p[i] != '\r' && p[i] != 0)) {
			d->bad++;
			continue;
		}
		memset(e, 0, sizeof *e);
		e->aid_len = (uint8_t)(i / 2);
		for(i = 0; i < e->aid_len; i++)
			e->aid[i] = (uint8_t)(hexval((unsigned char)p[2 * i]) << 4
					      | hexval((unsigned char)p[2 * i + 1]));
		copyl(e->name, sizeof e->name, skipb(p + 2 * e->aid_len));
		memcpy(e->group, d->group, sizeof e->group);
		return 1;
	}
	return 0;
}

int aid_dict_group_desc(const char *path, const char *group,
			char *out, size_t n)
{
	aid_dict_t d;
	char buf[128], g[AID_GROUP_MAX], *p, *desc;
	int r = -1;

	if(aid_dict_open(&d, path) < 0) return -1;
	while(getl(d.fp, buf, sizeof buf)) {
		p = skipb(buf);
		if(*p == '[' && group_line(p, g, sizeof g, &desc) == 0
		   && !strcmp(g, group)) {
			copyl(out, n, desc);
			r = 0;
			break;
		}
	}
	aid_dict_close(&d);
	return r;
}

void aid_hex(char *out, const uint8_t *p, unsigned len)
{
	static const char h[] = "0123456789ABCDEF";
	unsigned i;

	for(i = 0; i < len; i++) {
		out[2 * i] = h[p[i] >> 4];
		out[2 * i + 1] = h[p[i] & 15];
	}
	out[2 * len] = 0;
}

int aid_sw_present(uint16_t sw)
{
	/* 9000 selected; 61xx selected, more FCI to fetch; 6283 selected
	 * but the application is blocked (ISO 7816-4 5.6 / Table 6). */
	return sw == 0x9000 || (sw >> 8) == 0x61 || sw == 0x6283;
}

/* 3A activation and RATS. */
static int activate(const nfc_xfer_t *x, aid_scan_t *s, int wake)
{
	iso14443_3a_tag_t t;
	int r = iso14443_3a_poll(x, wake, &t);

	if(r != NFC_OK) return r;
	memcpy(s->atqa, t.atqa, 2);
	s->sak = t.sak;
	memcpy(s->uid, t.uid, t.uid_len);
	s->uid_len = t.uid_len;
	if(!(t.sak & 0x20)) return NFC_ERR_NOTSUP;	/* not ISO 14443-4 */
	r = iso_dep_activate(&s->dep, x);
	if(r != NFC_OK) return r;
	memcpy(s->ats, s->dep.ats, s->dep.ats_len);
	s->ats_len = s->dep.ats_len;
	return NFC_OK;
}

int aid_scan(const nfc_xfer_t *x, const char *path, aid_scan_t *s,
	     int (*progress)(void *ctx, unsigned tried), void *ctx)
{
	aid_dict_t d;
	aid_entry_t e;
	uint8_t apdu[6 + AID_MAX];
	size_t rn;
	int r;

	memset(s, 0, sizeof *s);
	if(aid_dict_open(&d, path) < 0) return AID_SCAN_NODICT;
	r = activate(x, s, 0);
	if(r != NFC_OK) {
		aid_dict_close(&d);
		return r;
	}
	while(aid_dict_next(&d, &e) == 1) {
		uint16_t sw;

		apdu[0] = 0x00;			/* CLA */
		apdu[1] = 0xA4;			/* SELECT */
		apdu[2] = 0x04;			/* by DF name (AID) */
		apdu[3] = 0x00;			/* first or only occurrence */
		apdu[4] = e.aid_len;
		memcpy(apdu + 5, e.aid, e.aid_len);
		apdu[5 + e.aid_len] = 0x00;	/* Le: the FCI, if any */
		s->tried++;
		r = iso_dep_xchg(&s->dep, apdu, 6u + e.aid_len, s->resp,
				 sizeof s->resp, &rn);
		if(r == NFC_OK || r == NFC_ERR_OVERFLOW)
			sw = iso_dep_sw(s->resp, rn);	/* a long FCI too */
		else {
			/* The card went away or got lost: wake it once
			 * and go on with the next AID. */
			s->lost++;
			if(s->lost > 2 || activate(x, s, 1) != NFC_OK) break;
			sw = 0;
		}
		if(aid_sw_present(sw)) {
			if(s->nhits < AID_HITS_MAX) {
				s->hit[s->nhits].e = e;
				s->hit[s->nhits].sw = sw;
			}
			s->nhits++;
		}
		if(progress && progress(ctx, s->tried)) break;
	}
	s->bad_lines = d.bad;
	aid_dict_close(&d);
	iso_dep_deselect(&s->dep);
	return NFC_OK;
}
