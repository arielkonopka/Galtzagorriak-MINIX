/* afmt.h - the About rows' text (spec K.7): pure formatting and parsing,
 * no I/O, tested on the PC (tests/host/settimetest).  Every buffer is at
 * least 24 bytes (af_hex: 2 * n + 1). */
#ifndef AFMT_H
#define AFMT_H

/* "Key: value" line: 1 and the value (without blanks and '\n', cut to
 * n - 1 bytes) in v when its key is key; else 0. */
int af_field(const char *line, const char *key, char *v, int n);

/* Free RAM: "143 KB (hole 120)". */
void af_ram(char *buf, long total, long hole);

/* A shared library of free -v: "21 KB, 612 B each" (size in flash, data
 * in every process). */
void af_lib(char *buf, unsigned long size, unsigned long area);

/* n bytes as upper-case hex. */
void af_hex(char *buf, const unsigned char *b, int n);

#endif
