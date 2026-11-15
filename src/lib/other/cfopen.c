/* cfopen - open a settings file: the SD card's copy, else the default
 * (Galtzagorriak MINIX, spec L.3).
 *
 *	FILE *cfopen(const char *path, const char *mode)
 *
 * The defaults are a read-only image in internal flash, mounted on /rom
 * (L.2).  Reading: `path` on the card; if it does not exist, "/rom" +
 * path.  Writing: always the card, making the directories as needed;
 * "a" and "+" modes start from a copy of the default when the card has
 * none.  Deleting the file on the card brings the default back; an empty
 * file means an empty setting.  Only absolute paths fall back.
 */
#ifndef _MINIX
#define _MINIX	1		/* the prototype in <stdio.h> */
#endif
#include <sys/types.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ROMDIR	"/rom"

static int rompath(const char *path, char *buf)
{
	if (path[0] != '/' || strlen(ROMDIR) + strlen(path) >= PATH_MAX)
		return 0;
	strcpy(buf, ROMDIR);
	strcat(buf, path);
	return 1;
}

static void mkdirs(const char *path, char *buf)	/* the parents of path */
{
	char *p;

	strncpy(buf, path, PATH_MAX - 1);
	buf[PATH_MAX - 1] = 0;
	for (p = buf + 1; (p = strchr(p, '/')) != NULL; p++) {
		*p = 0;
		(void) mkdir(buf, 0755);	/* EEXIST is fine */
		*p = '/';
	}
}

static void copy_default(const char *path, char *buf)
{
	FILE *in, *out;
	int c;

	if (access(path, 0) == 0 || !rompath(path, buf)) return;
	if ((in = fopen(buf, "r")) == NULL) return;
	if ((out = fopen(path, "w")) != NULL) {
		while ((c = getc(in)) != EOF) putc(c, out);
		fclose(out);
	}
	fclose(in);
}

FILE *cfopen(const char *path, const char *mode)
{
	char buf[PATH_MAX];
	FILE *f;

	if (mode[0] == 'r' && strchr(mode, '+') == NULL) {	/* reading */
		if ((f = fopen(path, mode)) != NULL || errno != ENOENT)
			return f;
		if (!rompath(path, buf)) return NULL;
		return fopen(buf, mode);
	}
	mkdirs(path, buf);			/* writing: the card */
	if (mode[0] == 'a' || strchr(mode, '+') != NULL)
		copy_default(path, buf);
	return fopen(path, mode);
}
