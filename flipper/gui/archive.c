/* SPDX-License-Identifier: GPL-3.0-or-later
 * archive - the stock Archive (file browser) on Galtzagorriak MINIX (spec
 * Phase 7; issue #389), a window app (spec K.8): the launcher draws, the
 * app only describes, with the same TSV records as the other window apps.
 *
 * Tabs as in stock: Favourites, Infrared (.ir), Sub-GHz (.sub), 125 kHz
 * RFID (.rfid), NFC (.nfc), iButton (.ibtn), Text (.txt) and the Browser
 * (every file under /home/flipper, folders too).  OK on a file: its
 * actions: Open, Name (rename), Favourite, Delete.  Favourites are
 * /home/flipper/favorites.txt, one path a line (stock's favorites.txt).
 *
 * One swap-friendly list: a tab's files are never held in memory.  The
 * desktop asks for a page (8 rows, `page`) and the app reads just that
 * page from the directory; a list's length is one counting pass.
 *
 * Open: a .txt file is shown here (its first lines); the other types are
 * opened by their apps: a "run" record (issue #413) asks the launcher to
 * start the app of /etc/apps with that label (Infrared, Sub-GHz, RFID,
 * iButton) with the file as its argument; this app ends then.
 *
 *	Archive:nobody:@file:=/bin/archive	(/etc/apps)
 *
 * RAM: no stdio, no libfuri; opendir's 1 KB DIR while a list is read;
 * the paths of the current folder and file in .bss (2 x 128 B).
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>		/* rename() only: no stdio streams */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "gm.h"

#define HOME	"/home/flipper"
#define FAVS	HOME "/favorites.txt"
#define FAVTMP	HOME "/.favorites.tmp"
#define PATHMAX	128
#define PAGE	8			/* rows of a page */
#define NAMEMAX	40			/* longest name the keyboard edits */

static const struct tab {
	const char *key, *label, *dir, *ext;	/* ext NULL: every file */
	const char *app;			/* opens it, or NULL */
} tabs[] = {
	{ "fav", "Favourites", NULL, NULL, NULL },
	{ "ir", "Infrared", HOME "/infrared", ".ir", "Infrared" },
	{ "sub", "Sub-GHz", HOME "/subghz", ".sub", "Sub-GHz" },
	{ "rfid", "125 kHz RFID", HOME "/lfrfid", ".rfid", "RFID" },
	{ "nfc", "NFC", HOME "/nfc", ".nfc", NULL },
	{ "ibtn", "iButton", HOME "/ibutton", ".ibtn", "iButton" },
	{ "txt", "Text", HOME, ".txt", "Archive" },
	{ "all", "Browser", HOME, NULL, NULL },
};
#define NTABS	(int) (sizeof tabs / sizeof tabs[0])
#define T_FAV	0
#define T_ALL	(NTABS - 1)

static char dir[PATHMAX];		/* the folder listed */
static char file[PATHMAX];		/* the file whose actions are shown */
static int tab = -1;			/* the tab listed */
static int depth;			/* Browser: folders below HOME */
static char top;			/* window on top: m l f c t */
static int lfirst;			/* the list's page */

/* ---- small things ---- */

static int ends(const char *s, const char *ext)
{
	size_t l = strlen(s), e = strlen(ext);

	return l > e && strcmp(s + l - e, ext) == 0;
}

static int known(const char *name)	/* a type of a tab (not Browser) */
{
	int i;

	for (i = 1; i < T_ALL; i++) if (ends(name, tabs[i].ext)) return i;
	return -1;
}

