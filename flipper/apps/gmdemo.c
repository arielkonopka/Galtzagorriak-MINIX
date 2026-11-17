/* gmdemo - an example window app (spec K.8): the launcher draws, the app
 * only describes.  No libfuri, no Canvas, no gui thread, no stdio: libgm
 * builds the records in a buffer on the stack and the app sleeps in
 * read() until the person does something that needs it.
 *
 *	gmdemo [-l file] [-n]
 *
 * -l: append every event it gets to file (the QEMU test reads it);
 * -n: show the first window and never read an event again (a deaf app:
 * the desktop must stay usable, mark it busy and close it on Back; the
 * SIGTERM it gets 2 s later is noted in the -l file).
 *
 * Start it from the launcher with "=" before the program in /etc/apps:
 *
 *	Demo:nobody:@app:=/usr/bin/gmdemo
 *
 * Build: link gm.o (src/lib/gm) statically; see flipper/minix.mk.
 */
#include <sys/types.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "gm.h"

#define ROWS	40		/* the long list */

/* The settings this demo edits: in a real app, a file (cfopen()). */
static short vol = 40, tone = 1, vib = 1, at = 7 * 60 + 30, days = 31;
static short year = 2026, month = 11, day = 17;
static char name[21] = "Wake up";
static char cur;		/* the window shown: m(ain) l(ong) a c q */
static int logfd = -1;

static void say(const char *a, const char *b)
{
	if (logfd < 0) return;
	(void) write(logfd, a, strlen(a));
	if (b != NULL) {
		(void) write(logfd, " ", 1);
		(void) write(logfd, b, strlen(b));
	}
	(void) write(logfd, "\n", 1);
}

static void w_main(void)
{
	gm_win("main", "list", "Demo", 0, 0);
	gm_int("vol", "Volume", 0, 100, 5, vol, "%");
	gm_enum("tone", "Tone", tone, "classic|rise|pips");
	gm_bool("vib", "Vibro", vib);
	gm_time("at", "Time", at / 60, at % 60);
	gm_date("day", "Date", year, month, day);
	gm_days("days", "Days", days);
	gm_input("name", "Name", name, 20);
	gm_item("clock", "Desk clock", "");
	gm_item("long", "Long list", "40");
	gm_item("about", "About", "");
	gm_action("save", "Save");
	gm_show();
	cur = 'm';
}

static void w_long(int first)
{
	char key[8], label[12];
	int i;

	if (first < 0 || first >= ROWS) first = 0;
	gm_win("long", "list", "Long list", ROWS, first);
	for (i = first; i < first + 16 && i < ROWS; i++) {
		key[0] = 'r';
		key[1] = (char) ('0' + i / 10);
		key[2] = (char) ('0' + i % 10);
		key[3] = 0;
		strcpy(label, "Row ");
		strcat(label, key + 1);
		gm_item(key, label, "");
	}
	gm_show();
	cur = 'l';
}

static void w_about(void)
{
	gm_win("about", "text", "About", 0, 0);
	gm_line("A window app (spec K.8).");
	gm_line("The launcher draws this");
	gm_line("text, moves the cursor and");
	gm_line("changes the values; the app");
	gm_line("only describes its screens");
	gm_line("and sleeps until it is needed.");
	gm_show();
	cur = 'a';
}

static void w_clock(void)
{
	char t[16];

	t[0] = 'N'; t[1] = 'e'; t[2] = 'x'; t[3] = 't'; t[4] = ' ';
	t[5] = (char) ('0' + at / 600); t[6] = (char) ('0' + at / 60 % 10);
	t[7] = ':';
	t[8] = (char) ('0' + at % 60 / 10); t[9] = (char) ('0' + at % 10);
	t[10] = 0;
	gm_win("clock", "clock", "Desk clock", 0, 0);
	gm_line(t);
	gm_line(name);
	gm_show();
	cur = 'c';
}

static void w_confirm(void)
{
	gm_win("save", "confirm", "Save", 0, 0);
	gm_line("Save the settings?");
	gm_action("yes", "Save");
	gm_show();
	cur = 'q';
}

/* -n: a SIGTERM from the desktop (2 s after Back) is noted */
static void term(int sig)
{
	(void) sig;
	say("term", NULL);
	_exit(0);
}

static int num(const char *s)
{
	return atoi(s);
}

/* submit: the values that changed */
static void values(char **f, int n)
{
	char *k, *v;
	int i;

	for (i = 2; i < n; i++) {
		say("value", f[i]);
		k = gm_kv(f[i], &v);
		if (strcmp(k, "vol") == 0) vol = (short) num(v);
		else if (strcmp(k, "tone") == 0) tone = (short) num(v);
		else if (strcmp(k, "vib") == 0) vib = (short) num(v);
		else if (strcmp(k, "days") == 0) days = (short) num(v);
		else if (strcmp(k, "at") == 0 && strlen(v) == 5)
			at = (short) (num(v) * 60 + num(v + 3));
		else if (strcmp(k, "day") == 0 && strlen(v) == 10) {
			year = (short) num(v);
			month = (short) num(v + 5);
			day = (short) num(v + 8);
		} else if (strcmp(k, "name") == 0) {
			strncpy(name, v, sizeof name - 1);
			name[sizeof name - 1] = 0;
		}
	}
}

int main(int argc, char **argv)
{
	char buf[128], ev[128], *f[GM_FIELDS];
	int i, deaf = 0, n;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-n") == 0) deaf = 1;
		else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc)
			logfd = open(argv[++i], O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
	if (gm_init(buf, sizeof buf) < 0) {
		(void) write(2, "gmdemo: start it from the launcher\n", 35);
		return 1;
	}
	say("start", NULL);
	w_main();
	if (deaf) {
		signal(SIGTERM, term);
		for (;;) pause();
	}
	for (;;) {
		switch (gm_event(ev, sizeof ev, f, GM_FIELDS)) {
		case GM_OK:
			say("ok", f[2]);
			if (strcmp(f[2], "clock") == 0) w_clock();
			else if (strcmp(f[2], "long") == 0) w_long(0);
			else if (strcmp(f[2], "about") == 0) w_about();
			else if (strcmp(f[2], "save") == 0) w_confirm();
			else if (strcmp(f[2], "yes") == 0) {
				say("saved", name);
				w_main();
			} else
				gm_busy(0);	/* "idle": nothing to show */
			break;
		case GM_SUBMIT:
			for (n = 0; n < GM_FIELDS && f[n] != NULL; n++) ;
			values(f, n);
			break;
		case GM_BACK:
			say("back", f[1]);
			cur = 'm';	/* the desktop shows main again */
			break;
		case GM_PAGE:
			say("page", f[2]);
			w_long(num(f[2]));
			break;
		case GM_REDRAW:
			if (cur == 'l') w_long(0);
			else w_main();
			break;
		case GM_CLOSE:
			say("close", NULL);
			return 0;
		}
	}
}
