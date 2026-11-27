/* SPDX-License-Identifier: GPL-3.0-or-later
 * ibuttonui - the stock iButton app on Galtzagorriak MINIX (spec R6.7;
 * issue #413, part 2 of the stock app ports), a window app (spec K.8).
 * A front end of the ikey command (/bin/ikey: /dev/ibutton, libibkey):
 * Read, Saved, Add manually, Write (RW1990, TM2004 and TM01x blanks, also
 * the memory of a DS1992, DS1996 or DS1971), Emulate and Info, with
 * ikey's own lines shown live (gui/runwin.c).  Every stock protocol:
 * DS1990, DS1992, DS1996, DS1971, DS1420, DSGeneric, Cyfral, Metakom
 * (issue #490).
 *
 *	iButton:nobody:@ibutton:=/bin/ibuttonui	(/etc/apps)
 *	ibuttonui FILE				(a .ibtn file's window)
 *
 * Files are stock's .ibtn in /home/flipper/ibutton: version 2
 * ("Protocol: DS1990" + "Rom Data:" [+ "Sram Data:" / "Eeprom Data:"],
 * "Protocol: Cyfral|Metakom" + "Data:") is written; version 1 ("Key type:"
 * + "Data:") is read too.  A key just read or added lives in a hidden
 * file (.last.ibtn: ikey read -o writes it, memory and all) until Save
 * renames it; so the memory of a DS1996 never passes through this
 * program.
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
#include "ibkey.h"

#define IKEY	"/bin/ikey"
#define HOME	"/home/flipper"
#define DIRIB	HOME "/ibutton"
#define PATHMAX	128
#define PAGE	8
#define NAMEMAX	24
#define KEYMAX	16			/* hex digits: Dallas 16, Cyfral 4, Metakom 8 */

#define NTYPES	8
#define NDS	6			/* types 0..5 are Dallas families */
#define TMPF	DIRIB "/.last.ibtn"
static const char *const types[NTYPES] = { "DS1990", "DS1992", "DS1996",
	"DS1971", "DS1420", "DSGeneric", "Cyfral", "Metakom" };
static const int digits[NTYPES] = { 16, 16, 16, 16, 16, 16, 4, 8 };
/* Add manually: a Dallas key is its serial (12 digits); the family code
 * and the CRC are filled in.  A generic key brings its family code too. */
static const int addn[NTYPES] = { 12, 12, 12, 12, 12, 14, 4, 8 };
static const char type_opts[] =
	"DS1990|DS1992|DS1996|DS1971|DS1420|DSGeneric|Cyfral|Metakom";

static struct run run;
static char file[PATHMAX];
static char name[NAMEMAX + 1] = "Key";
static char key[KEYMAX + 1];		/* the current key, hex */
static int type;			/* index in types[] */
static char tmpf[] = TMPF;
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

