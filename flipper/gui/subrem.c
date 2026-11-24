/* SPDX-License-Identifier: GPL-3.0-or-later
 * subrem - the Sub-GHz Remote app (spec Phase 7, K.8; issue #424 item 1),
 * a window app: a saved map file ties up to five saved .sub files to the
 * five pad keys (Up, Down, Left, Right, OK); pressing a key sends that
 * file through the subghz command, so the kernel's region gate (R8.2,
 * #383, off by default) and the command's own words apply unchanged.
 *
 *	Sub-GHz Remote:nobody:@subghz:=/bin/subrem	(/etc/apps)
 *
 * Map files live in /home/flipper/subghz_remote, the stock format
 * (Unleashed's SubGHz_Remote, GPLv3):
 *	Filetype: Flipper SubRem Map file
 *	Version: 1
 *	UP: /home/flipper/subghz/gate.sub
 *	ULABEL: Gate
 *	DOWN: ...            (DLABEL, LEFT/LLABEL, RIGHT/RLABEL, OK/OKLABEL)
 * The xBUTTON lines of multi-button files are ignored: subghz sends the
 * file as it is.  The user drops their own .sub and map files on the card
 * (spec D.3: none shipped).
 *
 * RAM: no stdio, no malloc; opendir's DIR while the map list is read, the
 * five paths and labels and the command's last lines in .bss.  The subghz
 * command (16 KB CHMEM) runs only while a send is in flight.
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
#define DIRMAP	HOME "/subghz_remote"
#define PATHMAX	128
#define LABELMAX 24
#define NKEY	5		/* Up, Down, Left, Right, OK */

/* The pad keys, in the order of the map file's slots. */
static const char *const keyname[NKEY] = { "up", "down", "left", "right", "ok" };
static const char *const keytag[NKEY]  = { "UP", "DOWN", "LEFT", "RIGHT", "OK" };
static const char *const keylab[NKEY]  = { "Up", "Down", "Left", "Right", "OK" };

static struct run run;
static char mapfile[PATHMAX];			/* the open map's path */
static char path[NKEY][PATHMAX];		/* each slot's .sub, "" none */
static char label[NKEY][LABELMAX];		/* each slot's label */
static int lfirst;				/* the map list's first row */
static char top = 'm';				/* m(ain) r(emote) u(run) */

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

/* Entry i of the map-file list (or its count, i < 0) into out. */
static int entry(int i, char *out)
{
	DIR *d;
	struct dirent *e;
	int n = 0;

	if ((d = opendir(DIRMAP)) == NULL) return i < 0 ? 0 : -1;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.' || !ends(e->d_name, ".txt")) continue;
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

/* Trim leading spaces and a trailing newline in place. */
static char *trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t') s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '))
		*--e = 0;
	return s;
}

/* Read the map file into path[]/label[]; return the number of keys with a
 * file, or -1 if the file cannot be read. */
static int load_map(const char *file)
{
	int fd, i, got = 0;
	char buf[256], *p, *nl, *val;
	int len = 0, used = 0;

	for (i = 0; i < NKEY; i++) { path[i][0] = 0; label[i][0] = 0; }
	if ((fd = open(file, O_RDONLY)) < 0) return -1;
	/* read the whole small file, then walk it line by line */
	len = (int) read(fd, buf, sizeof buf - 1);
	close(fd);
	if (len < 0) return -1;
	buf[len] = 0;
	for (p = buf; p < buf + len; p = nl + 1) {
		nl = strchr(p, '\n');
		if (nl == NULL) nl = buf + len;
		else *nl = 0;
		val = strchr(p, ':');
		if (val == NULL) { if (nl == buf + len) break; continue; }
		*val++ = 0;
		val = trim(val);
		for (i = 0; i < NKEY; i++) {
			if (strcmp(p, keytag[i]) == 0) {
				strncpy(path[i], val, PATHMAX - 1);
				if (val[0]) got++;
			} else {
				char lt[8];	/* "ULABEL".."OKLABEL" */
				int n = (int) strlen(keytag[i]);

				if (n + 5 < (int) sizeof lt) {
					memcpy(lt, keytag[i], n);
					memcpy(lt + n, "LABEL", 6);
					if (strcmp(p, lt) == 0)
						strncpy(label[i], val, LABELMAX - 1);
				}
			}
		}
		if (nl == buf + len) break;
	}
	(void) used;
	return got;
}

