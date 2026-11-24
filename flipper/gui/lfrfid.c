/* lfrfid - the 125 kHz RFID app: read a tag and save it to the SD card,
 * write a saved number to a blank fob or card, and emulate saved numbers.
 *
 * Reading and writing use the tested `rfid` command (fork + exec); the
 * emulation is the kernel's (RFIDIOCEMU on /dev/rfid); the saved tags are
 * `.rfid` files under /home/lfrfid, the stock format, one per number and
 * named by it (so no on-screen keyboard is needed).
 *
 * The screens are the shared ones of the stock app ports (gui/ui.c, issue
 * #413): the list asks for its rows one at a time, so the saved tags are
 * never held in memory (no table of names in .bss).  `lfrfid FILE` opens
 * one saved tag's actions (Archive's Open).
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <minix/rfid.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "gui.h"
#include "ui.h"
#include "../cmds/rfid_pw.h"

#define DIR_RFID	"/home/lfrfid"
#define PATHMAX		64

static unsigned long dur[128], pulse[128];	/* the EM4100 waveform */

/* ---- EM4100 waveform (as cmds/rfid.c) ---- */
static void em_bits(unsigned char id[5], unsigned char b[64])
{
	int r, c, p;

	for (r = 0; r < 9; r++) b[r] = 1;
	for (r = 0; r < 10; r++) {
		int nib = id[r / 2] >> (r % 2 ? 0 : 4) & 15;

		p = 0;
		for (c = 0; c < 4; c++) {
			b[9 + r * 5 + c] = nib >> (3 - c) & 1;
			p ^= b[9 + r * 5 + c];
		}
		b[9 + r * 5 + 4] = p;
	}
	for (c = 0; c < 4; c++) {
		p = 0;
		for (r = 0; r < 10; r++) p ^= b[9 + r * 5 + c];
		b[59 + c] = p;
	}
	b[63] = 0;
}

static int emu_start(unsigned char id[5])
{
	unsigned char b[64];
	struct rfid_emu e;
	int k, fd;

	em_bits(id, b);
	for (k = 0; k < 64; k++) {
		dur[2 * k] = dur[2 * k + 1] = 31;
		pulse[2 * k] = b[k] ? 32 : 0;
		pulse[2 * k + 1] = b[k] ? 0 : 32;
	}
	e.dur = dur;
	e.pulse = pulse;
	e.n = 128;
	if ((fd = open("/dev/rfid", O_RDWR)) < 0) return -1;
	if (ioctl(fd, RFIDIOCEMU, &e) < 0) { close(fd); return -1; }
	return fd;
}

/* ---- the rfid command, for reading and writing a tag ---- */

/* Run /bin/rfid with args (NULL-terminated), capturing its first line into
 * out (size osz).  Returns the exit status, or -1. */
