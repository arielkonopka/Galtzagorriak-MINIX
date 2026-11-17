/* lfrfid - the 125 kHz RFID app: read a tag and save it to the SD card,
 * write a saved number to a blank fob or card, and emulate saved numbers.
 *
 * Reading and writing use the tested `rfid` command (fork + exec); the
 * emulation is the kernel's (RFIDIOCEMU on /dev/rfid); the saved tags are
 * `.rfid` files under /home/lfrfid, the stock format, one per number and
 * named by it (so no on-screen keyboard is needed).
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

#define DIR_RFID	"/home/lfrfid"
#define MAXSAVED	32

static unsigned long dur[128], pulse[128];	/* the EM4100 waveform */
static char saved[MAXSAVED][16];		/* saved file base names */
static int nsaved;

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
static int load_tag(const char *base, unsigned char id[5])
{
	char path[64], line[80], *p;
	FILE *f;
	unsigned v[5];
	int i, ok = 0;

	sprintf(path, "%s/%s", DIR_RFID, base);
	if ((f = fopen(path, "r")) == NULL) return 0;
	while (fgets(line, sizeof line, f) != NULL)
		if ((p = strstr(line, "Data:")) != NULL
		    && sscanf(p + 5, "%x %x %x %x %x",
				&v[0], &v[1], &v[2], &v[3], &v[4]) == 5) {
			for (i = 0; i < 5; i++) id[i] = (unsigned char) v[i];
			ok = 1;
			break;
		}
	fclose(f);
	return ok;
}

