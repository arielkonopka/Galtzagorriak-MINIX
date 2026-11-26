/* carddet - what is this card or fob?  125 kHz RFID, 13.56 MHz NFC, or both.
 *
 * The Flipper has one coil for each band and cannot run both fields at the
 * same moment, so the detector takes turns: a try of `rfid`, then a try of
 * `nfc -r`, again and again, until one of them reads something (Back stops
 * it at any moment).  Both programs are the tested ones (fork + exec) and
 * each saves what it read as a stock file, so a dual-frequency fob that
 * answers in both fields gives a pair named alike:
 *	/home/flipper/lfrfid/cdNNN.rfid  and  /home/flipper/nfc/cdNNN.nfc
 * (a base of up to 14 characters with its extension, as an older /home
 * wants).  Nothing is kept in memory: the saved list asks for its rows one
 * at a time, as the RFID app's does.
 *
 * Writing a pair goes RFID first (`rfid write -f`), then NFC.  There is no
 * NFC card writer yet; that step says so and the .nfc file stays.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "gui.h"
#include "ui.h"

#define HOME		"/home/flipper"
#define DIR_RFID	HOME "/lfrfid"
#define DIR_NFC		HOME "/nfc"
#define PFX		"cd"
#define PATHMAX		64

#define HAS_RFID	1
#define HAS_NFC		2

/* ---- files of a base ---- */

static void path_rfid(const char *b, char *p) { sprintf(p, "%s/%s.rfid", DIR_RFID, b); }
static void path_nfc(const char *b, char *p) { sprintf(p, "%s/%s.nfc", DIR_NFC, b); }

/* Which files of base b exist. */
static int have(const char *b)
{
	char p[PATHMAX];
	int h = 0;

	path_rfid(b, p);
	if (access(p, F_OK) == 0) h |= HAS_RFID;
	path_nfc(b, p);
	if (access(p, F_OK) == 0) h |= HAS_NFC;
	return h;
}

/* The first free base cdNNN (neither file exists); 0 if none. */
static int new_base(char *b)
{
	int i;

	mkdir(HOME, 0755);
	mkdir(DIR_RFID, 0755);
	mkdir(DIR_NFC, 0755);
	for (i = 1; i < 1000; i++) {
		sprintf(b, PFX "%03d", i);
		if (have(b) == 0) return 1;
	}
	return 0;
}

/* The value of the line "KEY ..." (key with its colon) in a stock file. */
static int field(const char *path, const char *key, char *out, int n)
{
	char line[80];
	FILE *f;
	size_t kl = strlen(key);
	int ok = 0;

	out[0] = 0;
	if ((f = fopen(path, "r")) == NULL) return 0;
	while (fgets(line, sizeof line, f) != NULL)
		if (strncmp(line, key, kl) == 0) {
			char *v = line + kl;

			while (*v == ' ') v++;
			v[strcspn(v, "\r\n")] = 0;
			strncpy(out, v, (size_t) n - 1);
			out[n - 1] = 0;
			ok = 1;
			break;
		}
	fclose(f);
	return ok;
}

/* ---- the two readers, taking turns ---- */

/* Run argv; -1 if Back was pressed (the child is killed: the kernel drops
 * the field when it closes the device), else the exit status. */
static int run(char *const argv[])
{
	struct gui_in in;
	pid_t pid;
	int st, k;

	memset(&in, 0, sizeof in);
	if ((pid = fork()) < 0) return 255;
	if (pid == 0) {
		int fd = open("/dev/null", O_RDWR);

		if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
		execv(argv[0], argv + 1);
		_exit(127);
	}
	for (;;) {
		if (waitpid(pid, &st, WNOHANG) == pid)
			return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
		k = gui_input(&in, 0, 100);
		if (k == KEY_BACK) {
			kill(pid, SIGKILL);
			while (waitpid(pid, &st, 0) < 0) ;
			return -1;
		}
	}
}

/* One turn of each field into base b.  The HAS_ bits of what was read, or
 * -1 on Back. */
static int probe(const char *b)
{
	char rp[PATHMAX], np[PATHMAX];
	/* run(): argv[0] is the path to execute, the rest its argv */
	char *ar[] = { "/bin/rfid", "rfid", "-n", "1", "-o", rp, NULL };
	char *an[] = { "/bin/nfc", "nfc", "-r", "-o", np, NULL };
	int r;

	path_rfid(b, rp);
	path_nfc(b, np);
	ui_msg("Looking...", "125 kHz, 13.56 MHz");
	if (run(ar) < 0) return -1;
	if (run(an) < 0) return -1;
	r = have(b);
	return r;
}

/* ---- screens ---- */

static void shorten(char *s, int n) { if ((int) strlen(s) > n) s[n] = 0; }

