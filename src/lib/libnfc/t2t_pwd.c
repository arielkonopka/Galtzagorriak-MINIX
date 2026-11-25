/* t2t_pwd.c - see t2t_pwd.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <string.h>
#include "t2t_pwd.h"

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* One dictionary line: 8 hex digits, then anything after a space or '#'. */
static int parse(const char *l, unsigned char pwd[4])
{
	int i, a, b;

	while (*l == ' ' || *l == '\t') l++;
	for (i = 0; i < 4; i++) {
		a = hexval(l[2 * i]);
		b = a < 0 ? -1 : hexval(l[2 * i + 1]);
		if (a < 0 || b < 0) return -1;
		pwd[i] = (unsigned char)(a << 4 | b);
	}
	if (l[8] && l[8] != ' ' && l[8] != '\t' && l[8] != '\n' &&
	    l[8] != '\r' && l[8] != '#') return -1;
	return 0;
}

int t2t_pwd_weak(const unsigned char p[4])
{
	return (p[0] == p[1] && p[1] == p[2] && p[2] == p[3]) ||
	       (p[0] == 0x12 && p[1] == 0x34 && p[2] == 0x56 && p[3] == 0x78);
}

/* The index-th password line of the file (skipping comments), or -1. */
static int nth(const char *path, unsigned long index, unsigned char pwd[4])
{
	FILE *f = fopen(path, "r");
	char l[64];
	unsigned long n = 0;
	int r = -1;

	if (!f) return -1;
	while (fgets(l, sizeof(l), f)) {
		if (!strchr(l, '\n') && !feof(f)) {	/* long line: skip it */
			int c;
			while ((c = fgetc(f)) != EOF && c != '\n') ;
			continue;
		}
		if (parse(l, pwd) < 0) continue;
		if (n++ == index) { r = 0; break; }
	}
	fclose(f);
	return r;
}

int t2t_pwd_known(const char *dict, const unsigned char pwd[4])
{
	unsigned char p[4];
	char l[64];
	FILE *f = fopen(dict, "r");
	int r = 0, c;

	if (!f) return -1;
	while (!r && fgets(l, sizeof(l), f)) {
		if (!strchr(l, '\n') && !feof(f)) {
			while ((c = fgetc(f)) != EOF && c != '\n') ;
			continue;
		}
		if (parse(l, p) == 0 && !memcmp(p, pwd, 4)) r = 1;
	}
	fclose(f);
	return r;
}

int t2t_pwd_add(const char *dict, const unsigned char pwd[4], const char *note)
{
	FILE *f;
	int k, r;

	if (t2t_pwd_known(dict, pwd) == 1) return 0;
	f = fopen(dict, "a");
	if (!f) return -1;
	fprintf(f, "%02X%02X%02X%02X", pwd[0], pwd[1], pwd[2], pwd[3]);
	if (note && *note) {
		fputs("  # ", f);
		for (k = 0; note[k] && k < 40; k++)
			fputc(note[k] == '\n' ? ' ' : note[k], f);
	}
	fputc('\n', f);
	r = fflush(f) == 0 ? 0 : -1;
	if (fclose(f)) r = -1;
	return r;
}

static int save_last(const char *path, const unsigned char pwd[4])
{
	FILE *f = fopen(path, "w");
	int r;

	if (!f) return -1;
	fprintf(f, "%02X%02X%02X%02X\n", pwd[0], pwd[1], pwd[2], pwd[3]);
	r = fflush(f) == 0 ? 0 : -1;
	if (fclose(f)) r = -1;
	return r;
}

int t2t_pwd_choose(const struct t2t_cfg *c, const unsigned char uid[7],
		   unsigned char pwd[4], unsigned char pack[2])
{
	const char *dict = c->dict ? c->dict : T2T_DICT_USER;
	const char *last = c->last ? c->last : T2T_LAST;
	int tries;

	if (c->mode == T2T_LEGACY) return T2T_NONE;
	switch (c->mode) {
	case T2T_OWN:
		memcpy(pwd, c->own, 4);
		if (t2t_pwd_weak(pwd)) return T2T_BADCFG;
		break;
	case T2T_PREVIOUS: {
		FILE *f = fopen(last, "r");
		char l[32];

		if (!f) return T2T_BADCFG;
		l[0] = 0;
		if (!fgets(l, sizeof(l), f)) l[0] = 0;
		fclose(f);
		if (parse(l, pwd) < 0) return T2T_BADCFG;
		break;
	}
	case T2T_DICT:
		if (nth(dict, c->index, pwd) < 0) return T2T_BADCFG;
		break;
	case T2T_RANDOM:
		if (!c->rng) return T2T_BADCFG;
		for (tries = 0; tries < 16; tries++) {
			if (c->rng(pwd, 4) != 0) return T2T_BADCFG;
			if (!t2t_pwd_weak(pwd) &&
			    t2t_pwd_known(dict, pwd) != 1) break;
		}
		if (tries == 16) return T2T_BADCFG;
		break;
	default:
		return T2T_BADCFG;
	}
	/* The rule: in the dictionary first, then (and only then) usable. */
	if (t2t_pwd_add(dict, pwd, c->note ? c->note : "copy") < 0)
		return T2T_ERR;
	if (save_last(last, pwd) < 0) return T2T_ERR;
	pack[0] = (unsigned char)(pwd[0] ^ pwd[2] ^ uid[0]);
	pack[1] = (unsigned char)(pwd[1] ^ pwd[3] ^ uid[1]);
	return T2T_OK;
}