/* ---- windows ---- */

static void w_main(void)
{
	char name[PATHMAX];
	int n, i;

	top = 'm';
	n = entry(-1, name);
	gm_win("main", "list", "Sub-GHz Remote", n, lfirst);
	if (n == 0)
		gm_line("No map in " DIRMAP);
	for (i = lfirst; i < lfirst + 8 && i < n; i++)
		if (entry(i, name) == 0) {
			name[strlen(name) - 4] = 0;	/* drop ".txt" */
			gm_item(name, name, "");
		}
	gm_show();
}

static void w_remote(void)
{
	int i;

	top = 'r';
	gm_win_pad("remote", "Remote", 0);
	gm_line(base(mapfile));
	for (i = 0; i < NKEY; i++) {
		char line[48];
		const char *l = label[i][0] ? label[i]
			      : path[i][0] ? base(path[i]) : "(none)";

		strcpy(line, keylab[i]);
		strcat(line, ": ");
		strncat(line, l, 40);
		gm_line(line);
	}
	if (gm_show() < 0) _exit(0);
}

static void w_run(const char *title)
{
	top = 'u';
	gm_win("run", "text", title, 0, 0);
	run_lines(&run);
	if (run.status > 0)
		gm_line("(not sent)");
	gm_show();
}

/* Send slot i's file, show the subghz command's words. */
static void send(int i)
{
	char *av[4];

	if (!path[i][0]) return;
	av[0] = "subghz";
	av[1] = "tx_from_file";
	av[2] = path[i];
	av[3] = NULL;
	(void) run_sync(&run, SUBGHZ, av);
	w_run(label[i][0] ? label[i] : keylab[i]);
}

static void open_map(const char *name)
{
	int got;

	strcpy(mapfile, DIRMAP "/");
	strncat(mapfile, name, PATHMAX - (int) strlen(mapfile) - 5);
	strcat(mapfile, ".txt");
	got = load_map(mapfile);
	if (got < 0) {
		gm_win("err", "text", "Sub-GHz Remote", 0, 0);
		gm_line("Cannot read the map file");
		gm_show();
		top = 'e';
		return;
	}
	w_remote();
}

static void redraw(void)
{
	switch (top) {
	case 'r': w_remote(); break;
	case 'u': w_run("Send"); break;
	default:  w_main(); break;
	}
}

int main(int argc, char **argv)
{
	char buf[160], ev[256], *f[GM_FIELDS];

	run.fd = -1;
	run.pid = 0;
	if (gm_init(buf, sizeof buf) < 0) {
		(void) write(2, "subrem: start Sub-GHz Remote from the launcher\n",
			     47);
		return 1;
	}
	if (argc > 1 && strlen(argv[1]) < PATHMAX) {	/* Archive's Open */
		strncpy(mapfile, argv[1], PATHMAX - 1);
		if (load_map(mapfile) >= 0) w_remote();
		else w_main();
	} else
		w_main();
	for (;;) {
		switch (gm_event(ev, sizeof ev, f, GM_FIELDS)) {
		case GM_OK:
			if (top == 'm' && f[2] != NULL) open_map(f[2]);
			break;
		case GM_KEY:
			if (top == 'r' && f[1] != NULL && f[2] != NULL
			    && strcmp(f[2], "press") == 0) {
				int i;

				for (i = 0; i < NKEY; i++)
					if (strcmp(f[1], keyname[i]) == 0) {
						send(i);
						break;
					}
			}
			break;
		case GM_PAGE:
			if (top == 'm' && f[2] != NULL) {
				lfirst = atoi(f[2]);
				w_main();
			}
			break;
		case GM_REDRAW:
			redraw();
			break;
		case GM_BACK:
			if (top == 'r' || top == 'e') w_main();
			else if (top == 'u') w_remote();
			break;
		case GM_CLOSE:
			return 0;
		}
	}
}
