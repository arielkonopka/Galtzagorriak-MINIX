/* SPDX-License-Identifier: GPL-3.0-or-later
 * ibuttonui - the stock iButton app on Galtzagorriak MINIX (spec R6.7;
 * issue #413, part 2 of the stock app ports), a window app (spec K.8).
 * A front end of the ikey command (/bin/ikey: /dev/ibutton, libibkey):
 * Read, Saved, Add manually, Write (RW1990 blanks, Dallas) and Emulate,
 * with ikey's own lines shown live (gui/runwin.c).
 *
 *	iButton:nobody:@ibutton:=/bin/ibuttonui	(/etc/apps)
 *	ibuttonui FILE				(a .ibtn file's window)
 *
 * Files are stock's .ibtn in /home/flipper/ibutton: version 2
 * ("Protocol: DS1990" + "Rom Data:", "Protocol: Cyfral|Metakom" +
 * "Data:") is written; version 1 ("Key type:" + "Data:") is read too.
 *
 * RAM: no stdio, no malloc; opendir's 1 KB DIR while the list is read,
 * ikey's last lines (struct run, ~260 B), a path and the key in .bss.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "gm.h"
#include "runwin.h"

#define IKEY	"/bin/ikey"
#define HOME	"/home/flipper"
#define DIRIB	HOME "/ibutton"
#define PATHMAX	128
#define PAGE	8
#define NAMEMAX	24
#define KEYMAX	16			/* hex digits: Dallas 16, Cyfral 4, Metakom 8 */

static const char *const types[] = { "Dallas", "Cyfral", "Metakom" };
static const int digits[] = { 16, 4, 8 };
static const char type_opts[] = "Dallas|Cyfral|Metakom";

static struct run run;
static char file[PATHMAX];
static char name[NAMEMAX + 1] = "Key";
static char key[KEYMAX + 1];		/* the current key, hex */
static int type;			/* index in types[] */
static int lfirst;
static char nfrom = 'm';		/* the note window's parent */
static char top = 'm';			/* m l f k(ey) a(dd) r(un) n(ote) */
static char from = 'm';			/* the run window's parent */
static const char *rtitle = "";

static const char *base(const char *p)
{
	const char *s = strrchr(p, '/');

	return s ? s + 1 : p;
}

static int ends(const char *s, const char *ext)
{
	size_t a = strlen(s), b = strlen(ext);

	return a > b && strcmp(s + a - b, ext) == 0;
}

static int hexok(const char *s, int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'F')))
			return 0;
	return s[n] == 0;
}

static void upper(char *s)
{
	for (; *s; s++) if (*s >= 'a' && *s <= 'f') *s -= 'a' - 'A';
}

