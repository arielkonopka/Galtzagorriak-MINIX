/* SPDX-License-Identifier: GPL-3.0-or-later
 * irfile - stock infrared files as a stream; see irfile.h.
 * No malloc, no float (MINIX's printf has no %f on the Flipper): the duty
 * cycle "0.330000" is read and written as millionths.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "irfile.h"

#define VALMAX	64			/* longest value kept (others: cut) */

/* Skip blank and comment lines, read "key:" into key.  1, or 0 at EOF.
 * A line without ':' is skipped. */
static int getkey(struct irf *f, char *key, int ksz)
{
	int c, n;

	for (;;) {
		c = getc(f->fp);
		if (c == EOF) return 0;
		if (c == '\n') { f->line++; continue; }
		if (c == '#' || c == ' ' || c == '\t' || c == '\r') {
			while ((c = getc(f->fp)) != EOF && c != '\n') ;
			f->line++;
			if (c == EOF) return 0;
			continue;
		}
		n = 0;
		while (c != EOF && c != ':' && c != '\n') {
			if (n < ksz - 1) key[n++] = c;
			c = getc(f->fp);
		}
		key[n] = 0;
		if (c == ':') {
			c = getc(f->fp);
			if (c != ' ') ungetc(c, f->fp);
			return 1;
		}
		f->line++;
		if (c == EOF) return 0;
	}
}

/* The rest of the line, trailing blanks cut; longer than sz-1: cut. */
static void getval(struct irf *f, char *buf, int sz)
{
	int c, n = 0;

	while ((c = getc(f->fp)) != EOF && c != '\n')
		if (n < sz - 1) buf[n++] = c;
	f->line++;
	while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\r'
			|| buf[n - 1] == '\t')) n--;
	buf[n] = 0;
}

/* Numbers to the end of the line into d[0..cap).  The count, or -1 when
 * there are more than cap or a word is not a number (the line is read to
 * its end either way). */