static void scan_saved(void)
{
	DIR *d;
	struct dirent *e;
	unsigned char id[5];

	nsaved = 0;
	if ((d = opendir(DIR_RFID)) == NULL) return;
	while (nsaved < MAXSAVED && (e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') continue;		/* . and .. */
		if (strlen(e->d_name) >= sizeof saved[0]) continue;
		if (!load_tag(e->d_name, id)) continue;		/* a real tag */
		strcpy(saved[nsaved++], e->d_name);
	}
	closedir(d);
}

static void del_tag(const char *base)
{
	char path[64];

	sprintf(path, "%s/%s", DIR_RFID, base);
	unlink(path);
}

/* Rename a saved tag; 1 on success. */
static int rename_tag(const char *base, const char *newname)
{
	char a[64], b[64];

	if (newname[0] == 0 || strcmp(base, newname) == 0) return 1;
	sprintf(a, "%s/%s", DIR_RFID, base);
	sprintf(b, "%s/%s", DIR_RFID, newname);
	return rename(a, b) == 0;
}

/* ---- screens ---- */

/* A vertical menu: returns the chosen index, or -1 on Back. */
static int menu(const char *title, const char *const *items, int n)
{
	int sel = 0, i, k;

	for (;;) {
		gui_clear();
		gui_title(title);
		for (i = 0; i < n; i++)
			gui_text(6, 14 + i * 10, items[i], i == sel);
		gui_flush();
		k = gui_key() & ~KEY_LONG;
		if (k == KEY_BACK) return -1;
		if (k == KEY_UP) sel = (sel + n - 1) % n;
		if (k == KEY_DOWN) sel = (sel + 1) % n;
		if (k == KEY_OK) return sel;
	}
}

static void msg(const char *l1, const char *l2)
{
	gui_clear();
	gui_center(22, l1, 0);
	if (l2) gui_center(36, l2, 0);
	gui_flush();
}

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

/* An on-screen keyboard.  Three character rows of ten (the current case),
 * then a row of keys: Shift cycles lower-case, UPPER-CASE and digits and
 * symbols; Bksp rubs out the last character, Del clears the name, Ret
 * finishes (so does BACK).  Edits buf in place, at most max - 1 characters;
 * every character it offers is safe in a file name. */
#define KB_COLS		10
#define KB_FKEYS	4		/* Shift Bksp Del Ret */
static void text_input(char *buf, int max)
{
	static const char *const cs[3] = {	/* exactly 30 each */
		"abcdefghijklmnopqrstuvwxyz .-_",
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ .-_",
		"0123456789@#$%&()+=,.!?*-_ :;~"
	};
	static const char *const fn[KB_FKEYS] = { "Shift", "Bksp", "Del", "Ret" };
	int mode = 0, cx = 0, cy = 0, k, len = strlen(buf), r, c, w;

	for (;;) {
		gui_clear();
		gui_title("NAME");
		gui_text(2, 10, buf, 0);
		gui_vline(2 + len * 4, 8, 15, 1);		/* the cursor */
		for (r = 0; r < 3; r++)
			for (c = 0; c < KB_COLS; c++) {
				char s[2];

				s[0] = cs[mode][r * KB_COLS + c];
				s[1] = 0;
				gui_text(4 + c * 12, 22 + r * 10, s,
					r == cy && c == cx);
			}
		for (c = 0; c < KB_FKEYS; c++)
			gui_text(3 + c * 31, 54, fn[c], cy == 3 && c == cx);
		gui_flush();
		w = (cy < 3) ? KB_COLS : KB_FKEYS;
		k = gui_key() & ~KEY_LONG;
		if (k == KEY_BACK) return;
		if (k == KEY_UP) cy = (cy + 3) % 4;
		if (k == KEY_DOWN) cy = (cy + 1) % 4;
		if (k == KEY_LEFT) cx = (cx + w - 1) % w;
		if (k == KEY_RIGHT) cx = (cx + 1) % w;
		if (cy == 3) { if (cx >= KB_FKEYS) cx = KB_FKEYS - 1; }
		else if (cx >= KB_COLS) cx = KB_COLS - 1;
		if (k != KEY_OK) continue;
		if (cy < 3) {				/* a character */
			if (len < max - 1) {
				buf[len++] = cs[mode][cy * KB_COLS + cx];
				buf[len] = 0;
			}
		} else switch (cx) {			/* a key */
		case 0: mode = (mode + 1) % 3; break;		/* Shift */
		case 1: if (len > 0) buf[--len] = 0; break;	/* Bksp */
		case 2: len = 0; buf[0] = 0; break;		/* Del */
		case 3: return;					/* Ret */
		}
	}
}

/* A saved tag: Emulate / Write / Rename / Delete. */
static void do_saved_one(const char *base)
{
	static const char *const acts[] =
		{ "Emulate", "Write to tag", "Rename", "Delete" };
	unsigned char id[5];
	char name[16];
	int a;

	strcpy(name, base);
	if (!load_tag(name, id)) return;
	for (;;) {
		char title[24];

		sprintf(title, "%.14s", name);
		a = menu(title, acts, 4);
		if (a < 0) return;
		if (a == 0) do_emulate(id);
		else if (a == 1) {
			msg("Writing...", "hold a blank fob");
			msg(write_tag(id) ? "written" : "not written (locked?)",
				"BACK to return");
			gui_key();
		} else if (a == 2) {			/* Rename (max 14 chars) */
			char nn[15];

			strncpy(nn, name, sizeof nn - 1);
			nn[sizeof nn - 1] = 0;
			text_input(nn, sizeof nn);
			if (rename_tag(name, nn)) strcpy(name, nn);
			else { msg("rename failed", NULL); gui_key(); }
		} else {
			del_tag(name);
			return;
		}
	}
}

static void do_saved(void)
{
	const char *items[MAXSAVED];
	int i, sel;

	for (;;) {
		scan_saved();
		if (nsaved == 0) {
			msg("no saved tags", "BACK to return");
			gui_key();
			return;
		}
		for (i = 0; i < nsaved; i++) items[i] = saved[i];
		sel = menu("SAVED", items, nsaved);
		if (sel < 0) return;
		do_saved_one(saved[sel]);
	}
}

static void do_add(void)
{
	unsigned char id[5] = { 0x04, 0x00, 0x98, 0x3D, 0xC5 };

	if (!edit_num(id)) return;
	msg(save_tag(id) ? "saved" : "could not save", NULL);
	gui_key();
}

int main(void)
{
	static const char *const main_items[] = { "Read", "Saved", "Add manually" };
	int sel;

	if (gui_open() < 0) return 1;
	for (;;) {
		sel = menu("RFID", main_items, 3);
		if (sel < 0) break;
		if (sel == 0) do_read();
		else if (sel == 1) do_saved();
		else do_add();
	}
	gui_led(0, 0, 0);
	return 0;
}