static int hexval(int c)
{
	return c <= '9' ? c - '0' : c - 'A' + 10;
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

			int t;

			for (t = 0; t < NTYPES; t++)
				if (!strcmp(p, types[t])) type = t;
			if (!strcmp(p, "Dallas")) type = 0;
			else if (!strcmp(p, "(non-specific)")) type = 5;
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
	gm_item("hook", "Hookup", "");
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

static int saved(void)
{
	return file[0] && strcmp(file, tmpf) != 0;
}

/* Is this a Dallas family with a memory to copy (DS1992, DS1996, DS1971)? */
static int hasmem(void)
{
	const struct ibd_fam *f;

	return type < NDS && (f = ibd_byname(types[type])) != NULL
		&& f->mem != IBD_NOMEM;
}

/* The current key's actions (a saved file, a read or an added key). */
static void w_key(void)
{
	gm_win("key", "list", saved() ? base(file) : types[type], 0, 0);
	gm_item("ty", types[type], key);
	gm_item("info", "Info", "");
	gm_item("emu", "Emulate", "");
	if (type < NDS) {
		gm_item("wr", "Write", "");
		gm_item("wt", "Write to TM01x", "");
		if (hasmem()) gm_item("wm", "Write memory", "");
	} else {
		gm_item("wt", "Write to TM01x", "");
	}
	if (saved()) gm_item("del", "Delete", "");
	else {
		gm_input("name", "Name", name, NAMEMAX);
		gm_action("save", "Save");
	}
	gm_show();
	top = 'k';
}

/* What the key is: family, memory, whether the CRC is right. */
static void w_info(void)
{
	static char l1[24], l2[24];
	const struct ibd_fam *f = type < NDS ? ibd_byname(types[type]) : NULL;
	unsigned char rom[8];
	int i;

	strcpy(l1, "Family ");
	if (f != NULL && type < NDS) {
		for (i = 0; i < 8; i++)
			rom[i] = (unsigned char) (hexval(key[2 * i]) << 4
						| hexval(key[2 * i + 1]));
		l1[7] = "0123456789ABCDEF"[rom[0] >> 4];
		l1[8] = "0123456789ABCDEF"[rom[0] & 15];
		strcpy(l1 + 9, ibd_crc8(rom, 7) == rom[7] ? " CRC ok" : " CRC BAD");
		if (f->mem == IBD_NOMEM) strcpy(l2, "ROM only");
		else {
			char d[6];
			int n = 0, v = f->size, j;

			strcpy(l2, f->mem == IBD_SRAM ? "SRAM " : "EEPROM ");
			for (j = 1000; j > 0; j /= 10)
				if (v >= j || n) d[n++] = (char) ('0' + v / j % 10);
			d[n] = 0;
			strcat(l2, d);
			strcat(l2, " B");
		}
	} else {
		strcpy(l1, type == 6 ? "Cyfral, 2 bytes" : "Metakom, 4 bytes");
		strcpy(l2, "Line code, no CRC");
	}
	w_note(types[type], l1, l2);
}

static void w_hook(void)
{
	gm_win("hook", "text", "iButton hookup", 0, 0);
	gm_line("Touch the key to the");
	gm_line("pad on the top edge.");
	gm_line("Or wire to GPIO:");
	gm_line("pin 17 = data");
	gm_line("pin 18 = GND");
	gm_show();
	top = 'h';
}

static void w_add(void)
{
	gm_win("add", "list", "Add manually", 0, 0);
	gm_enum("ty", "Protocol", type, type_opts);
	gm_input("key", addn[type] == 12 ? "Serial (hex)" : "Key (hex)", key,
		KEYMAX + 1);
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
	case 'h': w_hook(); break;
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

/* ikey read -o wrote TMPF (ROM, and the memory of a DS1992, DS1996 or
 * DS1971): it is the current key. */
static void keep(void)
{
	strcpy(file, tmpf);
	if (load() < 0) {
		file[0] = 0;
		w_note("Read", "Could not keep the key", "(SD card?)");
		return;
	}
	w_key();
}

static void put(int fd, const char *s)
{
	(void) write(fd, s, strlen(s));
}

#define Z4	" 00 00 00 00"
#define Z32	Z4 Z4 Z4 Z4 Z4 Z4 Z4 Z4

/* TMPF for the current type and key: a key added by hand.  A family with
 * memory gets it zeroed, as stock does.  0, or -1. */
static int mk_tmp(void)
{
	const struct ibd_fam *f = type < NDS ? ibd_byname(types[type]) : NULL;
	char pair[4];
	int fd, i;

	mkdir(HOME, 0755);
	mkdir(DIRIB, 0755);
	if ((fd = open(tmpf, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) return -1;
	put(fd, "Filetype: Flipper iButton key\nVersion: 2\nProtocol: ");
	put(fd, types[type]);
	put(fd, type < NDS ? "\nRom Data:" : "\nData:");
	for (i = 0; key[i] && key[i + 1]; i += 2) {
		pair[0] = ' '; pair[1] = key[i]; pair[2] = key[i + 1]; pair[3] = 0;
		put(fd, pair);
	}
	put(fd, "\n");
	if (f != NULL && f->mem != IBD_NOMEM) {
		put(fd, f->mem == IBD_SRAM ? "Sram Data:" : "Eeprom Data:");
		for (i = 0; i < f->size; i += 32) put(fd, Z32);
		put(fd, "\n");
	}
	return close(fd);
}

/* The key from the Add manually page: the digits typed into a whole ROM
 * (family code, serial, CRC).  0, or -1 (what to tell the user). */
static const char *complete(void)
{
	unsigned char rom[8], k4[IBK_MAXKEY];
	int i, n = type == 5 ? 7 : 6, o = type == 5 ? 0 : 1;

	if (!hexok(key, addn[type]))
		return type < NDS ? (type == 5 ? "needs 14 hex digits"
			: "needs 12 hex digits") : type == 6 ?
			"needs 4 hex digits" : "needs 8 hex digits";
	if (type >= NDS) {
		if (type == 7) {
			for (i = 0; i < 4; i++)
				k4[i] = (unsigned char) (hexval(key[2 * i]) << 4
							| hexval(key[2 * i + 1]));
			if (ibk_valid(IBK_METAKOM, k4) != 1)
				return "bytes need even parity";
		}
		return NULL;
	}
	rom[0] = ibd_byname(types[type])->code;
	for (i = 0; i < n; i++)
		rom[o + i] = (unsigned char) (hexval(key[2 * i]) << 4
						| hexval(key[2 * i + 1]));
	rom[7] = (unsigned char) ibd_crc8(rom, 7);
	for (i = 0; i < 8; i++) {
		key[2 * i] = "0123456789ABCDEF"[rom[i] >> 4];
		key[2 * i + 1] = "0123456789ABCDEF"[rom[i] & 15];
	}
	key[16] = 0;
	return NULL;
}

/* Save: the hidden file gets its name (a link, then the old name goes: an
 * existing file of that name is never overwritten). */
static void save(void)
{
	char dst[PATHMAX];

	if (!name[0] || strchr(name, '/') || name[0] == '.') {
		w_note("Save", "Bad name", NULL);
		return;
	}
	strcpy(dst, DIRIB "/");
	strcat(dst, name);
	strcat(dst, ".ibtn");
	if (link(tmpf, dst) < 0) {
		w_note("Save", "Cannot write (is there", "one of that name?)");
		return;
	}
	(void) unlink(tmpf);
	strcpy(file, dst);
	w_key();
}

static void values(char **f)
{
	char *k, *v;
	int i, x;

	for (i = 2; i < GM_FIELDS && f[i] != NULL; i++) {
		k = gm_kv(f[i], &v);
		x = atoi(v);
		if (!strcmp(k, "ty") && x >= 0 && x < NTYPES) type = x;
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
	static char *av_read[] = { "ikey", "read", "-o", TMPF, NULL };
	char *av[8];
	int n;

	if (!strcmp(win, "main")) {
		if (!strcmp(k, "read")) {
			mkdir(HOME, 0755);
			mkdir(DIRIB, 0755);
			start("Read", av_read);
		} else if (!strcmp(k, "saved")) w_list(0);
		else if (!strcmp(k, "hook")) w_hook();
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
		const char *bad = complete();

		if (bad != NULL) {
			w_note("Add manually", types[type], bad);
			return;
		}
		if (mk_tmp() < 0) {
			w_note("Add manually", "Cannot write", "(SD card?)");
			return;
		}
		strcpy(file, tmpf);
		w_key();
	} else if (!strcmp(win, "key") || !strcmp(win, "wtq")) {
		n = 0;
		av[n++] = "ikey";
		if (!strcmp(k, "emu")) {
			av[n++] = "emulate";
			av[n++] = (char *) types[type];
			av[n++] = key;
			av[n] = NULL;
			start("Emulate", av);
		} else if (!strcmp(k, "wr")) {
			av[n++] = "write";
			av[n++] = (char *) types[type];
			av[n++] = key;
			av[n] = NULL;
			start("Write", av);
		} else if (!strcmp(k, "wm")) {
			av[n++] = "copy";
			av[n++] = file;
			av[n] = NULL;
			start("Write memory", av);
		} else if (!strcmp(k, "wt") && type >= NDS) {
			gm_win("wtq", "confirm", "Write to TM01x", 0, 0);
			gm_line("A TM01x blank is");
			gm_line("finalized: it cannot");
			gm_line("be written again.");
			gm_action("yes", "Write");
			gm_show();
		} else if (!strcmp(k, "wt") || !strcmp(k, "yes")) {
			av[n++] = "write";
			if (type < NDS) {
				av[n++] = "-b";
				av[n++] = "tm01x";
			}
			av[n++] = (char *) types[type];
			av[n++] = key;
			av[n] = NULL;
			start("Write", av);
		} else if (!strcmp(k, "info")) w_info();
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
	} else if (!strcmp(win, "key")) top = saved() ? 'l' : 'm';
	else if (!strcmp(win, "delq") || !strcmp(win, "wtq")) top = 'k';
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
