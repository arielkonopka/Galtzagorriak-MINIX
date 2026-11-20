/* mf_dict.c - the MIFARE Classic key dictionary as a stream.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #387.  See mf_dict.h.
 */
#include "mf_dict.h"
#include <ctype.h>
#include <string.h>

int mf_dict_open(mf_dict_t *d, const char *path)
{
	d->line = 0;
	d->fp = fopen(path, "r");
	return d->fp ? 0 : -1;
}

void mf_dict_close(mf_dict_t *d)
{
	if(d->fp) fclose(d->fp);
	d->fp = NULL;
}

void mf_dict_rewind(mf_dict_t *d)
{
	if(d->fp) rewind(d->fp);
	d->line = 0;
}

static int hexval(int c)
{
	if(c >= '0' && c <= '9') return c - '0';
	if(c >= 'a' && c <= 'f') return c - 'a' + 10;
	if(c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

int mf_dict_next(mf_dict_t *d, mf_key_t *k)
{
	char buf[64];		/* a key line is 12 chars; buffer is bounded */
	if(!d->fp) return 0;
	while(fgets(buf, sizeof buf, d->fp)) {
		char *p = buf;
		int i, n = 0;
		d->line++;
		while(*p == ' ' || *p == '\t') p++;
		if(*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
			continue;	/* comment or blank */
		/* collect exactly 12 hex digits */
		for(i = 0; i < MF_KEY_SIZE; i++) {
			int hi = hexval((unsigned char)p[n]);
			int lo = (hi < 0) ? -1 : hexval((unsigned char)p[n + 1]);
			if(hi < 0 || lo < 0) return -1;
			k->data[i] = (uint8_t)(hi << 4 | lo);
			n += 2;
		}
		/* the rest of the line must be only blanks/comment */
		while(p[n] == ' ' || p[n] == '\t') n++;
		if(p[n] != '\0' && p[n] != '\n' && p[n] != '\r' && p[n] != '#')
			return -1;
		return 1;
	}
	return 0;		/* end of file */
}

unsigned long mf_dict_count(mf_dict_t *d)
{
	mf_key_t k;
	unsigned long n = 0;
	int r;
	mf_dict_rewind(d);
	while((r = mf_dict_next(d, &k)) != 0)
		if(r == 1) n++;	/* skip malformed lines in the count */
	mf_dict_rewind(d);
	return n;
}