static int run_rfid(char *const argv[], char *out, int osz)
{
	int pfd[2], st, n, got = 0;
	pid_t pid;

	if (out) out[0] = 0;
	if (pipe(pfd) < 0) return -1;
	if ((pid = fork()) < 0) { close(pfd[0]); close(pfd[1]); return -1; }
	if (pid == 0) {
		dup2(pfd[1], 1);
		dup2(pfd[1], 2);
		close(pfd[0]);
		close(pfd[1]);
		execv("/bin/rfid", argv);
		_exit(127);
	}
	close(pfd[1]);
	while (out && got < osz - 1 && (n = read(pfd[0], out + got, osz - 1 - got)) > 0)
		got += n;
	if (out) {
		out[got] = 0;
		out[strcspn(out, "\n")] = 0;
	}
	close(pfd[0]);
	while (waitpid(pid, &st, 0) < 0) ;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Read a tag: 1 and the number in id, or 0. */
static int read_tag(unsigned char id[5])
{
	char line[64], *p;
	char *av[] = { "rfid", NULL };
	unsigned v[5];
	int i;

	run_rfid(av, line, sizeof line);
	if ((p = strstr(line, "EM4100 ")) == NULL) return 0;
	if (sscanf(p + 7, "%x %x %x %x %x",
			&v[0], &v[1], &v[2], &v[3], &v[4]) != 5) return 0;
	for (i = 0; i < 5; i++) id[i] = (unsigned char) v[i];
	return 1;
}

/* Write a number to a tag: 1 if written, 0 if not. */
static int write_tag(unsigned char id[5])
{
	char b[5][4], line[80];
	char *av[] = { "rfid", "write", "EM", b[0], b[1], b[2], b[3], b[4], NULL };
	int i;

	for (i = 0; i < 5; i++) sprintf(b[i], "%02X", id[i]);
	return run_rfid(av, line, sizeof line) == 0;
}

/* ---- saved .rfid files ---- */

/* The number lives in the file's contents (the stock .rfid "Data:" line),
 * not its name, so a tag keeps working after it is renamed.  A new save is
 * named by its number ("<num>.rfid" would be 15 characters and MINIX file
 * names stop at 14, so no extension); Rename gives it any name up to 14. */
static void name_of(unsigned char id[5], char *s)
{
	sprintf(s, "%02X%02X%02X%02X%02X", id[0], id[1], id[2], id[3], id[4]);
}

static int save_tag(unsigned char id[5])
{
	char path[64], base[16];
	FILE *f;

	mkdir(DIR_RFID, 0755);			/* harmless if it exists */
	name_of(id, base);
	sprintf(path, "%s/%s", DIR_RFID, base);
	if ((f = fopen(path, "w")) == NULL) return 0;
	fprintf(f, "Filetype: Flipper RFID key\nVersion: 1\n"
		"Key type: EM4100\nData: %02X %02X %02X %02X %02X\n",
		id[0], id[1], id[2], id[3], id[4]);
	fclose(f);
	return 1;
}

/* The tag's number from a saved file's "Data:" line; 1 if found. */
static int load_tag(const char *path, unsigned char id[5])
{
	char line[80], *p;
	FILE *f;
	unsigned v[5];
	int i, ok = 0;

	if ((f = fopen(path, "r")) == NULL) return 0;
	while (fgets(line, sizeof line, f) != NULL) {
		/* `rfid -o` saves every protocol (T19a): this app plays
		 * EM4100 only, until R9.0.9 (a FDX-A also has 5 bytes) */
		if (strncmp(line, "Key type:", 9) == 0
		    && strcmp(line + 9, " EM4100\n") != 0
		    && strcmp(line + 9, " EM4100\r\n") != 0) break;
		if ((p = strstr(line, "Data:")) != NULL
		    && sscanf(p + 5, "%x %x %x %x %x",
				&v[0], &v[1], &v[2], &v[3], &v[4]) == 5) {
			for (i = 0; i < 5; i++) id[i] = (unsigned char) v[i];
			ok = 1;
			break;
		}
	}
	fclose(f);
	return ok;
}

/* Saved tag i (counting only real tags) into path; the count when i < 0.
 * One directory pass a call: a list asks only for the rows on screen. */
static int saved_tag(int i, char *path, char *name)
{
	DIR *d;
	struct dirent *e;
	unsigned char id[5];
	char p[PATHMAX];
	int n = 0;

	if ((d = opendir(DIR_RFID)) == NULL) return i < 0 ? 0 : -1;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') continue;		/* . and .. */
		if (strlen(DIR_RFID) + strlen(e->d_name) + 2 > PATHMAX) continue;
		sprintf(p, "%s/%s", DIR_RFID, e->d_name);
		if (!load_tag(p, id)) continue;			/* a real tag */
		if (n++ == i) {
			if (path) strcpy(path, p);
			if (name) {
				strncpy(name, e->d_name, UI_ITEM - 1);
				name[UI_ITEM - 1] = 0;
			}
			closedir(d);
			return 0;
		}
	}
	closedir(d);
	return i < 0 ? n : -1;
}