/* The two bands of base b on one screen. */
static void show(const char *title, const char *b, const char *hint)
{
	char p[PATHMAX], v[40], l[40];
	int h = have(b), y = 16;

	gui_clear();
	gui_title(title);
	if (h & HAS_RFID) {
		path_rfid(b, p);
		field(p, "Key type:", v, sizeof v);
		shorten(v, 14);
		sprintf(l, "RFID %s", v);
		gui_text(2, y, l, 0);
		field(p, "Data:", v, sizeof v);
		shorten(v, 20);
		gui_text(2, y + 10, v, 0);
		y += 22;
	}
	if (h & HAS_NFC) {
		path_nfc(b, p);
		field(p, "Device type:", v, sizeof v);
		shorten(v, 14);
		sprintf(l, "NFC %s", v);
		gui_text(2, y, l, 0);
		field(p, "UID:", v, sizeof v);
		shorten(v, 20);
		gui_text(2, y + 10, v, 0);
	}
	if (hint) gui_center(56, hint, 0);
	gui_flush();
}

static const char *kind(int h)
{
	return h == (HAS_RFID | HAS_NFC) ? "RFID + NFC" : h == HAS_RFID ? "RFID only" : "NFC only";
}

static void del(const char *b)
{
	char p[PATHMAX];

	path_rfid(b, p);
	unlink(p);
	path_nfc(b, p);
	unlink(p);
}

/* Write a saved pair: RFID, then NFC. */
static void write_pair(const char *b)
{
	char rp[PATHMAX], np[PATHMAX];
	char *aw[] = { "/bin/rfid", "rfid", "write", "-n", "20", "-f", rp, NULL };
	int h = have(b), r;

	path_rfid(b, rp);
	path_nfc(b, np);
	if (h & HAS_RFID) {
		ui_msg("1: RFID", "hold a blank fob; BACK");
		r = run(aw);
		if (r < 0) return;
		ui_note(r == 0 ? "RFID written" : "RFID not written", "any key");
		gui_key();
	}
	if (h & HAS_NFC)
		ui_note("NFC: no writer yet", "the .nfc file is kept");
}

/* A saved base: Write / Delete. */
static void one(const char *b)
{
	static const char *const acts[] = { "Write pair", "Delete" };
	int a;

	for (;;) {
		a = ui_menu(b, acts, 2);
		if (a < 0) return;
		if (a == 0) write_pair(b);
		else { del(b); return; }
	}
}

/* ---- the saved list: cdNNN of either folder, each name once ---- */

/* Base number i (counting each base once); the count when i < 0. */
static int saved_base(int i, char *b)
{
	static const char *const dir[2] = { DIR_RFID, DIR_NFC };
	static const char *const ext[2] = { ".rfid", ".nfc" };
	DIR *d;
	struct dirent *e;
	char name[16], p[PATHMAX];
	int n = 0, k, l;

	for (k = 0; k < 2; k++) {
		if ((d = opendir(dir[k])) == NULL) continue;
		while ((e = readdir(d)) != NULL) {
			l = (int) strlen(e->d_name) - (int) strlen(ext[k]);
			if (l <= 0 || l >= (int) sizeof name
			    || strncmp(e->d_name, PFX, 2) != 0
			    || strcmp(e->d_name + l, ext[k]) != 0) continue;
			memcpy(name, e->d_name, (size_t) l);
			name[l] = 0;
			if (k == 1) {		/* a pair counts at its RFID */
				path_rfid(name, p);
				if (access(p, F_OK) == 0) continue;
			}
			if (n++ == i) {
				strcpy(b, name);
				closedir(d);
				return 0;
			}
		}
		closedir(d);
	}
	return i < 0 ? n : -1;
}

static int item(void *ctx, int i, char *buf)
{
	char b[16];

	(void) ctx;
	if (saved_base(i, b) != 0) return -1;
	sprintf(buf, "%s %s", b, have(b) == (HAS_RFID | HAS_NFC) ? "R+N"
		: have(b) == HAS_RFID ? "R" : "N");
	return 0;
}

static void saved(void)
{
	char b[16];
	int n, sel = 0;

	for (;;) {
		if ((n = saved_base(-1, NULL)) == 0) {
			ui_note("nothing saved", "BACK to return");
			return;
		}
		sel = ui_list("SAVED", item, NULL, n, sel, 0, NULL);
		if (sel < 0) return;
		if (saved_base(sel, b) == 0) one(b);
	}
}

/* Detect until something is read (or Back); then what to do with it. */
static void detect(void)
{
	char b[16];
	int h, k;

	for (;;) {
		if (!new_base(b)) { ui_note("too many saved", "delete some"); return; }
		do {
			h = probe(b);
			if (h < 0) return;
		} while (h == 0);
		/* a fob that answers one field may need a second turn for the other */
		if (h != (HAS_RFID | HAS_NFC) && probe(b) > 0) h = have(b);
		gui_led(0, 40, 0);
		for (;;) {
			char t[24];

			sprintf(t, "%s", kind(h));
			show(t, b, "OK write <del >next");
			k = gui_key();
			if (k == KEY_OK) { write_pair(b); continue; }
			break;
		}
		gui_led(0, 0, 0);
		if (k == KEY_BACK) return;
		if (k == KEY_LEFT) del(b);	/* Left: forget it, scan again */
		/* KEY_RIGHT: keep it and scan for the next card */
	}
}

int main(void)
{
	static const char *const items[] = { "Detect", "Saved" };
	int sel;

	if (gui_open() < 0) return 1;
	for (;;) {
		sel = ui_menu("CARD DETECT", items, 2);
		if (sel < 0) break;
		if (sel == 0) detect();
		else saved();
	}
	gui_led(0, 0, 0);
	return 0;
}
