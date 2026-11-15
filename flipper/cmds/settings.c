/* settings - the SD card's settings against their defaults in /rom
 * (Galtzagorriak MINIX, spec L.4).
 *
 * Usage:	settings diff		list the files under /etc that differ
 *					from their default (/rom/etc)
 *		settings reset [path ...]
 *					back to the default: the named files,
 *					or every one under /etc with a default
 *
 * Reset removes the card's copy, so cfopen()/cfcat read the default.
 * Files that the system reads directly at boot or login (rc, ttytab,
 * profile, fstab, setup, passwd, group) are copied from the default
 * instead, so the card keeps one.  The accounts (passwd, group) are reset
 * only when they are named.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define ROM	"/rom"

static char *keep[] = { "/etc/rc", "/etc/ttytab", "/etc/profile",
			"/etc/fstab", "/etc/setup", "/etc/passwd", "/etc/group",
			NULL };
static char *accounts[] = { "/etc/passwd", "/etc/group", NULL };

static int in(char **list, const char *path)
{
	for (; *list != NULL; list++) if (strcmp(*list, path) == 0) return 1;
	return 0;
}

static int same(const char *a, const char *b)	/* 1: equal, 0: no, -1 */
{
	FILE *fa, *fb;
	int ca, cb;

	if ((fa = fopen(a, "r")) == NULL) return -1;
	if ((fb = fopen(b, "r")) == NULL) { fclose(fa); return -1; }
	do {
		ca = getc(fa);
		cb = getc(fb);
	} while (ca == cb && ca != EOF);
	fclose(fa);
	fclose(fb);
	return ca == cb;
}

static int copy(const char *from, const char *to)
{
	FILE *in, *out;
	int c;

	if ((in = fopen(from, "r")) == NULL) return -1;
	if ((out = fopen(to, "w")) == NULL) { fclose(in); return -1; }
	while ((c = getc(in)) != EOF) putc(c, out);
	fclose(in);
	return fclose(out) == 0 ? 0 : -1;
}

static int reset(const char *path)		/* one file, absolute */
{
	char def[PATH_MAX];
	int changed;

	if (strlen(ROM) + strlen(path) >= sizeof(def)) return 1;
	strcpy(def, ROM);
	strcat(def, path);
	if (access(def, 0) < 0) {
		fprintf(stderr, "settings: %s has no default\n", path);
		return 1;
	}
	if (in(keep, path)) {
		if (same(path, def) == 1) return 0;
		if (copy(def, path) < 0) {
			fprintf(stderr, "settings: %s: %s\n", path,
							strerror(errno));
			return 1;
		}
		printf("%s: the default again\n", path);
		return 0;
	}
	changed = same(path, def) == 0;
	if (unlink(path) < 0) {
		if (errno == ENOENT) return 0;
		fprintf(stderr, "settings: %s: %s\n", path, strerror(errno));
		return 1;
	}
	if (changed) printf("%s: the default again\n", path);
	return 0;
}

/* Each file under ROM + dir (one level of subdirectories is enough for
 * /etc): diff (all = 0) or reset (all = 1). */
static int walk(const char *dir, int all)
{
	char rdir[PATH_MAX], card[PATH_MAX], def[PATH_MAX];
	struct dirent *e;
	struct stat st;
	DIR *d;
	int r = 0;

	sprintf(rdir, "%s%s", ROM, dir);
	if ((d = opendir(rdir)) == NULL) {
		fprintf(stderr, "settings: %s: %s (is /rom mounted?)\n", rdir,
							strerror(errno));
		return 1;
	}
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		if (strlen(dir) + strlen(e->d_name) + strlen(ROM) + 2
							> sizeof(def)) continue;
		sprintf(card, "%s/%s", dir, e->d_name);
		sprintf(def, "%s%s", ROM, card);
		if (stat(def, &st) < 0) continue;
		if (S_ISDIR(st.st_mode)) { r |= walk(card, all); continue; }
		if (access(card, 0) < 0) continue;	/* the default already */
		if (all) {
			if (!in(accounts, card)) r |= reset(card);
		} else if (same(card, def) == 0)
			printf("%s\n", card);
	}
	closedir(d);
	return r;
}

int main(int argc, char **argv)
{
	int i, r = 0;

	if (argc == 2 && strcmp(argv[1], "diff") == 0) return walk("/etc", 0);
	if (argc == 2 && strcmp(argv[1], "reset") == 0) return walk("/etc", 1);
	if (argc > 2 && strcmp(argv[1], "reset") == 0) {
		for (i = 2; i < argc; i++) {
			if (argv[i][0] != '/') {
				fprintf(stderr, "settings: %s: an absolute "
					"path, please\n", argv[i]);
				r = 1;
				continue;
			}
			r |= reset(argv[i]);
		}
		return r;
	}
	fprintf(stderr, "Usage: settings diff | reset [path ...]\n");
	return 1;
}