/* Rename a saved tag (in its folder); 1 on success, path is the new one. */
static int rename_tag(char *path, const char *newname)
{
	char b[PATHMAX], *s = strrchr(path, '/');
	int dl = s ? (int) (s - path) + 1 : 0;

	if (newname[0] == 0 || strcmp(path + dl, newname) == 0) return 1;
	if (strchr(newname, '/') || dl + strlen(newname) + 1 > PATHMAX) return 0;
	memcpy(b, path, (size_t) dl);
	strcpy(b + dl, newname);
	if (access(b, F_OK) == 0 || rename(path, b) != 0) return 0;
	strcpy(path, b);
	return 1;
}

/* ---- screens ---- */

#define menu(t, items, n)	ui_menu(t, items, n)
#define msg(l1, l2)		ui_msg(l1, l2)

static void show_num(const char *title, unsigned char id[5], const char *hint)
{
	char line[40];

	gui_clear();
	gui_title(title);
	sprintf(line, "%02X %02X %02X %02X %02X",
		id[0], id[1], id[2], id[3], id[4]);
	gui_center(26, line, 0);
	if (hint) gui_center(52, hint, 0);
	gui_flush();
}

/* Emulate id until OK/Back. */
static void do_emulate(unsigned char id[5])
{
	int fd, k;

	if ((fd = emu_start(id)) < 0) {
		msg("cannot open", "/dev/rfid");
		gui_key();
		return;
	}
	gui_led(0, 40, 0);
	for (;;) {
		show_num("EMULATING", id, "hold to reader; OK/BACK");
		k = gui_key() & ~KEY_LONG;
		if (k == KEY_OK || k == KEY_BACK) break;
	}
	close(fd);				/* the kernel drops the field */
	gui_led(0, 0, 0);
}

/* Read a tag, offer to save it. */
static void do_read(void)
{
	unsigned char id[5];
	int k;

	for (;;) {				/* `rfid` itself tries for ~2 s */
		msg("Reading...", "hold tag to the back");
		if (read_tag(id)) break;
		gui_clear();
		gui_center(22, "no tag", 0);
		gui_center(40, "OK retry  BACK exit", 0);
		gui_flush();
		if ((gui_key() & ~KEY_LONG) == KEY_BACK) return;
	}
	for (;;) {
		show_num("READ", id, "OK save  BACK cancel");
		k = gui_key() & ~KEY_LONG;
		if (k == KEY_BACK) return;
		if (k == KEY_OK) {
			msg(save_tag(id) ? "saved" : "could not save", NULL);
			gui_key();
			return;
		}
	}
}

/* Edit a five-byte number; 1 if OK was pressed on it, 0 on Back. */
static int edit_num(unsigned char id[5])
{
	int sel = 0, i, k;
	char line[8];

	for (;;) {
		gui_clear();
		gui_title("NUMBER");
		for (i = 0; i < 5; i++) {
			sprintf(line, "%02X", id[i]);
			gui_text(14 + i * 20, 28, line, i == sel);
		}
		gui_center(44, "L/R pick  U/D change", 0);
		gui_center(54, "OK done  BACK cancel", 0);
		gui_flush();
		k = gui_key() & ~KEY_LONG;
		if (k == KEY_BACK) return 0;
		if (k == KEY_LEFT) sel = (sel + 4) % 5;
		if (k == KEY_RIGHT) sel = (sel + 1) % 5;
		if (k == KEY_UP) id[sel]++;
		if (k == KEY_DOWN) id[sel]--;
		if (k == KEY_OK) return 1;
	}
}

