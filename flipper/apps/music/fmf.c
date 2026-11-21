/* SPDX-License-Identifier: GPL-3.0-or-later
 * fmf - the stock Music Player's song files (FMF and RTTTL), see fmf.h.
 */
#include <string.h>
#include "fmf.h"

#define FS_FIRST	0		/* the first line: which format? */
#define FS_HEAD		1		/* FMF: "Key: value" lines */
#define FS_RDEF		2		/* RTTTL: the d=,o=,b= part */
#define FS_FNOTES	3		/* FMF: the Notes line */
#define FS_RNOTES	4		/* RTTTL: the notes, to the end */
#define FS_TAIL		5		/* after the notes: nothing counts */
#define FS_END		6		/* -1 seen, nothing pending */

/* C8 .. B8 in 1/100 Hz (A4 = 440 Hz); octave o is this >> (8 - o). */
static const unsigned long oct8[12] = {
	418601, 443492, 469864, 497803, 527404, 558765,
	591991, 627193, 664488, 704000, 745862, 790213
};
/* A B C D E F G: semitones above C */
static const unsigned char semi[7] = { 9, 11, 0, 2, 4, 5, 7 };

static int upper(int c)
{
	return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

static int isdig(int c)
{
	return c >= '0' && c <= '9';
}

static int space(int c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

void fmf_init(struct fmf *m)
{
	memset(m, 0, sizeof *m);
	m->bpm = 120;
	m->dur = 4;
	m->oct = 5;
	m->line = 1;
}

const char *fmf_error(int bad)
{
	switch (bad) {
	case FE_TYPE:	 return "not a music file";
	case FE_HEADER:	 return "bad BPM, Duration or Octave";
	case FE_NOTE:	 return "bad note";
	case FE_NONOTES: return "no notes";
	}
	return "";
}

/* A number of at most 4 digits, or -1. */
static int num(const char *s)
{
	int v = 0, k = 0;

	while (*s == ' ') s++;
	if (!isdig(*s)) return -1;
	while (isdig(*s)) {
		if (++k > 4) return -1;
		v = v * 10 + (*s++ - '0');
	}
	while (*s == ' ' || *s == '\r') s++;
	return *s == 0 ? v : -1;
}

static int set(struct fmf *m, int key, int v)
{
	switch (upper(key)) {
	case 'B': if (v < 1 || v > 1000) return -1; m->bpm = (short) v; break;
	case 'D': if (v < 1 || v > 128) return -1; m->dur = (short) v; break;
	case 'O': if (v < 0 || v > 8) return -1; m->oct = (short) v; break;
	}
	return 0;
}

int fmf_note(const struct fmf *m, const char *s, struct fmf_note *n)
{
	const char *p = s;
	unsigned long ms, add, v;
	int d = 0, k = 0, idx, o, rest = 0, c;

	while (isdig(*p)) {
		d = d * 10 + (*p++ - '0');
		if (++k > 3) return -1;
	}
	if (k == 0) d = m->dur;
	if (d < 1 || d > 128) return -1;
	c = upper(*p++);
	if (c == 'P') rest = 1, idx = 0;
	else if (c >= 'A' && c <= 'G') idx = semi[c - 'A'];
	else return -1;
	if (*p == '#') {
		if (rest) return -1;
		idx++, p++;
	}
	k = 0;
	while (*p == '.') k++, p++;
	o = m->oct;
	if (isdig(*p)) {
		o = *p++ - '0';
		if (o > 8) return -1;
	}
	while (*p == '.') k++, p++;
	if (*p != 0 || k > 3) return -1;

	ms = 240000UL / ((unsigned long) m->bpm * (unsigned long) d);
	for (add = ms / 2; k > 0; k--, add /= 2) ms += add;
	if (ms < 1) ms = 1;
	if (ms > 65535) ms = 65535;
	n->ms = (unsigned short) ms;
	if (rest) {
		n->hz = 0;
		return 0;
	}
	idx += o * 12;			/* B#8 is C9: still in 16 bits */
	o = idx / 12;
	v = oct8[idx % 12];
	v = o >= 8 ? v << (o - 8) : v >> (8 - o);
	n->hz = (unsigned short) ((v + 50) / 100);
	if (n->hz == 0) n->hz = 1;
	return 0;
}

static int fail(struct fmf *m, int bad)
{
	m->bad = (unsigned char) bad;
	return FMF_ERR;
}

/* An FMF header line, in lbuf (the key, ':' and the value): 0, -1 a bad
 * value, -2 not the music Filetype. */
static int header(struct fmf *m)
{
	char *v = strchr(m->lbuf, ':');

	if (v == NULL) return 0;	/* a line without a key: ignored */
	*v++ = 0;
	while (*v == ' ') v++;
	if (strcmp(m->lbuf, "Filetype") == 0) {
		char *e = v + strlen(v);

		while (e > v && e[-1] == ' ') *--e = 0;
		return strcmp(v, "Flipper Music Format") == 0 ? 0 : -2;
	}
	if (strcmp(m->lbuf, "BPM") == 0) return set(m, 'B', num(v));
	if (strcmp(m->lbuf, "Duration") == 0) return set(m, 'D', num(v));
	if (strcmp(m->lbuf, "Octave") == 0) return set(m, 'O', num(v));
	return 0;			/* Version and unknown keys */
}

/* The RTTTL defaults, in lbuf: "d=4,o=5,b=100", spaces anywhere. */
static int defaults(struct fmf *m)
{
	char *p = m->lbuf, *e, key;

	m->dur = 4, m->oct = 6, m->bpm = 63;	/* RTTTL's own defaults */
	while (*p) {
		while (*p == ' ' || *p == ',') p++;
		if (*p == 0) break;
		key = *p++;
		while (*p == ' ') p++;
		if (*p++ != '=') return -1;
		if ((e = strchr(p, ',')) != NULL) *e = 0;
		if (upper(key) != 'D' && upper(key) != 'O' && upper(key) != 'B')
			return -1;
		if (set(m, key, num(p)) < 0) return -1;
		if (e == NULL) break;
		p = e + 1;
	}
	return 0;
}

/* The note in tok, if any: FMF_NOTE or FMF_MORE (or FMF_ERR). */
static int flush(struct fmf *m, struct fmf_note *n)
{
	if (m->tl == 0) return FMF_MORE;
	m->tok[m->tl] = 0;
	m->tl = 0;
	if (fmf_note(m, m->tok, n) < 0) return fail(m, FE_NOTE);
	m->notes++;
	return FMF_NOTE;
}

static int feed(struct fmf *m, int c, struct fmf_note *n)
{
	if (c < 0) {			/* the end of the file */
		int r;

		if (m->st == FS_FNOTES || m->st == FS_RNOTES) {
			m->st = FS_END;
			if ((r = flush(m, n)) != FMF_MORE) return r;
		}
		m->st = FS_END;
		return m->notes ? FMF_END : fail(m, FE_NONOTES);
	}
	switch (m->st) {
	case FS_FIRST:
	case FS_HEAD:
	case FS_RDEF:
		if (m->st == FS_FIRST && c == ':') {
			m->lbuf[m->ll] = 0;
			if (strcmp(m->lbuf, "Filetype") == 0) {
				m->st = FS_HEAD;	/* the FMF header */
			} else {
				m->st = FS_RDEF;	/* RTTTL: name read */
				m->ll = 0;
				return FMF_MORE;
			}
		} else if (m->st == FS_HEAD && c == ':' && m->ll == 5
			   && memcmp(m->lbuf, "Notes", 5) == 0) {
			m->st = FS_FNOTES;
			m->ll = 0;
			return FMF_MORE;
		} else if (m->st == FS_RDEF && c == ':') {
			m->lbuf[m->ll] = 0;
			m->ll = 0;
			if (defaults(m) < 0) return fail(m, FE_HEADER);
			m->st = FS_RNOTES;
			return FMF_MORE;
		} else if (c == '\n' && m->st != FS_RDEF) {
			m->lbuf[m->ll] = 0;
			m->ll = 0;
			if (m->st == FS_FIRST) {
				if (m->lbuf[0] == 0) return FMF_MORE;
				return fail(m, FE_TYPE);
			}
			switch (header(m)) {
			case -1: return fail(m, FE_HEADER);
			case -2: return fail(m, FE_TYPE);
			}
			return FMF_MORE;
		}
		if (c == '\n' || c == '\r') return FMF_MORE;
		if (m->ll < FMF_LINE - 1) m->lbuf[m->ll++] = (char) c;
		return FMF_MORE;
	case FS_FNOTES:
	case FS_RNOTES:
		if (c == ',' || (c == '\n' && m->st == FS_FNOTES)) {
			if (c == '\n') m->st = FS_TAIL;
			return flush(m, n);
		}
		if (space(c)) return FMF_MORE;
		if (m->tl >= FMF_TOK - 1) return fail(m, FE_NOTE);
		m->tok[m->tl++] = (char) c;
		return FMF_MORE;
	}
	return FMF_MORE;		/* FS_TAIL, FS_END */
}

int fmf_feed(struct fmf *m, int c, struct fmf_note *n)
{
	int r;

	if (m->bad) return FMF_ERR;
	r = feed(m, c, n);
	if (c == '\n' && r != FMF_ERR) m->line++;	/* line: the error's */
	return r;
}
