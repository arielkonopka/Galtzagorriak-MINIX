/* SPDX-License-Identifier: GPL-3.0-or-later
 * subghzui - the stock Sub-GHz app on Galtzagorriak MINIX (spec Phase 9;
 * issue #413, part 2 of the stock app ports), a window app (spec K.8):
 * the launcher draws, the app describes.  A front end of the subghz
 * command (/usr/bin/subghz, the subghz package, libsubghz): the app runs
 * it with its output on a pipe and shows the lines live, so the decoded
 * protocol and key, and the kernel's refusal of a send (the region's
 * bands and duty cycle, R8.2, T32b), come with the command's own words.
 *
 *	Sub-GHz:nobody:@subghz:=/bin/subghzui	(/etc/apps)
 *	subghzui FILE				(a .sub file's window)
 *
 * Menu as stock: Read, Read RAW, Saved, Add manually, Frequency analyzer,
 * Radio settings.  Saved lists the .sub files of /home/flipper/subghz a page at a
 * time (a file is never read whole: subghz streams it).  Add manually
 * writes a stock Princeton key file.  Frequency and preset are the app's
 * (Radio settings), not kept after it ends (no new persistent state).
 *
 * RAM: no stdio, no malloc; opendir's 1 KB DIR while a list is read, the
 * command's last lines (struct run, ~260 B) and two paths in .bss.  The
 * subghz command itself (16 KB CHMEM) runs only while a window needs it.
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

#define SUBGHZ	"/usr/bin/subghz"
#define HOME	"/home/flipper"
#define DIRSUB	HOME "/subghz"
#define PATHMAX	128
#define PAGE	8
#define NAMEMAX	24

/* Stock's frequency list (subghz_setting.c, the common ones) and presets. */
static const unsigned long freqs[] = {
	300000000UL, 315000000UL, 390000000UL, 433075000UL, 433420000UL,
	433920000UL, 434420000UL, 868350000UL, 915000000UL
};
#define NFREQ	(int) (sizeof freqs / sizeof freqs[0])
static const char freq_opts[] =
	"300.00|315.00|390.00|433.07|433.42|433.92|434.42|868.35|915.00";
static const char *const presets[] = {
	"FuriHalSubGhzPresetOok270Async", "FuriHalSubGhzPresetOok650Async",
	"FuriHalSubGhzPreset2FSKDev238Async",
	"FuriHalSubGhzPreset2FSKDev476Async"
};
static const char preset_opts[] = "AM270|AM650|FM238|FM476";

static struct run run;
static char file[PATHMAX];		/* the file whose window is shown */
static char addkey[8] = "000000", addname[NAMEMAX + 1] = "Manual";
static int fi = 5, pi = 1;		/* 433.92 MHz, AM650 (stock's default) */
static int te = 400;
static int lfirst;
static char nfrom = 'm';		/* the note window's parent */
static char top = 'm';			/* m l f r(un) s(et) a(dd) n(ote) */
static const char *rtitle = "";		/* the run window's title */

static const char *base(const char *p)
{
	const char *s = strrchr(p, '/');

	return s ? s + 1 : p;
}

static char *ultoa(char *t, unsigned long v)	/* t: at least 12 bytes */
{
	char *p = t + 11;

	*p = 0;
	do *--p = (char) ('0' + v % 10); while ((v /= 10) != 0);
	return p;
}

static int ends(const char *s, const char *ext)
{
	size_t a = strlen(s), b = strlen(ext);

	return a > b && strcmp(s + a - b, ext) == 0;
}

/* Entry i of the saved list (or the count, i < 0) into out. */
static int entry(int i, char *out)
{
	DIR *d;
	struct dirent *e;
	int n = 0;

	if ((d = opendir(DIRSUB)) == NULL) return i < 0 ? 0 : -1;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.' || !ends(e->d_name, ".sub")) continue;
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
	gm_win("main", "list", "Sub-GHz", 0, 0);
	gm_item("read", "Read", "");
	gm_item("raw", "Read RAW", "");
	gm_item("saved", "Saved", "");
	gm_item("add", "Add manually", "");
	gm_item("fa", "Frequency analyzer", "");
	gm_item("set", "Radio settings", "");
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
	gm_win_live("run", "text", rtitle, 1);
	run_lines(&run);
	if (run.pid == 0)
		gm_line(run.status == 0 ? "-- done" : "-- failed");
	gm_show();
	top = 'r';
}

static void live(void)
{
	if (top == 'r') w_run();
}

