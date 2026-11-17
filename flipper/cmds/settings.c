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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "../gui/gui.h"
#include "../../src/commands/settings/sgroup.h"

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

/* ---- the system settings (spec K.7) ----
 * settings GROUP: the group's screen, a Pine-style list (R5.6a.7): Up/Down
 * choose, Left/Right change the value (applied at once), OK on an action
 * row does it, Back leaves; /etc/flipper/GROUP.conf is written then, once,
 * if a saved value changed.  settings apply: every group's file applied
 * (the boot script).  The screens move to the desktop's window records
 * (K.8) later; the groups (src/commands/settings) stay as they are. */
static struct sgroup *groups[] = { &display_group, &sound_group };
#define NGROUPS	(sizeof(groups) / sizeof(groups[0]))

static struct sgroup *find(const char *name)
{
	unsigned i;

	for (i = 0; i < NGROUPS; i++)
		if (strcmp(groups[i]->name, name) == 0) return groups[i];
	return NULL;
}

static void conf_path(char *p, struct sgroup *g)
{
	strcpy(p, "/etc/flipper/");
	strcat(p, g->name);
	strcat(p, ".conf");
}

/* "Key: value" lines; a value is the number, or the label of an enum. */
static void conf_load(struct sgroup *g)
{
	char path[40], line[48], *v;
	struct srow *r;
	FILE *f;
	int i, k;

	conf_path(path, g);
	if ((f = cfopen(path, "r")) == NULL) return;
	while (fgets(line, sizeof line, f) != NULL) {
		line[strcspn(line, "\n")] = 0;
		if ((v = strchr(line, ':')) == NULL) continue;
		*v++ = 0;
		while (*v == ' ') v++;
		for (i = 0; i < g->nrows; i++) {
			r = &g->rows[i];
			if (strcmp(r->name, line) != 0 || (r->flags & S_NOSAVE))
				continue;
			if (r->type == S_INT) {
				k = atoi(v);
				if (k >= r->min && k <= r->max) r->val = k;
			} else if (r->type == S_ENUM)
				for (k = r->min; k <= r->max; k++)
					if (strcmp(r->labels[k], v) == 0) r->val = k;
		}
	}
	fclose(f);
}

static int conf_save(struct sgroup *g)
{
	char path[40];
	struct srow *r;
	FILE *f;
	int i;

	(void) mkdir("/etc/flipper", 0755);
	conf_path(path, g);
	if ((f = fopen(path, "w")) == NULL) return -1;
	for (i = 0; i < g->nrows; i++) {
		r = &g->rows[i];
		if (r->type == S_ACT || (r->flags & S_NOSAVE)) continue;
		if (r->type == S_INT) fprintf(f, "%s: %d\n", r->name, r->val);
		else fprintf(f, "%s: %s\n", r->name, r->labels[r->val]);
	}
	fclose(f);
	return 0;
}

static void draw(struct sgroup *g, int sel)
{
	char v[16];
	struct srow *r;
	int i, y;

	gui_clear();
	gui_title(g->title);
	for (i = 0; i < g->nrows; i++) {
		r = &g->rows[i];
		y = 7 + i * 6;
		if (i == sel) gui_fill(0, y - 1, GUI_W - 1, y + 5, 1);
		gui_text(6, y, r->name, i == sel);
		if (r->type == S_INT) sprintf(v, "[%d%s]", r->val, r->unit);
		else if (r->type == S_ENUM) sprintf(v, "[%s]", r->labels[r->val]);
		else strcpy(v, "");
		gui_text(GUI_W - 2 - (int) strlen(v) * 4, y, v, i == sel);
		if (i == sel) gui_text(1, y, ">", 1);
	}
	gui_hline(0, GUI_W - 1, 56, 1);
	gui_text(1, 58, g->rows[sel].type == S_ACT ? "OK Run  Back Done"
					: "<> Change  Back Done", 0);
	gui_flush();
}

static int screen(struct sgroup *g)
{
	struct gui_in in;
	int sel = 0, k, changed = 0, d;
	struct srow *r;

	if (gui_open() < 0) {
		fprintf(stderr, "settings: cannot open the screen\n");
		return 1;
	}
	conf_load(g);
	g->load(g);
	memset(&in, 0, sizeof in);
	for (;;) {
		draw(g, sel);
		k = gui_input(&in, KEY_LEFT | KEY_RIGHT, -1);
		r = &g->rows[sel];
		if (k == KEY_UP) sel = sel > 0 ? sel - 1 : g->nrows - 1;
		else if (k == KEY_DOWN) sel = sel < g->nrows - 1 ? sel + 1 : 0;
		else if (k == KEY_BACK) break;
		else if (k == KEY_OK && r->type == S_ACT) g->apply(g, sel);
		else if ((k == KEY_LEFT || k == KEY_RIGHT) && r->type != S_ACT) {
			d = k == KEY_LEFT ? -r->step : r->step;
			if (r->val + d < r->min || r->val + d > r->max) continue;
			r->val += d;
			g->apply(g, sel);
			if (!(r->flags & S_NOSAVE)) changed = 1;
		}
	}
	gui_close();
	if (changed && conf_save(g) < 0) {
		fprintf(stderr, "settings: cannot write /etc/flipper/%s.conf\n",
			g->name);
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int i, r = 0;
	struct sgroup *g;

	if (argc == 2 && strcmp(argv[1], "apply") == 0) {	/* at boot */
		for (i = 0; i < (int) NGROUPS; i++) {
			groups[i]->load(groups[i]);
			conf_load(groups[i]);
			groups[i]->apply(groups[i], -1);
		}
		return 0;
	}
	if (argc == 2 && (g = find(argv[1])) != NULL) return screen(g);

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
	fprintf(stderr, "Usage: settings diff | reset [path ...] | apply"
		" | display | sound\n");
	return 1;
}
