/* afmt.c - the About rows' text (spec K.7); see afmt.h. */
#include <stdio.h>
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

void af_ram(char *buf, long total, long hole)
{
	sprintf(buf, "%ld KB (hole %ld)", total / 1024 % 10000,
		hole / 1024 % 10000);
}

void af_lib(char *buf, unsigned long size, unsigned long area)
{
	sprintf(buf, "%lu KB, %lu B each", (size + 1023) / 1024 % 10000,
		area % 100000);
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
