/* afmt.c - the About rows' text (spec K.7); see afmt.h. */
#include <string.h>
#include "afmt.h"

int af_field(const char *line, const char *key, char *v, int n)
{
	size_t k = strlen(key);
	int i = 0;

	if (strncmp(line, key, k) != 0 || line[k] != ':') return 0;
	for (line += k + 1; *line == ' ' || *line == '\t'; line++) ;
	while (line[i] != 0 && line[i] != '\n' && line[i] != '\r' && i < n - 1) {
		v[i] = line[i];
		i++;
	}
	while (i > 0 && (v[i - 1] == ' ' || v[i - 1] == '\t')) i--;
	v[i] = 0;
	return 1;
}

/* No sprintf: the window app links no stdio (K.8). */
char *af_dec(char *p, unsigned long v)
{
	char t[12];
	int n = 0;

	do t[n++] = (char) ('0' + v % 10); while ((v /= 10) > 0);
	while (n > 0) *p++ = t[--n];
	*p = 0;
	return p;
}

void af_ram(char *buf, long total, long hole)
{
	char *p = af_dec(buf, (unsigned long) (total / 1024 % 10000));

	strcpy(p, " KB (hole ");
	p = af_dec(p + 10, (unsigned long) (hole / 1024 % 10000));
	strcpy(p, ")");
}

void af_lib(char *buf, unsigned long size, unsigned long area)
{
	char *p = af_dec(buf, (size + 1023) / 1024 % 10000);

	strcpy(p, " KB, ");
	p = af_dec(p + 5, area % 100000);
	strcpy(p, " B each");
}

void af_hex(char *buf, const unsigned char *b, int n)
{
	static const char hex[] = "0123456789ABCDEF";
	int i;

	for (i = 0; i < n; i++) {
		buf[2 * i] = hex[b[i] >> 4];
		buf[2 * i + 1] = hex[b[i] & 15];
	}
	buf[2 * n] = 0;
}