static void w_list(int first)
{
	char key[6], name[PATHMAX], lab[34];
	int n = entry(-1, NULL), i;

	if (first < 0 || first >= n) first = 0;
	lfirst = first;
	top = 'l';
	if (n == 0) {
		gm_win("list", "list", "Saved", 0, 0);
		gm_line("No .sub files in");
		gm_line(DIRSUB);
		gm_show();
		return;
	}
	gm_win("list", "list", "Saved", n, first);
	for (i = first; i < first + PAGE && i < n; i++) {
		if (entry(i, name) < 0) break;
		key[0] = 'f';
		key[1] = (char) ('0' + i / 1000 % 10);
		key[2] = (char) ('0' + i / 100 % 10);
		key[3] = (char) ('0' + i / 10 % 10);
		key[4] = (char) ('0' + i % 10);
		key[5] = 0;
		strncpy(lab, name, sizeof lab - 1);
		lab[sizeof lab - 1] = 0;
		if (ends(lab, ".sub")) lab[strlen(lab) - 4] = 0;
		gm_item(key, lab, "");
	}
	gm_show();
}

static void w_file(void)
{
	gm_win("file", "list", base(file), 0, 0);
	gm_item("send", "Send", "");
	gm_item("info", "Info (decode)", "");
	gm_item("del", "Delete", "");
	gm_show();
	top = 'f';
}

static void w_set(void)
{
	static char *const av[] = { "subghz", "region", NULL };
	struct run r;

	(void) run_sync(&r, SUBGHZ, av);
	gm_win("set", "list", "Radio settings", 0, 0);
	gm_enum("fq", "Frequency", fi, freq_opts);
	gm_enum("mod", "Modulation", pi, preset_opts);
	gm_item("reg", "Region", r.n > 0 ? r.l[0] : "?");
	gm_show();
	top = 's';
}

static void w_add(void)
{
	gm_win("add", "list", "Add manually", 0, 0);
	gm_item("proto", "Protocol", "Princeton");
	gm_input("key", "Key (hex)", addkey, 7);
	gm_int("te", "TE", 100, 1200, 10, te, "us");
	gm_enum("fq", "Frequency", fi, freq_opts);
	gm_input("name", "Name", addname, NAMEMAX);
	gm_action("save", "Save");
	gm_show();
	top = 'a';
}

static void redraw(void)
{
	switch (top) {
	case 'l': w_list(lfirst); break;
	case 'f': w_file(); break;
	case 'r': w_run(); break;
	case 's': w_set(); break;
	case 'a': w_add(); break;
	default: w_main();
	}
}

/* ---- actions ---- */

static void start(const char *title, char *const *av)
{
	rtitle = title;
	(void) run_start(&run, SUBGHZ, av);
	w_run();
}

static void do_read(void)
{
	char t[12];
	static char hz[12];
	char *av[4];

	strcpy(hz, ultoa(t, freqs[fi]));
	av[0] = "subghz"; av[1] = "rx"; av[2] = hz; av[3] = NULL;
	start("Read", av);
}

static void do_file(const char *what)
{
	char *av[4];

	av[0] = "subghz";
	av[1] = (char *) what;
	av[2] = file;
	av[3] = NULL;
	start(what[0] == 'd' ? "Info" : "Send", av);
}

static int hexkey(const char *s, unsigned long *k)
{
	char *e;

	if (!*s || strlen(s) > 6) return 0;
	*k = strtoul(s, &e, 16);
	return *e == 0;
}

static void put(int fd, const char *s)
{
	(void) write(fd, s, strlen(s));
}