static int isdir(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int join(char *out, const char *d, const char *name)
{
	if (strlen(d) + 1 + strlen(name) >= PATHMAX) return -1;
	strcpy(out, d);
	strcat(out, "/");
	strcat(out, name);
	return 0;
}

static const char *base(const char *p)
{
	const char *s = strrchr(p, '/');

	return s ? s + 1 : p;
}

/* ---- favourites: one path a line, read a byte at a time (no stdio) ---- */

/* Line i of the favourites into out; 0, or -1 when there is none.  With
 * i < 0: the number of lines. */
static int fav_line(int i, char *out)
{
	char c;
	int fd = open(FAVS, O_RDONLY), n = 0, k = 0;

	if (fd < 0) return i < 0 ? 0 : -1;
	while (read(fd, &c, 1) == 1) {
		if (c == '\n') {
			if (k > 0) {
				if (n == i) {
					out[k < PATHMAX ? k : PATHMAX - 1] = 0;
					close(fd);
					return 0;
				}
				n++;
			}
			k = 0;
			continue;
		}
		if (k < PATHMAX - 1 && out) out[k] = c;
		k++;
	}
	if (k > 0) {
		if (n == i) { out[k < PATHMAX ? k : PATHMAX - 1] = 0; close(fd); return 0; }
		n++;
	}
	close(fd);
	return i < 0 ? n : -1;
}

static int is_fav(const char *path)
{
	char l[PATHMAX];
	int i;

	for (i = 0; fav_line(i, l) == 0; i++) if (!strcmp(l, path)) return 1;
	return 0;
}

/* Remove path from the favourites (into a new file, then rename); with
 * to != NULL put that in its place (a rename). */
static void fav_drop(const char *path, const char *to)
{
	char l[PATHMAX];
	int i, fd = open(FAVTMP, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd < 0) return;
	for (i = 0; fav_line(i, l) == 0; i++) {
		if (!strcmp(l, path)) {
			if (!to) continue;
			strcpy(l, to);
		}
		(void) write(fd, l, strlen(l));
		(void) write(fd, "\n", 1);
	}
	close(fd);
	(void) rename(FAVTMP, FAVS);
}

static void fav_add(const char *path)
{
	int fd;

	if (is_fav(path)) return;
	if ((fd = open(FAVS, O_WRONLY | O_CREAT | O_APPEND, 0644)) < 0) return;
	(void) write(fd, path, strlen(path));
	(void) write(fd, "\n", 1);
	close(fd);
}

/* ---- a list's entries, read from the folder page by page ---- */

/* Does entry e of the folder belong to the list? */
static int wanted(const char *name)
{
	char p[PATHMAX];

	if (name[0] == '.') return 0;		/* ., .., hidden */
	if (tab == T_ALL) return 1;
	if (tabs[tab].ext == NULL || !ends(name, tabs[tab].ext)) return 0;
	return join(p, dir, name) == 0 && !isdir(p);
}

/* Entry i (or the count, i < 0) into out. */
static int entry(int i, char *out)
{
	DIR *d;
	struct dirent *e;
	int n = 0;

	if (tab == T_FAV) return fav_line(i, out);
	if ((d = opendir(dir)) == NULL) return i < 0 ? 0 : -1;
	while ((e = readdir(d)) != NULL) {
		if (!wanted(e->d_name)) continue;
		if (n++ == i) {
			strncpy(out, e->d_name, PATHMAX - 1);
			out[PATHMAX - 1] = 0;
			closedir(d);
			return 0;
		}
	}
	closedir(d);
	return i < 0 ? n : -1;
}

/* ---- windows ---- */

static void w_main(void)
{
	int i;

	gm_win("main", "list", "Archive", 0, 0);
	for (i = 0; i < NTABS; i++) gm_item(tabs[i].key, tabs[i].label, "");
	gm_show();
	top = 'm';
}

static void w_list(int first)
{
	char id[4], key[6], name[PATHMAX], p[PATHMAX], lab[34];
	const char *title;
	int n = entry(-1, NULL), i;

	id[0] = 'l';
	id[1] = (char) ('0' + depth % 10);	/* a folder deeper: a child */
	id[2] = 0;
	title = depth ? base(dir) : tabs[tab].label;
	if (first < 0 || first >= n) first = 0;
	lfirst = first;
	if (n == 0) {
		gm_win(id, "list", title, 0, 0);
		gm_line(tab == T_FAV ? "No favourites yet" : "No files");
		gm_show();
		top = 'l';
		return;
	}
	gm_win(id, "list", title, n, first);
	for (i = first; i < first + PAGE && i < n; i++) {
		if (entry(i, name) < 0) break;
		key[0] = 'f';
		key[1] = (char) ('0' + i / 1000 % 10);
		key[2] = (char) ('0' + i / 100 % 10);
		key[3] = (char) ('0' + i / 10 % 10);
		key[4] = (char) ('0' + i % 10);
		key[5] = 0;
		strncpy(lab, tab == T_FAV ? base(name) : name, sizeof lab - 1);
		lab[sizeof lab - 1] = 0;
		if (tab == T_ALL && join(p, dir, name) == 0 && isdir(p))
			gm_item(key, lab, ">");
		else gm_item(key, lab, "");
	}
	gm_show();
	top = 'l';
}

static void w_file(void)
{
	char name[NAMEMAX + 1];

	strncpy(name, base(file), NAMEMAX);
	name[NAMEMAX] = 0;
	gm_win("file", "list", name, 0, 0);
	gm_item("open", "Open", "");
	gm_input("name", "Name", name, NAMEMAX);
	gm_bool("fav", "Favourite", is_fav(file));
	gm_item("del", "Delete", "");
	gm_show();
	top = 'f';
}

static void w_confirm(void)
{
	gm_win("delq", "confirm", "Delete", 0, 0);
	gm_line(base(file));
	gm_action("yes", "Delete");
	gm_show();
	top = 'c';
}

static void w_note(const char *l1, const char *l2)
{
	gm_win("note", "text", base(file), 0, 0);
	gm_line(l1);
	if (l2) gm_line(l2);
	gm_show();
	top = 't';
}

/* A text file: its first lines, cut to the screen. */
static void w_text(void)
{
	char l[31], c;
	int fd = open(file, O_RDONLY), k = 0, n = 0;

	gm_win("text", "text", base(file), 0, 0);
	if (fd < 0) gm_line("cannot read the file");
	while (fd >= 0 && n < 12 && read(fd, &c, 1) == 1) {
		if (c == '\n' || k == 30) {
			l[k] = 0;
			gm_line(k ? l : " ");
			n++;
			k = 0;
			if (c == '\n') continue;
		}
		if (c == '\t') c = ' ';
		if ((unsigned char) c >= 0x20 && c != 0x7F) l[k++] = c;
	}
	if (k > 0 && n < 12) { l[k] = 0; gm_line(l); }
	if (fd >= 0) close(fd);
	gm_show();
	top = 't';
}

static void redraw(void)
{
	if (top == 'l') w_list(lfirst);
	else if (top == 'f' || top == 'c' || top == 't') w_file();
	else w_main();
}

/* ---- events ---- */

static void open_entry(int i)
{
	char name[PATHMAX], p[PATHMAX];

	if (entry(i, name) < 0) { w_list(lfirst); return; }
	if (tab == T_FAV) strcpy(p, name);
	else if (join(p, dir, name) < 0) { gm_busy(0); return; }
	if (tab == T_ALL && isdir(p)) {		/* into the folder */
		strcpy(dir, p);
		depth++;
		w_list(0);
		return;
	}
	strcpy(file, p);
	w_file();
}

static void do_open(void)
{
	int t = known(base(file));

	if (t == 6) { w_text(); return; }	/* .txt: here */
	if (t < 0 || tabs[t].app == NULL) {
		w_note("No app opens", "this type yet.");
		return;
	}
	if (gm_run(tabs[t].app, file) < 0) w_note("Open it in the app:",
						   tabs[t].app);
}

static void do_rename(const char *to)
{
	char p[PATHMAX], d[PATHMAX], *s;
	const char *ext = strrchr(base(file), '.');

	if (!to[0] || strchr(to, '/') || to[0] == '.') return;
	strcpy(d, file);
	if ((s = strrchr(d, '/')) == NULL) return;
	*s = 0;
	if (join(p, d, to) < 0) return;
	if (ext && !strrchr(to, '.') && strlen(p) + strlen(ext) < PATHMAX)
		strcat(p, ext);			/* keep the type */
	if (access(p, F_OK) == 0) return;	/* never over another file */
	if (rename(file, p) == 0) {
		if (is_fav(file)) fav_drop(file, p);
		strcpy(file, p);
	}
}

static void submit(char **f)
{
	char *k, *v;
	int i;

	for (i = 2; i < GM_FIELDS && f[i] != NULL; i++) {
		k = gm_kv(f[i], &v);
		if (!strcmp(k, "fav")) {
			if (v[0] == '1') fav_add(file);
			else fav_drop(file, NULL);
		} else if (!strcmp(k, "name")) do_rename(v);
	}
}

static void ok(const char *win, const char *key)
{
	int i;

	if (!strcmp(win, "main")) {
		for (i = 0; i < NTABS; i++) if (!strcmp(key, tabs[i].key)) break;
		if (i == NTABS) { gm_busy(0); return; }
		tab = i;
		depth = 0;
		strcpy(dir, tabs[i].dir ? tabs[i].dir : HOME);
		w_list(0);
	} else if (win[0] == 'l' && key[0] == 'f') open_entry(atoi(key + 1));
	else if (!strcmp(key, "open")) do_open();
	else if (!strcmp(key, "del")) w_confirm();
	else if (!strcmp(key, "yes")) {
		if (unlink(file) == 0) fav_drop(file, NULL);
		w_list(lfirst);			/* the folder again */
	} else gm_busy(0);
}

/* Back from win: the window under it is on top now. */
static void back(const char *win)
{
	char *s;

	if (win[0] == 'l') {
		if (depth > 0 && (s = strrchr(dir, '/')) != NULL) {
			*s = 0;
			depth--;
			top = 'l';
		} else top = 'm';
	} else if (!strcmp(win, "file")) top = 'l';
	else top = 'f';
}

int main(void)
{
	char buf[128], ev[128], *f[GM_FIELDS];

	if (gm_init(buf, sizeof buf) < 0) {
		(void) write(2, "archive: start it from the launcher\n", 36);
		return 1;
	}
	mkdir(HOME, 0755);
	w_main();
	for (;;) {
		switch (gm_event(ev, sizeof ev, f, GM_FIELDS)) {
		case GM_OK: ok(f[1], f[2]); break;
		case GM_SUBMIT: submit(f); break;
		case GM_BACK: back(f[1]); break;
		case GM_PAGE: w_list(atoi(f[2])); break;
		case GM_REDRAW: redraw(); break;
		case GM_CLOSE: return 0;
		}
	}
}