static long getnums(struct irf *f, uint32_t *d, size_t cap)
{
	int c, bad = 0, in = 0;
	size_t n = 0;
	unsigned long v = 0;

	for (;;) {
		c = getc(f->fp);
		if (c >= '0' && c <= '9') {
			if (v > 429496729UL) bad = 1;
			v = v * 10 + (c - '0');
			in = 1;
			continue;
		}
		if (in) {
			if (n < cap) d[n] = (uint32_t) v;
			n++;
			v = 0;
			in = 0;
		}
		if (c == '\n') {
			f->line++;
			/* As FlipperFormat: the array goes on while the next
			 * line starts with a number (stock's SIRC vectors). */
			c = getc(f->fp);
			if (c >= '0' && c <= '9') { ungetc(c, f->fp); continue; }
			if (c != EOF) ungetc(c, f->fp);
			break;
		}
		if (c == EOF) break;
		if (c != ' ' && c != '\t' && c != '\r') bad = 1;
	}
	if (bad || n > cap) return -1;
	return (long) n;
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

/* "07 00 00 00": 4 bytes, the first the lowest (stock's byte order). */
static int gethex4(const char *s, uint32_t *v)
{
	int i, h, l;

	*v = 0;
	for (i = 0; i < 4; i++) {
		while (*s == ' ') s++;
		if ((h = hexval(s[0])) < 0 || (l = hexval(s[1])) < 0) return -1;
		*v |= (uint32_t) (h << 4 | l) << (8 * i);
		s += 2;
		if (*s != ' ' && *s != 0) return -1;
	}
	while (*s == ' ') s++;
	return *s ? -1 : 0;
}

static int getulong(const char *s, unsigned long *v)
{
	unsigned long x = 0;

	if (!isdigit((unsigned char) *s)) return -1;
	while (isdigit((unsigned char) *s)) {
		if (x > 429496729UL) return -1;
		x = x * 10 + (*s++ - '0');
	}
	if (*s) return -1;
	*v = x;
	return 0;
}

/* "0.330000" (or "0.33", "1") as millionths. */
static int getduty(const char *s, unsigned long *v)
{
	unsigned long ip = 0, fp = 0, sc = IRF_DUTY_ONE;

	if (!isdigit((unsigned char) *s)) return -1;
	while (isdigit((unsigned char) *s)) ip = ip * 10 + (*s++ - '0');
	if (*s == '.') {
		s++;
		while (isdigit((unsigned char) *s)) {
			sc /= 10;
			if (sc) fp += (*s - '0') * sc;
			s++;
		}
	}
	if (*s || ip > 1 || (ip == 1 && fp)) return -1;
	*v = ip * IRF_DUTY_ONE + fp;
	return 0;
}

int irf_name_ok(const char *name)
{
	size_t n = strlen(name);

	if (n == 0 || n > IRF_NAME_MAX) return 0;
	for (; *name; name++)
		if (*name < 0x20 || *name > 0x7E) return 0;
	return 1;
}

int irf_msg_valid(const InfraredMessage *m)
{
	unsigned a, c;

	if (!infrared_is_protocol_valid(m->protocol)) return 0;
	a = infrared_get_protocol_address_length(m->protocol);
	c = infrared_get_protocol_command_length(m->protocol);
	if (a < 32 && (m->address >> a) != 0) return 0;
	if (c < 32 && (m->command >> c) != 0) return 0;
	return 1;
}

int irf_open(struct irf *f, FILE *fp)
{
	char key[16], val[VALMAX];

	memset(f, 0, sizeof(*f));
	f->fp = fp;
	f->line = 1;
	if (!getkey(f, key, sizeof(key)) || strcmp(key, "Filetype")) return -1;
	getval(f, val, sizeof(val));
	if (!strcmp(val, "IR signals file")) f->kind = IRF_SIGNALS;
	else if (!strcmp(val, "IR library file")) f->kind = IRF_LIBRARY;
	else if (!strcmp(val, "IR tests file")) f->kind = IRF_TESTS;
	else return -1;
	if (!getkey(f, key, sizeof(key)) || strcmp(key, "Version")) return -1;
	getval(f, val, sizeof(val));
	return strcmp(val, "1") ? -1 : 0;
}

/* Field bits of a signal. */
#define F_PROTO	0x01
#define F_ADDR	0x02
#define F_CMD	0x04
#define F_FREQ	0x08
#define F_DUTY	0x10
#define F_DATA	0x20
#define F_COUNT	0x40
#define F_REP	0x80
#define NEED_PARSED	(F_PROTO | F_ADDR | F_CMD)
#define NEED_RAW	(F_FREQ | F_DUTY | F_DATA)

/* Read fields until the set `need` (0: not known yet, from "type") is
 * complete or a new "name" starts.  Returns 1 complete, 0 EOF, -1 bad. */
static int fields(struct irf *f, struct irf_sig *s, InfraredMessage *m,
	unsigned need, int bad)
{
	char key[16], val[VALMAX];
	unsigned have = 0;
	long n;

	while (need == 0 || (have & need) != need) {
		if (!getkey(f, key, sizeof(key))) { f->eof = 1; return bad ? -1 : 0; }
		if (!strcmp(key, "name")) {
			getval(f, f->next, sizeof(f->next));
			f->havenext = 1;
			return -1;		/* the signal ended early */
		}
		if (s && !strcmp(key, "data") && s->type == IRF_RAW) {
			n = getnums(f, s->data, s->cap);
			if (n < 0) bad = 1; else s->n = (size_t) n;
			have |= F_DATA;
			continue;
		}
		getval(f, val, sizeof(val));
		if (s && !strcmp(key, "type")) {
			if (!strcmp(val, "parsed")) {
				s->type = IRF_PARSED; need = NEED_PARSED;
			} else if (!strcmp(val, "raw")) {
				s->type = IRF_RAW;	/* tests: data only */
				need = f->kind == IRF_TESTS ? F_DATA : NEED_RAW;
			} else if (!strcmp(val, "parsed_array")) {
				s->type = IRF_ARRAY; need = F_COUNT;
			} else bad = 1;
		} else if (!strcmp(key, "protocol")) {
			m->protocol = infrared_get_protocol_by_name(val);
			if (!infrared_is_protocol_valid(m->protocol)) bad = 1;
			have |= F_PROTO;
		} else if (!strcmp(key, "address")) {
			if (gethex4(val, &m->address)) bad = 1;
			have |= F_ADDR;
		} else if (!strcmp(key, "command")) {
			if (gethex4(val, &m->command)) bad = 1;
			have |= F_CMD;
		} else if (!s && !strcmp(key, "repeat")) {
			if (!strcmp(val, "true")) m->repeat = true;
			else if (strcmp(val, "false")) bad = 1;
			have |= F_REP;
		} else if (s && !strcmp(key, "frequency")) {
			if (getulong(val, &s->freq)) bad = 1;
			have |= F_FREQ;
		} else if (s && !strcmp(key, "duty_cycle")) {
			if (getduty(val, &s->duty)) bad = 1;
			have |= F_DUTY;
		} else if (s && !strcmp(key, "count")) {
			if (getulong(val, &s->count)) bad = 1;
			have |= F_COUNT;
		}				/* other keys: ignored */
	}
	return bad ? -1 : 1;
}

int irf_next(struct irf *f, struct irf_sig *s)
{
	char key[16];
	int r;

	s->type = 0xFF;
	s->n = 0;
	s->freq = INFRARED_COMMON_CARRIER_FREQUENCY;
	s->duty = 330000;
	s->count = 0;
	memset(&s->msg, 0, sizeof(s->msg));
	if (f->havenext) {
		f->havenext = 0;
		strcpy(s->name, f->next);
	} else {
		for (;;) {
			if (f->eof || !getkey(f, key, sizeof(key))) {
				f->eof = 1;
				return 0;
			}
			if (!strcmp(key, "name")) break;
			getval(f, s->name, sizeof(s->name));	/* stray: skip */
		}
		getval(f, s->name, sizeof(s->name));
	}
	r = fields(f, s, &s->msg, 0, !irf_name_ok(s->name));
	if (r == 0) return -1;		/* EOF inside the signal */
	if (r < 0) return -1;
	if (s->type == IRF_PARSED && !irf_msg_valid(&s->msg)) return -1;
	return 1;
}

int irf_find(struct irf *f, const char *name, struct irf_sig *s)
{
	int r;

	while ((r = irf_next(f, s)) != 0)
		if (r > 0 && !strcmp(s->name, name)) return 1;
	return 0;
}

int irf_msg(struct irf *f, InfraredMessage *m)
{
	memset(m, 0, sizeof(*m));
	if (fields(f, NULL, m, NEED_PARSED | F_REP, 0) <= 0) return -1;
	return 0;
}

int irf_write_header(FILE *fp, int kind)
{
	static const char *const ft[] = { "IR signals file", "IR library file",
		"IR tests file" };

	if (kind < 0 || kind > IRF_TESTS) return -1;
	fprintf(fp, "Filetype: %s\nVersion: 1\n", ft[kind]);
	return ferror(fp) ? -1 : 0;
}

static void puthex4(FILE *fp, uint32_t v)
{
	fprintf(fp, "%02X %02X %02X %02X\n", (unsigned) (v & 0xFF),
		(unsigned) (v >> 8 & 0xFF), (unsigned) (v >> 16 & 0xFF),
		(unsigned) (v >> 24 & 0xFF));
}

int irf_write(FILE *fp, const struct irf_sig *s)
{
	size_t i;

	if (!irf_name_ok(s->name)) return -1;
	if (s->type == IRF_PARSED) {
		if (!irf_msg_valid(&s->msg)) return -1;
		fprintf(fp, "#\nname: %s\ntype: parsed\nprotocol: %s\naddress: ",
			s->name, infrared_get_protocol_name(s->msg.protocol));
		puthex4(fp, s->msg.address);
		fputs("command: ", fp);
		puthex4(fp, s->msg.command);
	} else if (s->type == IRF_RAW) {
		if (s->n == 0 || s->n > IRF_RAW_MAX || s->duty > IRF_DUTY_ONE)
			return -1;
		fprintf(fp, "#\nname: %s\ntype: raw\nfrequency: %lu\n"
			"duty_cycle: %lu.%06lu\ndata:", s->name, s->freq,
			s->duty / IRF_DUTY_ONE, s->duty % IRF_DUTY_ONE);
		for (i = 0; i < s->n; i++)
			fprintf(fp, " %lu", (unsigned long) s->data[i]);
		fputc('\n', fp);
	} else return -1;
	return ferror(fp) ? -1 : 0;
}