/* A saved tag (path): Emulate / Write / Rename / Delete. */
static void do_saved_one(const char *file)
{
	static const char *const acts[] =
		{ "Emulate", "Write to tag", "Rename", "Delete" };
	unsigned char id[5];
	char path[PATHMAX];
	int a;

	if (strlen(file) >= PATHMAX) return;
	strcpy(path, file);
	if (!load_tag(path, id)) { ui_note("not an EM4100 tag", NULL); return; }
	for (;;) {
		char title[24], *b = strrchr(path, '/');

		sprintf(title, "%.14s", b ? b + 1 : path);
		a = menu(title, acts, 4);
		if (a < 0) return;
		if (a == 0) do_emulate(id);
		else if (a == 1) {
			msg("Writing...", "hold a blank fob");
			ui_note(write_tag(id) ? "written" : "not written (locked?)",
				"BACK to return");
		} else if (a == 2) {			/* Rename (max 14 chars) */
			char nn[15];

			strncpy(nn, b ? b + 1 : path, sizeof nn - 1);
			nn[sizeof nn - 1] = 0;
			if (ui_text("NAME", nn, sizeof nn) && !rename_tag(path, nn))
				ui_note("rename failed", NULL);
		} else {
			unlink(path);
			return;
		}
	}
}

static int saved_item(void *ctx, int i, char *buf)
{
	(void) ctx;
	return saved_tag(i, NULL, buf);
}

static void do_saved(void)
{
	char path[PATHMAX];
	int n, sel = 0;

	for (;;) {
		if ((n = saved_tag(-1, NULL, NULL)) == 0) {
			ui_note("no saved tags", "BACK to return");
			return;
		}
		sel = ui_list("SAVED", saved_item, NULL, n, sel, 0, NULL);
		if (sel < 0) return;
		if (saved_tag(sel, path, NULL) == 0) do_saved_one(path);
	}
}

static void do_add(void)
{
	unsigned char id[5] = { 0x04, 0x00, 0x98, 0x3D, 0xC5 };

	if (!edit_num(id)) return;
	msg(save_tag(id) ? "saved" : "could not save", NULL);
	gui_key();
}

/* ---- the copier's password (issue #448): where `rfid copy` gets it ---- */
static int pw_item(void *ctx, int i, char *buf)
{
	unsigned long w;

	(void) ctx;
	if (!pw_nth(PW_DICT, i, &w)) return 0;
	sprintf(buf, "%08lX", w);
	return 1;
}

static void do_copier(void)
{
	static const char *const modes[] =
		{ "Legacy: none", "Own: new random", "Previous copier", "From dictionary" };
	unsigned long w;
	char hex[12], line[40];
	char *av[] = { "rfid", "copier", NULL, NULL, NULL };
	int m, n, k;

	m = menu("COPIER PASSWORD", modes, 4);
	if (m < 0) return;
	av[2] = (char *) pw_mode_name[m];
	if (m == PW_DICT_PICK) {
		if ((n = pw_count(PW_DICT)) == 0) {
			ui_note("dictionary is empty", "BACK to return");
			return;
		}
		k = ui_list("DICTIONARY", pw_item, NULL, n, 0, 0, NULL);
		if (k < 0 || !pw_nth(PW_DICT, k, &w)) return;
		sprintf(hex, "%08lX", w);
		av[3] = hex;
	}
	ui_note(run_rfid(av, line, sizeof line) == 0 ? "saved" : "not saved", "BACK to return");
}

int main(int argc, char **argv)
{
	static const char *const main_items[] =
		{ "Read", "Saved", "Add manually", "Copier password" };
	int sel;

	if (gui_open() < 0) return 1;
	if (argc > 1 && argv[1][0] == '/') {	/* Archive's Open (#413) */
		do_saved_one(argv[1]);
		gui_led(0, 0, 0);
		return 0;
	}
	for (;;) {
		sel = menu("RFID", main_items, 4);
		if (sel < 0) break;
		if (sel == 0) do_read();
		else if (sel == 1) do_saved();
		else if (sel == 2) do_add();
		else do_copier();
	}
	gui_led(0, 0, 0);
	return 0;
}