static int entry(int i, char *out)
{
	DIR *d;
	struct dirent *e;
	int n = 0;

	if ((d = opendir(DIRIB)) == NULL) return i < 0 ? 0 : -1;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.' || !ends(e->d_name, ".ibtn")) continue;
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

/* The hex digits after "Tag: " into key (spaces dropped). */
static void take(const char *v)
{
	int n = 0;

	for (; *v && *v != '\n' && n < KEYMAX; v++)
		if (*v != ' ') key[n++] = *v;
	key[n] = 0;
	upper(key);
}

/* Read file's type and key (small: the first 384 bytes).  0, or -1. */
static int load(void)
{
	char b[385], *l, *nl;
	int fd, n;

	type = -1;
	key[0] = 0;
	if ((fd = open(file, O_RDONLY)) < 0) return -1;
	n = read(fd, b, sizeof b - 1);
	close(fd);
	if (n <= 0) return -1;
	b[n] = 0;
	for (l = b; l && *l; l = nl) {
		if ((nl = strchr(l, '\n')) != NULL) *nl++ = 0;
		if (!strncmp(l, "Protocol: ", 10) || !strncmp(l, "Key type: ", 10)) {
			const char *p = l + 10;

			if (!strncmp(p, "DS", 2) || !strcmp(p, "Dallas")) type = 0;
			else if (!strcmp(p, "Cyfral")) type = 1;
			else if (!strcmp(p, "Metakom")) type = 2;
		} else if (!strncmp(l, "Rom Data: ", 10)) take(l + 10);
		else if (!strncmp(l, "Data: ", 6)) take(l + 6);
	}
	if (type < 0 || !hexok(key, digits[type])) { type = 0; return -1; }
	return 0;
}

/* ---- windows ---- */

static void w_main(void)
{
	gm_win("main", "list", "iButton", 0, 0);
	gm_item("read", "Read", "");
	gm_item("saved", "Saved", "");
	gm_item("add", "Add manually", "");
	gm_show();
	top = 'm';
}

static void w_note(const char *t, const char *l1, const char *l2)
{
	gm_win("note", "text", t, 0, 0);
	gm_line(l1);
	if (l2) gm_line(l2);
	gm_show();
	if (top != 'n') nfrom = top;
	top = 'n';
}

static void w_run(void)
{
	gm_win_live("run", "list", rtitle, 1);
	run_lines(&run);
	if (run.pid == 0) {
		gm_line(run.status == 0 ? "-- done" : "-- failed");
		if (rtitle[0] == 'R' && run.status == 0)	/* Read: a key */
			gm_action("keep", "Use this key");
	}
	gm_show();
	top = 'r';
}

static void live(void)
{
	if (top == 'r') w_run();
}

static void w_list(int first)
{
	char k[6], nm[PATHMAX], lab[34];
	int n = entry(-1, NULL), i;

	if (first < 0 || first >= n) first = 0;
	lfirst = first;
	top = 'l';
	if (n == 0) {
		gm_win("list", "list", "Saved", 0, 0);
		gm_line("No .ibtn files in");
		gm_line(DIRIB);
		gm_show();
		return;
	}
	gm_win("list", "list", "Saved", n, first);
	for (i = first; i < first + PAGE && i < n; i++) {
		if (entry(i, nm) < 0) break;
		k[0] = 'f';
		k[1] = (char) ('0' + i / 1000 % 10);
		k[2] = (char) ('0' + i / 100 % 10);
		k[3] = (char) ('0' + i / 10 % 10);
		k[4] = (char) ('0' + i % 10);
		k[5] = 0;
		strncpy(lab, nm, sizeof lab - 1);
		lab[sizeof lab - 1] = 0;
		if (ends(lab, ".ibtn")) lab[strlen(lab) - 5] = 0;
		gm_item(k, lab, "");
	}
	gm_show();
}

/* The current key's actions (a saved file, a read or an added key). */
static void w_key(void)
{
	gm_win("key", "list", file[0] ? base(file) : types[type], 0, 0);
	gm_item("ty", types[type], key);
	gm_item("emu", "Emulate", "");
	if (type == 0) gm_item("wr", "Write", "");
	if (file[0]) gm_item("del", "Delete", "");
	else {
		gm_input("name", "Name", name, NAMEMAX);
		gm_action("save", "Save");
	}
	gm_show();
	top = 'k';
}

static void w_add(void)
{
	gm_win("add", "list", "Add manually", 0, 0);
	gm_enum("ty", "Protocol", type, type_opts);
	gm_input("key", "Key (hex)", key, KEYMAX + 1);
	gm_action("ok", "Next");
	gm_show();
	top = 'a';
}

static void redraw(void)
{
	switch (top) {
	case 'l': w_list(lfirst); break;
	case 'k': w_key(); break;
	case 'a': w_add(); break;
	case 'r': w_run(); break;
	default: w_main();
	}
}

/* ---- actions ---- */

static void start(const char *title, char *const *av)
{
	rtitle = title;
	from = top;
	(void) run_start(&run, IKEY, av);
	w_run();
}

/* "Dallas 0123456789ABCDEF" (ikey read's last line) into type and key. */
static void keep(void)
{
	const char *l = run_last(&run), *sp = strchr(l, ' ');
	int t;

	for (t = 0; t < 3; t++)
		if (sp && (size_t) (sp - l) == strlen(types[t])
		    && !strncmp(l, types[t], sp - l)) break;
	if (t == 3 || strlen(sp + 1) > KEYMAX) { gm_busy(0); return; }
	type = t;
	strcpy(key, sp + 1);
	file[0] = 0;
	w_key();
}

static void put(int fd, const char *s)
{
	(void) write(fd, s, strlen(s));
}

static void save(void)
{
	int fd, i;
	char pair[4];

	if (!name[0] || strchr(name, '/') || name[0] == '.') {
		w_note("Save", "Bad name", NULL);
		return;
	}
	mkdir(HOME, 0755);
	mkdir(DIRIB, 0755);
	strcpy(file, DIRIB "/");
	strcat(file, name);
	strcat(file, ".ibtn");
	if ((fd = open(file, O_WRONLY | O_CREAT | O_EXCL, 0644)) < 0) {
		w_note("Save", "Cannot write (is there", "one of that name?)");
		file[0] = 0;
		return;
	}
	put(fd, "Filetype: Flipper iButton key\nVersion: 2\nProtocol: ");
	put(fd, type == 0 ? "DS1990" : types[type]);
	put(fd, type == 0 ? "\nRom Data:" : "\nData:");
	for (i = 0; key[i] && key[i + 1]; i += 2) {
		pair[0] = ' '; pair[1] = key[i]; pair[2] = key[i + 1]; pair[3] = 0;
		put(fd, pair);
	}
	put(fd, "\n");
	if (close(fd) < 0) w_note("Save", "Write failed", NULL);
	else w_key();
}

static void values(char **f)
{
	char *k, *v;
	int i, x;

	for (i = 2; i < GM_FIELDS && f[i] != NULL; i++) {
		k = gm_kv(f[i], &v);
		x = atoi(v);
		if (!strcmp(k, "ty") && x >= 0 && x < 3) type = x;
		else if (!strcmp(k, "key")) {
			strncpy(key, v, KEYMAX);
			key[KEYMAX] = 0;
			upper(key);
		} else if (!strcmp(k, "name")) {
			strncpy(name, v, NAMEMAX);
			name[NAMEMAX] = 0;
		}
	}
}

static void ok(const char *win, const char *k)
{
	static char *av_read[] = { "ikey", "read", NULL };
	char *av[5];

	if (!strcmp(win, "main")) {
		if (!strcmp(k, "read")) start("Read", av_read);
		else if (!strcmp(k, "saved")) w_list(0);
		else if (!strcmp(k, "add")) {
			file[0] = 0;
			type = 0;
			key[0] = 0;
			w_add();
		} else gm_busy(0);
	} else if (!strcmp(win, "list") && k[0] == 'f') {
		char nm[PATHMAX];

		if (entry(atoi(k + 1), nm) < 0
		    || strlen(DIRIB) + strlen(nm) + 2 > PATHMAX) {
			w_list(lfirst);
			return;
		}
		strcpy(file, DIRIB "/");
		strcat(file, nm);
		if (load() < 0) w_note(nm, "Not an iButton key file", NULL);
		else w_key();
	} else if (!strcmp(win, "add") && !strcmp(k, "ok")) {
		if (!hexok(key, digits[type])) {
			w_note("Add manually", types[type], type == 0 ?
				"needs 16 hex digits" : type == 1 ?
				"needs 4 hex digits" : "needs 8 hex digits");
			return;
		}
		w_key();
	} else if (!strcmp(win, "key")) {
		av[0] = "ikey";
		av[2] = (char *) types[type];
		av[3] = key;
		av[4] = NULL;
		if (!strcmp(k, "emu")) { av[1] = "emulate"; start("Emulate", av); }
		else if (!strcmp(k, "wr")) { av[1] = "write"; start("Write", av); }
		else if (!strcmp(k, "save")) save();
		else if (!strcmp(k, "del")) {
			gm_win("delq", "confirm", "Delete", 0, 0);
			gm_line(base(file));
			gm_action("yes", "Delete");
			gm_show();
		} else gm_busy(0);
	} else if (!strcmp(win, "delq") && !strcmp(k, "yes")) {
		(void) unlink(file);
		file[0] = 0;
		w_list(lfirst);
	} else if (!strcmp(win, "run") && !strcmp(k, "keep")) keep();
	else gm_busy(0);
}

static void back(const char *win)
{
	if (!strcmp(win, "note")) { top = nfrom; return; }
	if (!strcmp(win, "run")) {
		run_stop(&run);
		top = from;
	} else if (!strcmp(win, "key")) top = file[0] ? 'l' : 'm';
	else if (!strcmp(win, "delq")) top = 'k';
	else top = 'm';
}

int main(int argc, char **argv)
{
	char buf[128], ev[128], *f[GM_FIELDS];

	if (gm_init(buf, sizeof buf) < 0) {
		(void) write(2, "ibuttonui: start it from the launcher\n", 38);
		return 1;
	}
	run.fd = -1;
	if (argc > 1 && strlen(argv[1]) < PATHMAX) {	/* Archive's Open */
		strcpy(file, argv[1]);
		if (load() < 0) w_note(base(file), "Not an iButton key file", NULL);
		else w_key();
	} else w_main();
	for (;;) {
		switch (run_event(&run, ev, sizeof ev, f, GM_FIELDS, live)) {
		case GM_OK: if (f[1] && f[2]) ok(f[1], f[2]); break;
		case GM_SUBMIT: values(f); break;
		case GM_BACK: if (f[1]) back(f[1]); break;
		case GM_PAGE:
			if (f[1] && f[2] && !strcmp(f[1], "list"))
				w_list(atoi(f[2]));
			else redraw();
			break;
		case GM_REDRAW: redraw(); break;
		case GM_CLOSE: run_stop(&run); return 0;
		}
	}
}
