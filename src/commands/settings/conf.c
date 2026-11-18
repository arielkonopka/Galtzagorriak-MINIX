/* conf.c - a group's file, /etc/flipper/NAME.conf (spec K.7, L.3): "Key:
 * value" lines, the number of an S_INT row or the option of an S_ENUM
 * row.  read/write, no stdio (whose FILE and buffer would be malloc'd:
 * the window app has no heap, K.8).  Reading as cfopen() does: the card's
 * copy, else the default in /rom; writing: only the card. */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include "sgroup.h"

#define CONF_MAX	160		/* a group's file, on the stack */

static void path(char *p, int g, int rom)
{
	strcpy(p, rom ? "/rom/etc/flipper/" : "/etc/flipper/");
	strcat(p, sg_name(g));
	strcat(p, ".conf");
}

/* Option k of "a|b|c": its start, *len its length; NULL past the end. */
const char *sg_opt(const char *opts, int k, int *len)
{
	const char *e;

	for (; k > 0 && opts != NULL; k--)
		if ((opts = strchr(opts, '|')) != NULL) opts++;
	if (opts == NULL) return NULL;
	e = strchr(opts, '|');
	*len = e != NULL ? (int) (e - opts) : (int) strlen(opts);
	return opts;
}

static int saved(const struct srow *r)
{
	return (r->type == S_INT || r->type == S_ENUM)
		&& !(r->flags & S_NOSAVE);
}

void conf_parse(int g, char *buf)
{
	const struct srow *rows = NULL, *r;
	char *line, *next, *v, *e;
	const char *o;
	int n, i, k, len, neg;
	long x;

	rows = sg_rows(g, &n);
	for (line = buf; *line != 0; line = next) {
		if ((next = strchr(line, '\n')) != NULL) *next++ = 0;
		else next = line + strlen(line);
		if ((v = strchr(line, ':')) == NULL) continue;
		*v++ = 0;
		while (*v == ' ' || *v == '\t') v++;
		for (e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\r'); )
			*--e = 0;
		for (i = 0; i < n; i++) {
			r = &rows[i];
			if (!saved(r) || strcmp(r->name, line) != 0) continue;
			if (r->type == S_INT) {
				neg = *v == '-';
				for (x = 0, e = v + neg; *e >= '0' && *e <= '9'
				     && x < 100000L; e++)
					x = x * 10 + (*e - '0');
				if (neg) x = -x;
				if (*e == 0 && e > v + neg && x >= r->min
				    && x <= r->max)
					sg_val[i] = (short) x;
			} else
				for (k = r->min; k <= r->max; k++)
					if ((o = sg_opt(r->opts, k, &len)) != NULL
					    && (int) strlen(v) == len
					    && strncmp(o, v, (size_t) len) == 0)
						sg_val[i] = (short) k;
		}
	}
}

static char *put(char *p, char *end, const char *s, int n)
{
	while (n-- > 0 && *s != 0 && p < end) *p++ = *s++;
	return p;
}

int conf_format(int g, char *buf, int size)
{
	const struct srow *rows, *r;
	char *p = buf, *end = buf + size - 1, t[8];
	const char *o;
	int n, i, len, v, k;

	rows = sg_rows(g, &n);
	for (i = 0; i < n; i++) {
		r = &rows[i];
		if (!saved(r)) continue;
		p = put(p, end, r->name, 99);
		p = put(p, end, ": ", 2);
		if (r->type == S_INT) {
			v = sg_val[i];
			k = 8;
			if (v < 0) p = put(p, end, "-", 1), v = -v;
			do t[--k] = (char) ('0' + v % 10); while ((v /= 10) > 0);
			p = put(p, end, t + k, 8 - k);
		} else if ((o = sg_opt(r->opts, sg_val[i], &len)) != NULL)
			p = put(p, end, o, len);
		p = put(p, end, "\n", 1);
	}
	*p = 0;
	return p < end ? (int) (p - buf) : -1;
}

void conf_load(int g)
{
	char p[40], buf[CONF_MAX];
	int fd, n = -1;

	path(p, g, 0);
	if ((fd = open(p, O_RDONLY)) < 0) {
		path(p, g, 1);
		fd = open(p, O_RDONLY);
	}
	if (fd >= 0) {
		n = (int) read(fd, buf, sizeof buf - 1);
		close(fd);
	}
	if (n <= 0) return;
	buf[n] = 0;
	conf_parse(g, buf);
}

int conf_save(int g)
{
	char p[40], buf[CONF_MAX];
	int fd, n, ok;

	if ((n = conf_format(g, buf, (int) sizeof buf)) < 0) return -1;
	(void) mkdir("/etc/flipper", 0755);
	path(p, g, 0);
	if ((fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) return -1;
	ok = write(fd, buf, (size_t) n) == n;
	return close(fd) == 0 && ok ? 0 : -1;
}