/* A stock Princeton key file (Filetype: Flipper SubGhz Key File). */
static void do_save(void)
{
	static const char hx[] = "0123456789ABCDEF";
	char t[12], kb[24];
	unsigned long k;
	int fd, i;

	if (!hexkey(addkey, &k)) {
		w_note("Add manually", "Key: 1 to 6 hex digits", "(24 bits)");
		return;
	}
	if (!addname[0] || strchr(addname, '/') || addname[0] == '.'
	    || strlen(DIRSUB) + strlen(addname) + 6 > PATHMAX) {
		w_note("Add manually", "Bad name", NULL);
		return;
	}
	mkdir(HOME, 0755);
	mkdir(DIRSUB, 0755);
	strcpy(file, DIRSUB "/");
	strcat(file, addname);
	strcat(file, ".sub");
	if (access(file, F_OK) == 0) {
		w_note("Add manually", "A file of that name", "is there already");
		return;
	}
	if ((fd = open(file, O_WRONLY | O_CREAT | O_EXCL, 0644)) < 0) {
		w_note("Add manually", "Cannot write", file);
		return;
	}
	for (i = 0; i < 8; i++) {		/* 8 bytes, the key in the low 3 */
		unsigned b = i < 5 ? 0 : (unsigned) (k >> (8 * (7 - i))) & 0xFF;

		kb[i * 3] = hx[b >> 4];
		kb[i * 3 + 1] = hx[b & 15];
		kb[i * 3 + 2] = i < 7 ? ' ' : '\n';
	}
	kb[23] = 0;
	put(fd, "Filetype: Flipper SubGhz Key File\nVersion: 1\nFrequency: ");
	put(fd, ultoa(t, freqs[fi]));
	put(fd, "\nPreset: ");
	put(fd, presets[pi]);
	put(fd, "\nProtocol: Princeton\nBit: 24\nKey: ");
	put(fd, kb);
	put(fd, "\nTE: ");
	put(fd, ultoa(t, (unsigned long) te));
	put(fd, "\n");
	if (close(fd) < 0) { w_note("Add manually", "Write failed", NULL); return; }
	w_file();
}

static void values(char **f)
{
	char *k, *v;
	int i, x;

	for (i = 2; i < GM_FIELDS && f[i] != NULL; i++) {
		k = gm_kv(f[i], &v);
		x = atoi(v);
		if (!strcmp(k, "fq") && x >= 0 && x < NFREQ) fi = x;
		else if (!strcmp(k, "mod") && x >= 0 && x < 4) pi = x;
		else if (!strcmp(k, "te") && x >= 100 && x <= 1200) te = x;
		else if (!strcmp(k, "key")) {
			strncpy(addkey, v, sizeof addkey - 1);
			addkey[sizeof addkey - 1] = 0;
		} else if (!strcmp(k, "name")) {
			strncpy(addname, v, NAMEMAX);
			addname[NAMEMAX] = 0;
		}
	}
}

static void ok(const char *win, const char *key)
{
	if (!strcmp(win, "main")) {
		if (!strcmp(key, "read")) do_read();
		else if (!strcmp(key, "raw"))
			w_note("Read RAW", "Recording RAW needs subghz",
				"rx_raw (not in the CLI yet)");
		else if (!strcmp(key, "saved")) w_list(0);
		else if (!strcmp(key, "add")) w_add();
		else if (!strcmp(key, "fa"))
			w_note("Frequency analyzer",
				"Open Spectrum from the", "launcher (Sub-GHz scan)");
		else if (!strcmp(key, "set")) w_set();
		else gm_busy(0);
	} else if (!strcmp(win, "list") && key[0] == 'f') {
		char name[PATHMAX];

		if (entry(atoi(key + 1), name) < 0
		    || strlen(DIRSUB) + strlen(name) + 2 > PATHMAX) {
			w_list(lfirst);
			return;
		}
		strcpy(file, DIRSUB "/");
		strcat(file, name);
		w_file();
	} else if (!strcmp(win, "file")) {
		if (!strcmp(key, "send")) do_file("tx_from_file");
		else if (!strcmp(key, "info")) do_file("decode");
		else if (!strcmp(key, "del")) {
			gm_win("delq", "confirm", "Delete", 0, 0);
			gm_line(base(file));
			gm_action("yes", "Delete");
			gm_show();
		} else gm_busy(0);
	} else if (!strcmp(win, "delq") && !strcmp(key, "yes")) {
		(void) unlink(file);
		w_list(lfirst);
	} else if (!strcmp(win, "add") && !strcmp(key, "save")) do_save();
	else gm_busy(0);
}

/* Back from win: the desktop shows its parent. */
static void back(const char *win)
{
	if (!strcmp(win, "note")) { top = nfrom; return; }
	if (!strcmp(win, "run")) {
		run_stop(&run);
		top = rtitle[0] == 'R' ? 'm' : 'f';
	} else if (!strcmp(win, "file")) top = file[0] ? 'l' : 'm';
	else if (!strcmp(win, "delq")) top = 'f';
	else top = 'm';
}

int main(int argc, char **argv)
{
	char buf[128], ev[128], *f[GM_FIELDS];

	if (gm_init(buf, sizeof buf) < 0) {
		(void) write(2, "subghzui: start it from the launcher\n", 37);
		return 1;
	}
	run.fd = -1;
	if (argc > 1 && strlen(argv[1]) < PATHMAX) {	/* Archive's Open */
		strcpy(file, argv[1]);
		w_file();
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
