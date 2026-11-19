/* launcher - the main screen of Galtzagorriak MINIX.
 *
 * Started as root by /etc/rc; it takes over the LCD from the boot screen
 * and shows the apps listed in /etc/apps, one per line:
 *
 *	Label:user:/path/to/program arg ...
 *	Label:user:icon:/path/to/program arg ...	(spec R5.6a.1)
 *
 * UP/DOWN choose, OK starts.  The app runs as the given user (e.g.
 * "nobody"), in group "gui" so it may use the screen and buttons; only
 * apps marked "root" keep root's rights.  The launcher waits until the app
 * exits (BACK in every app), then redraws.
 *
 * A window app ("=" before its program, spec K.8) does not get the screen:
 * it describes its windows on a pipe and the launcher draws them and
 * moves through them itself (winsrv.c), so the app needs no GUI code and
 * an open one costs about 4 KB.
 *
 * Pages (R5.6a.4, spec K.6): in a row, SETTINGS <- APPS -> HOME -> NOTES
 * -> RUNNING.  APPS is the app list; SETTINGS the system groups (K.7) and
 * the launcher's own (icons, hints, battery, /etc/launcher.conf); HOME the
 * tiles of the desktop's modules; NOTES the last 8 notifications (T23c:
 * OK opens one, Back clears it); RUNNING the parked apps (R12.20, not
 * there yet: the page says so).  A page is
 * an entry of pages[]: its name, hint text and neighbours.  Left and
 * Right act on the release of a key pressed alone (gui_input), so the
 * backlight gesture Left+Up (R6.10) never turns a page.  The look is spec
 * R5.6a.7 ("Pine meets modern UX").
 *
 * The desktop (spec K.6, T23a): the title bar is the status line - the
 * battery on the left, the page's name, and on the right the time, the
 * unread notifications and a module's status.  It is redrawn only when
 * something changes: a key, a record, the minute (a poll() timeout, only
 * while the screen is on: with the light off the launcher sleeps with no
 * timeout).  Modules write records to the FIFO /var/run/gui (desktop.c,
 * guimod); the launcher reads it when poll() says so, keys first.
 *
 *	launcher [-a apps] [-c conf]	other files, for the QEMU tests
 */
#include <lib.h>
#include <minix/callnr.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <minix/fg.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "gui.h"
#include "appline.h"
#include "winsrv.h"
#include "desktop.h"

#define MAXAPPS	16
#define MAXARGS	8
#define CMDMAX	APP_CMDMAX
#define ROW_Y(i)	(7 + (i) * 6)
#define HINT_Y	58			/* the hint line; its rule at 56 */
#define NOTE_MS	2000			/* a notice on the message line */

/* One app: label, user and command, packed in one malloc'd string
 * (R2a.5: was 16 fixed 100-byte entries in .bss).  An entry with an icon
 * has its 6 rows at the start of the block, before the label (R5.6a.5:
 * no allocation of its own); bit i of has_icon says which. */
struct app {
	char *label;
	char *user;
	char *cmd;
} apps[MAXAPPS];
int napps;
int fsck_err;		/* /etc/rc found errors on the card (spec N.3) */
static unsigned short has_icon;
static unsigned char opt;		/* OPT_* below */
#define OPT_ICONS	1
#define OPT_HINTS	2
#define OPT_BATT	4
#define OPT_LOG		8		/* GuiLog: on (spec K.3, K.8) */
/* The battery on the title bar (owner 2026-11-17): the charge in percent
 * (BAT_NONE: not read yet or no gauge) and BAT_CHG while charging, read
 * from /dev/power at a redraw when the last reading is a minute old - so
 * the launcher never wakes for it. */
static unsigned char bat;
static unsigned short bat_t;		/* seconds (mod 2^16) of that reading */
#define BAT_CHG		0x80
#define BAT_NONE	0x7F
/* The desktop (spec K.3, K.6): the module table, the FIFO (read end, and
 * a write end of our own so that it never reads as closed), the light. */
static struct desk desk;
static int dk_rfd = -1, dk_wfd = -1, bl_fd = -1;
static char dk_skip;			/* the rest of an overlong record */
static unsigned char dk_new;		/* notifications in the last desk_read */
static char clock_shown;		/* the frame on the LCD has the time */
static time_t shown_min;		/* and this minute (time() / 60) */

#define ICON(i)	((unsigned char *) apps[i].label - ICON_H)

static void free_apps(void)
{
	while (napps > 0) {
		napps--;
		free(has_icon & (1 << napps) ? (char *) ICON(napps)
						: apps[napps].label);
	}
	has_icon = 0;
}

static void load_apps(const char *apps_file)
{
	FILE *f;
	char line[128], *m;
	struct app_fields a;
	unsigned char rows[ICON_H];
	size_t la, lu, lc;
	int ic;

	fsck_err = access("/etc/.fsck-errors", F_OK) == 0;
	free_apps();
	/* the card's copy, else the default in /rom (spec L.3) */
	if ((f = cfopen(apps_file, "r")) == NULL) return;
	while (napps < MAXAPPS && fgets(line, sizeof line, f) != NULL) {
		if (app_line(line, &a) < 0) continue;
		ic = a.icon != NULL && icon_parse(a.icon, rows, gui_glyph_row);
		la = strlen(a.label);
		lu = strlen(a.user);
		lc = strlen(a.cmd);
		if ((m = malloc((ic ? ICON_H : 0) + la + lu + lc + 3)) == NULL)
			break;
		if (ic) {
			memcpy(m, rows, ICON_H);
			m += ICON_H;
			has_icon |= 1 << napps;
		}
		apps[napps].label = strcpy(m, a.label);
		apps[napps].user = strcpy(m + la + 1, a.user);
		apps[napps].cmd = strcpy(m + la + lu + 2, a.cmd);
		napps++;
	}
	fclose(f);
}

/* /etc/launcher.conf: "Icons: on|off", "Hints: on|off", "Battery: on|off"
 * (stock Key: value; missing file or key: on) and "GuiLog: on|off" (off
 * when missing): the window apps' records and events on the console. */
static void load_conf(const char *conf_file)
{
	FILE *f;
	char line[40];

	opt = OPT_ICONS | OPT_HINTS | OPT_BATT;
	if ((f = cfopen(conf_file, "r")) == NULL) return;
	while (fgets(line, sizeof line, f) != NULL) {
		int bit = strncmp(line, "Icons:", 6) == 0 ? OPT_ICONS
			: strncmp(line, "Hints:", 6) == 0 ? OPT_HINTS
			: strncmp(line, "Battery:", 8) == 0 ? OPT_BATT
			: strncmp(line, "GuiLog:", 7) == 0 ? OPT_LOG : 0;
		char *v = strchr(line, ':') + 1;

		if (bit == 0) continue;
		while (*v == ' ' || *v == '\t') v++;
		if (strncmp(v, "off", 3) == 0) opt &= ~bit;
		else if (strncmp(v, "on", 2) == 0) opt |= bit;
	}
	fclose(f);
}

static void save_conf(const char *conf_file)
{
	FILE *f;

	if ((f = fopen(conf_file, "w")) == NULL) return;
	fprintf(f, "Icons: %s\nHints: %s\nBattery: %s\n",
		opt & OPT_ICONS ? "on" : "off", opt & OPT_HINTS ? "on" : "off",
		opt & OPT_BATT ? "on" : "off");
	if (opt & OPT_LOG) fprintf(f, "GuiLog: on\n");
	fclose(f);
}

/*===========================================================================*
 *				Pages					     *
 *===========================================================================*/
/* A page: its title, hint line, and the pages Left and Right lead to.
 * No pointers, so the table stays in flash (a PIC program's pointers
 * would be relocated into RAM); the rows are drawn by page number. */
struct page {
	char title[10];
	char hints[32];
	signed char left, right;		/* page index, -1: none */
};

#define PG_APPS	0
#define PG_SYS	1			/* SETTINGS: the groups (K.7) */
#define PG_SET	2			/* the launcher's own settings */
#define PG_HOME	3			/* the modules' tiles (K.6) */
#define PG_NOTES 4			/* the notifications (T23c) */
#define PG_RUN	5			/* the parked apps (R12.20) */
#define PG_NOTE	6			/* one notification, opened */
static const struct page pages[] = {
	{ "APPS", "<Settings OK Run >Home", PG_SYS, PG_HOME },
	{ "SETTINGS", "OK Open  Back Done", -1, -1 },
	{ "LAUNCHER", "OK Toggle  Back Done", -1, -1 },
	{ "HOME", "<Apps  >Notes", PG_APPS, PG_NOTES },
	{ "NOTES", "<Home OK Open Back Clear >Run", PG_HOME, PG_RUN },
	{ "RUNNING", "<Notes", PG_NOTES, -1 },
	{ "NOTE", "OK Keep  Back Clear", -1, -1 },
};
static int nsel;			/* PG_NOTE: the notification shown */
#define NOTE_COLS	31		/* characters on a row of PG_NOTE */

/* The SETTINGS page (spec K.7): every group but Launcher is a program run
 * like an app, the groups of setgroup as window apps ("=", K.8: the
 * launcher draws them); "" = not there yet.  Power is today's app until
 * its group comes (T24c). */
static const struct {
	char name[9];
	char user[7];
	char cmd[24];
} sys_tab[] = {
	{ "Display", "root", "=/bin/setgroup display" },
	{ "Sound", "root", "=/bin/setgroup sound" },
	{ "LED", "", "" },
	{ "Time", "root", "=/bin/setgroup time" },
	{ "Power", "nobody", "/bin/power" },
	{ "Storage", "", "" },
	{ "Gestures", "", "" },
	{ "Launcher", "", "" },
	{ "About", "root", "=/bin/setgroup about" },
};
#define SYS_N		(int) (sizeof(sys_tab) / sizeof(sys_tab[0]))
#define SYS_LAUNCHER	7

/* "HH:MM" of hm (hour * 60 + minute) into b[6]. */
static char *hm_text(char *b, unsigned hm)
{
	b[0] = (char) ('0' + hm / 600);
	b[1] = (char) ('0' + hm / 60 % 10);
	b[2] = ':';
	b[3] = (char) ('0' + hm % 60 / 10);
	b[4] = (char) ('0' + hm % 10);
	b[5] = 0;
	return b;
}

static int page_rows(int pg)
{
	return pg == PG_APPS ? napps : pg == PG_SYS ? SYS_N
		: pg == PG_HOME ? dk_tiles(&desk) : pg == PG_NOTES ? desk.notes
		: pg == PG_RUN ? 0 : pg == PG_NOTE ? 2 + ((int) strlen(
			desk.n[nsel].body) + NOTE_COLS - 1) / NOTE_COLS : 3;
}

static const char set_name[3][8] = { "Icons", "Hints", "Battery" };
static const unsigned char set_bit[3] = { OPT_ICONS, OPT_HINTS, OPT_BATT };

/* Draw row i of page pg at y. */
static void page_row(int pg, int i, int y, int inv)
{
	int on;

	if (pg == PG_HOME)
		gui_text(6, y, dk_tile(&desk, i)->text, inv);
	else if (pg == PG_NOTES) {	/* "* Title          12:34" */
		const struct dk_note *e = &desk.n[i];
		char b[6];

		if (e->unread) gui_text(6, y, "*", inv);
		gui_text(11, y, e->title, inv);
		gui_text(GUI_W - 22, y, hm_text(b, e->hm), inv);
	} else if (pg == PG_NOTE) {	/* from, title, the text wrapped */
		const struct dk_note *e = &desk.n[nsel];
		char b[NOTE_COLS + 1];

		if (i == 0) {
			gui_text(gui_text(1, y, e->name, 0) + 5, y,
				 hm_text(b, e->hm), 0);
		} else if (i == 1)
			gui_text(1, y, e->title, 0);
		else {
			strncpy(b, e->body + (i - 2) * NOTE_COLS, NOTE_COLS);
			b[NOTE_COLS] = 0;
			gui_text(1, y, b, 0);
		}
	}
	else if (pg == PG_SYS) {
		gui_text(6, y, sys_tab[i].name, inv);
		if (sys_tab[i].cmd[0] == 0 && i != SYS_LAUNCHER)
			gui_text(50, y, "(later)", inv);
	} else if (pg == PG_SET) {
		on = opt & set_bit[i];
		gui_text(6, y, set_name[i], inv);
		gui_text(42, y, on ? "[on]" : "[off]", inv);
	} else if (opt & OPT_ICONS) {
		if (has_icon & (1 << i)) gui_bits8(6, y, ICON(i), ICON_H, inv);
		gui_text(16, y, apps[i].label, inv);
	} else
		gui_text(6, y, apps[i].label, inv);
}

/* /dev/power's "soc=N ... chg=C": the charge, and charging (the charger's
 * status 1 pre-charge or 2 fast charge, BQ25896). */
static void bat_read(void)
{
	char b[120], *p;
	int fd, n, soc = -1, chg = 0;
	struct tms tm;
	long t = (long) times(&tm);

	if ((unsigned short) (t / CLK_TCK - bat_t) < 60) return;
	bat = BAT_NONE;
	bat_t = (unsigned short) (t / CLK_TCK);
	if ((fd = open("/dev/power", O_RDONLY)) < 0) return;
	n = read(fd, b, sizeof b - 1);
	close(fd);
	if (n <= 0) return;
	b[n] = 0;
	for (p = b; *p != 0; p++) {	/* "soc=N", "chg=C": by hand (no
					 * strstr/atoi: GOT slots in RAM) */
		int *v = p[0] == 's' && p[1] == 'o' && p[2] == 'c' && p[3] == '='
			? &soc : p[0] == 'c' && p[1] == 'h' && p[2] == 'g'
			&& p[3] == '=' ? &chg : NULL;

		if (v == NULL) continue;
		for (*v = 0, p += 4; *p >= '0' && *p <= '9'; p++)
			*v = *v * 10 + *p - '0';
		p--;
	}
	if (soc < 0 || soc > 100) return;
	bat = (unsigned char) soc | (chg == 1 || chg == 2 ? BAT_CHG : 0);
}

/* Read again at the next frame (start, an app ran). */
static void bat_stale(void)
{
	struct tms tm;

	bat_t = (unsigned short) ((long) times(&tm) / CLK_TCK - 60);
}

/* On the black title bar, left: a battery 11x5 (and its tip) filled
 * with the charge from the right, the percentage, a bolt while charging
 * (orange on black). */
static void bat_draw(void)
{
	static const unsigned char bolt[5] = { 0x20, 0x40, 0xE0, 0x40, 0x80 };
	char t[6], *q = t + 5;
	int soc = bat & ~BAT_CHG, w, x;

	if (soc == BAT_NONE) return;
	gui_hline(1, 11, 1, 0);
	gui_hline(1, 11, 5, 0);
	gui_vline(1, 1, 5, 0);
	gui_vline(11, 1, 5, 0);
	gui_vline(12, 2, 4, 0);
	/* inside x 2..10, y 2..4: full is solid; the charge empties it
	 * from the left (owner 2026-11-17) */
	w = (soc * 9 + 50) / 100;
	if (w > 0) gui_fill(11 - w, 2, 10, 4, 0);
	*q = 0;
	*--q = '%';
	do *--q = (char) ('0' + soc % 10); while ((soc /= 10) > 0);
	x = 15 + gui_text(15, 1, q, 1);
	if (bat & BAT_CHG) gui_bits8(x + 1, 1, bolt, 5, 1);
}

/* "n/m" into p (no sprintf: it would bring stdio's data along). */
static char *pos_text(char *p, int n, int m)
{
	char *q = p + 11;

	*q = 0;
	do *--q = (char) ('0' + m % 10); while ((m /= 10) > 0);
	*--q = '/';
	do *--q = (char) ('0' + n % 10); while ((n /= 10) > 0);
	return q;
}

/* The screen is lit (/dev/backlight, HAL_BL_OFF clear); no device: yes. */
static int screen_on(void)
{
	unsigned long st;

	if (bl_fd < 0) return 1;
	lseek(bl_fd, 0L, SEEK_SET);
	if (read(bl_fd, (char *) &st, 4) != 4) return 1;
	return !(st & 0x08000000UL);
}

/* The right of the status line, right to left while there is room (x0:
 * the first column free right of the title): the time (only while the
 * screen is lit: after a dark spell it would be old until the next key),
 * the unread notifications, a module's status, the position on the list. */
static void status_right(int x0, int pg, int sel, int n)
{
	char b[12], *t;
	int x = GUI_W - 1, part, w;

	clock_shown = 0;
	for (part = 0; part < 4; part++) {
		t = NULL;
		if (part == 0 && screen_on()) {
			time_t now = time((time_t *) NULL);
			struct tm *tm = localtime(&now);

			shown_min = now / 60;
			clock_shown = 1;
			b[0] = (char) ('0' + tm->tm_hour / 10);
			b[1] = (char) ('0' + tm->tm_hour % 10);
			b[2] = ':';
			b[3] = (char) ('0' + tm->tm_min / 10);
			b[4] = (char) ('0' + tm->tm_min % 10);
			b[5] = 0;
			t = b;
		} else if (part == 1 && dk_unread(&desk) > 0) {
			t = pos_text(b, 0, dk_unread(&desk)) + 2;
			*--t = '*';		/* "*3": over "0/3" */
		} else if (part == 2 && desk.st >= 0) {
			memcpy(b, desk.t[(int) desk.st].text, DK_STATUS);
			b[DK_STATUS] = 0;
			t = b;
		} else if (part == 3 && (opt & OPT_HINTS) && n > 0
			   && (pg == PG_APPS || pg == PG_NOTES))
			t = pos_text(b, sel + 1, n);
		else if (part == 3 && (opt & OPT_HINTS) && pg == PG_NOTE)
			t = pos_text(b, nsel + 1, desk.notes);
		if (t == NULL || *t == 0) continue;
		w = (int) strlen(t) * 4;
		if (x - w < x0 + 2) continue;
		x -= w;
		gui_text(x, 1, t, 1);
		x -= 4;
	}
}

/* Draw a page: the status line (title bar), rows, the scroll mark, and
 * the hint line or a notice in its place. */
static void draw(int pg, int sel, int top, const char *note)
{
	const struct page *p = &pages[pg];
	int hints = (opt & OPT_HINTS) != 0;
	int vis = GUI_ROWS - 1 - hints, n = page_rows(pg), i, y;
	const char *title = fsck_err && pg == PG_APPS
				? "CARD HAS ERRORS: RUN FSCK" : p->title;

	gui_clear();
	gui_title(title);
	if (opt & OPT_BATT) {
		bat_read();
		bat_draw();
	}
	status_right((GUI_W + (int) strlen(title) * 4) / 2, pg, sel, n);
	if (pg == PG_NOTE) sel = -1;		/* text: no selection */
	for (i = 0; i < vis && top + i < n; i++) {
		y = ROW_Y(i);
		if (top + i == sel) gui_fill(0, y - 1, GUI_W - 1, y + 5, 1);
		page_row(pg, top + i, y, top + i == sel);
		if (top + i == sel) gui_text(1, y, ">", 1);
	}
	if (n > vis) {			/* the scroll mark at x = 127 */
		int h = vis * 6, len = h * vis / n, off = h * top / n;

		if (len < 2) len = 2;
		for (i = 6 + off; i < 6 + off + len; i++)	/* on the selected
							 * row's bar: inverted;
							 * y 6 is the title's */
			if (i > 6) gui_pixel(GUI_W - 1, i, i < ROW_Y(sel - top) - 1
						|| i > ROW_Y(sel - top) + 5);
	}
	if (pg == PG_APPS && n == 0) gui_center(30, "no apps in /etc/apps", 0);
	if (pg == PG_HOME && n == 0) gui_center(30, "no tiles", 0);
	if (pg == PG_NOTES && n == 0) gui_center(30, "no notifications", 0);
	if (pg == PG_RUN) {		/* R12.20 (park) is not there yet */
		gui_center(24, "nothing running", 0);
		gui_center(36, "(parking apps comes later)", 0);
	}
	if (hints) {
		gui_hline(0, GUI_W - 1, HINT_Y - 2, 1);
		gui_text(1, HINT_Y, note != NULL ? note : p->hints, 0);
	}
	gui_flush();
}

/*===========================================================================*
 *				Running an app				     *
 *===========================================================================*/
/* The gid of group "gui" in /etc/group, or -1.  Read in small pieces on
 * the stack: getgrnam() would keep 1.3 KB of buffers in .bss for this one
 * number (the room the desktop's tiles take, spec K.6). */
static int gui_gid(void)
{
	static const char want[] = "gui";
	char b[64], ch;
	int fd = open("/etc/group", O_RDONLY), n, i, col = 0, field = 0;
	int match = 1, digits = 0;
	long gid = 0;

	if (fd < 0) return -1;
	while ((n = read(fd, b, sizeof b)) > 0) {
		for (i = 0; i < n; i++) {
			ch = b[i];
			if (ch == '\n') {
				if (field == 2 && match && digits > 0) break;
				col = field = digits = 0;
				match = 1;
				gid = 0;
			} else if (ch == ':') {
				if (field == 0 && col != 3) match = 0;
				if (++field == 3 && match && digits > 0) {
					close(fd);
					return (int) gid;
				}
				col = 0;
			} else if (field == 0) {
				if (col >= 3 || ch != want[col]) match = 0;
				col++;
			} else if (field == 2 && match) {
				if (ch < '0' || ch > '9' || gid > 30000) match = 0;
				else {
					gid = gid * 10 + ch - '0';
					digits++;
				}
			}
		}
		if (i < n) break;		/* "gui:*:10" with no members */
	}
	close(fd);
	return field == 2 && match && digits > 0 ? (int) gid : -1;
}

/* Run an app as its user and wait for it.  NULL, or a notice to show. */
static char *run(struct app *ap)
{
	char cmd[CMDMAX], *argv[MAXARGS + 1], *p;
	struct passwd *pw;
	int argc = 0, st, fg, gid;
	pid_t pid;

	strcpy(cmd, ap->cmd);
	for (p = strtok(cmd, " "); p != NULL && argc < MAXARGS; p = strtok(NULL, " "))
		argv[argc++] = p;
	argv[argc] = NULL;
	if (argc == 0) return NULL;

	if ((pw = getpwnam(ap->user)) == NULL) {
		if (opt & OPT_HINTS) return "unknown user";
		gui_clear();			/* the old full-screen notice */
		gui_center(24, "unknown user:", 0);
		gui_center(32, ap->user, 0);
		gui_flush();
		sleep(2);
		return NULL;
	}
	if (ap->cmd[0] == '=')		/* a window app: the launcher draws */
		return win_run(ap->label, ap->cmd + 1, pw, (opt & OPT_LOG) != 0);
	win_reap(1);			/* a closed window app still alive */
	gui_close();			/* the app opens the devices itself */
	fg = open("/dev/lcd", O_RDONLY);	/* R4b.8: the hand-over */
	gid = gui_gid();		/* before: the child only execs */
	{	/* R12.22: the app goes low in RAM, idle images stay high, so a
		 * swapped one finds its addresses free again; never swapped */
		message m;

		m.m1_i2 = MI_FGEXEC;
		(void) _syscall(MM, MEMINFO, &m);
	}
	if ((pid = vfork()) == 0) {	/* R2c.2; the child only sets ids,
					 * changes directory and execs */
		if (pw->pw_uid != 0) {
			if (setgid(gid >= 0 ? (gid_t) gid : pw->pw_gid) < 0
			    || setuid(pw->pw_uid) < 0)
				_exit(126);	/* never run with root rights */
		}
		chdir(pw->pw_dir[0] != 0 ? pw->pw_dir : "/");
		execv(argv[0], argv);
		_exit(127);
	}
	/* The app owns the screen and buttons now: long Back ends it.  (Its
	 * first frames, before this, are not dropped: nobody owned them.) */
	if (pid > 0 && fg >= 0) (void) ioctl(fg, FGIOCSET, &pid);
	if (pid > 0) while (waitpid(pid, &st, 0) < 0) ;
	if (fg >= 0) {
		int none = 0;

		(void) ioctl(fg, FGIOCSET, &none);	/* the app is gone */
		close(fg);
	}
	if (gui_open() < 0) exit(1);
	if (pid < 0) return "cannot start";
	if (WIFEXITED(st) && WEXITSTATUS(st) == 127) return "not found";
	if (WIFSIGNALED(st) && WTERMSIG(st) != SIGTERM
	    && WTERMSIG(st) != SIGKILL) return "app crashed";
	return NULL;
}

/*===========================================================================*
 *				The desktop's FIFO			     *
 *===========================================================================*/
/* /var/run/gui (spec K.6): a FIFO, root's, 0620, group gui; made or fixed
 * here (written only when it is not so already).  Opened without
 * blocking, and once more for writing, so that a FIFO with no module
 * never reads as closed (a read is EAGAIN until a record comes). */
static void desk_open(void)
{
	struct stat st;
	int g = gui_gid();
	gid_t gid = g >= 0 ? (gid_t) g : 0;

	desk.st = -1;
	if ((bl_fd = open("/dev/backlight", O_RDONLY)) >= 0)
		(void) fcntl(bl_fd, F_SETFD, FD_CLOEXEC);
	(void) mkdir("/var/run", 0755);
	if (stat(DK_PATH, &st) == 0 && !S_ISFIFO(st.st_mode))
		(void) unlink(DK_PATH);
	if (stat(DK_PATH, &st) < 0) {
		if (mkfifo(DK_PATH, 0600) < 0 || stat(DK_PATH, &st) < 0) return;
	}
	if (st.st_uid != 0 || st.st_gid != gid) (void) chown(DK_PATH, 0, gid);
	if ((st.st_mode & 07777) != 0620) (void) chmod(DK_PATH, 0620);
	if ((dk_rfd = open(DK_PATH, O_RDONLY | O_NONBLOCK)) < 0) return;
	if ((dk_wfd = open(DK_PATH, O_WRONLY | O_NONBLOCK)) < 0) {
		close(dk_rfd);
		dk_rfd = -1;
		return;
	}
	(void) fcntl(dk_rfd, F_SETFD, FD_CLOEXEC);	/* not the apps' */
	(void) fcntl(dk_wfd, F_SETFD, FD_CLOEXEC);
}

static int alive(int pid)
{
	return kill(pid, 0) == 0 || errno == EPERM;
}

/* Read the records waiting in the FIFO: what changed (DC_* bits).  A
 * tile or status of a module that has exited is dropped (a notification
 * is not: it owns nothing, and its sender may be gone already).  A
 * record is written whole (one write() of at most 128 B), so a read that
 * ends inside one finds its rest in the FIFO at once: the line buffer is
 * on the stack, nothing is kept between calls but the "overlong" mark.
 * At most 4 reads at a record's end, then back to poll() (keys first). */
static int desk_read(void)
{
	char buf[2 * DK_LINE], *p, *nl;
	int n, len = 0, c = 0, reads = 0;
	struct dk_rec r;

	for (;;) {
		n = read(dk_rfd, buf + len, sizeof buf - 1 - len);
		if (n <= 0) break;		/* EAGAIN: empty */
		len += n;
		p = buf;
		while ((nl = memchr(p, '\n', (size_t) (buf + len - p))) != NULL) {
			*nl = 0;
			if (dk_skip)
				dk_skip = 0;
			else if (dk_parse(p, &r) > 0 && (r.verb == DV_NOTIFY
				 || r.verb == DV_CLOSE || alive(r.pid))) {
				c |= dk_apply(&desk, &r);	/* else dropped */
				if (r.verb == DV_NOTIFY) {	/* its time */
					time_t now = time((time_t *) NULL);
					struct tm *tm = localtime(&now);

					desk.n[0].hm = (unsigned short)
						(tm->tm_hour * 60 + tm->tm_min);
					if (dk_new < DK_NNOTES) dk_new++;
				}
			}
			p = nl + 1;
		}
		len -= (int) (p - buf);
		memmove(buf, p, (size_t) len);
		if (len >= DK_LINE) {		/* overlong: up to its '\n' */
			dk_skip = 1;
			len = 0;
		}
		if (len == 0 && ++reads >= 4) break;
	}
	return c & ~DC_FULL;
}

/* ms until the clock's next minute: only while it is on the screen and
 * the screen is lit (else -1: no wake-ups for it). */
static int minute_ms(void)
{
	time_t now;

	if (!clock_shown || !screen_on()) return -1;
	now = time((time_t *) NULL);
	if (now / 60 != shown_min) return 0;
	return (int) (60 - now % 60) * 1000;
}

static long now_ticks(void)
{
	struct tms tm;

	return (long) times(&tm);
}

int main(int argc, char **argv)
{
	struct gui_in in;
	int pg = PG_APPS, sel = 0, top = 0, k, n, vis, i, m, c;
	int was = 0, saved = 0, dirty = 1;
	long note_t = 0;
	char *note = NULL;
	char *apps_file = "/etc/apps", *conf_file = "/etc/launcher.conf";

	for (i = 1; i + 1 < argc; i += 2) {
		if (strcmp(argv[i], "-a") == 0) apps_file = argv[i + 1];
		else if (strcmp(argv[i], "-c") == 0) conf_file = argv[i + 1];
	}
	signal(SIGHUP, SIG_IGN);
	signal(SIGINT, SIG_IGN);
	if (gui_open() < 0) {
		fprintf(stderr, "launcher: cannot open /dev/lcd or /dev/buttons\n");
		return 1;
	}
	load_conf(conf_file);
	load_apps(apps_file);
	desk_open();
	bat_stale();				/* read at the first frame */
	memset(&in, 0, sizeof in);
	for (;;) {
		n = page_rows(pg);
		vis = GUI_ROWS - 1 - ((opt & OPT_HINTS) != 0);
		if (sel >= n) sel = n > 0 ? n - 1 : 0;
		if (sel < top) top = sel;
		if (sel >= top + vis) top = sel - vis + 1;
		if (top > 0 && top + vis > n) top = n > vis ? n - vis : 0;
		/* a frame only when something changed (spec K.6) */
		if (dirty) draw(pg, sel, top, note);
		dirty = 0;
		/* the next thing to do without a key: a closed window app's
		 * SIGTERM, the end of a notice, the clock's minute */
		win_reap(0);
		i = win_pending();
		if (note != NULL) {
			m = NOTE_MS - (int) ((now_ticks() - note_t) * 1000 / CLK_TCK);
			if (m < 0) m = 0;
			if (i < 0 || m < i) i = m;
		}
		if ((m = minute_ms()) >= 0 && (i < 0 || m < i)) i = m;
		if (i < 0 && clock_shown) {	/* the light went off: no old
						 * time when it comes back */
			dirty = 1;
			continue;
		}
		k = gui_wait(&in, KEY_LEFT | KEY_RIGHT, i, dk_rfd);
		if (k == GUI_FD) {		/* records from modules */
			dk_new = 0;
			c = desk_read();
			if (c & DC_NOTE) {
				note = desk.n[0].title;
				note_t = now_ticks();
				/* the rows move down: keep the one in hand */
				if (pg == PG_NOTES && sel > 0) sel += dk_new;
				if (pg == PG_NOTE && (nsel += dk_new) >= desk.notes) {
					pg = PG_NOTES;	/* it was pushed out */
					sel = desk.notes - 1;
				}
			}
			if ((c & (DC_STATUS | DC_NOTE))
			    || ((c & DC_TILES) && pg == PG_HOME)) dirty = 1;
			continue;
		}
		if (k == 0) {
			if (note != NULL && now_ticks() - note_t
					>= (long) NOTE_MS * CLK_TCK / 1000) {
				note = NULL;
				dirty = 1;
			}
			if (clock_shown && time((time_t *) NULL) / 60
						!= shown_min) dirty = 1;
			if (pg == PG_HOME && dk_purge(&desk, alive)) dirty = 1;
			continue;
		}
		note = NULL;
		dirty = 1;
		if (pg == PG_HOME) (void) dk_purge(&desk, alive);
		if (k == KEY_UP) sel = sel > 0 ? sel - 1 : (n > 0 ? n - 1 : 0);
		else if (k == KEY_DOWN) sel = sel < n - 1 ? sel + 1 : 0;
		else if (pg == PG_APPS) {
			if (k == KEY_OK && napps > 0) {
				note = run(&apps[sel]);
				note_t = now_ticks();
				memset(&in, 0, sizeof in);	/* the app had them */
				load_conf(conf_file);
				load_apps(apps_file);
				bat_stale();
			} else if (k == KEY_LEFT && pages[pg].left >= 0) {
				was = sel;
				saved = opt;
				pg = pages[pg].left;
				sel = top = 0;
			} else if (k == KEY_RIGHT && pages[pg].right >= 0) {
				was = sel;
				pg = pages[pg].right;
				sel = top = 0;
				(void) dk_purge(&desk, alive);
			}
		} else if (pg == PG_HOME || pg == PG_RUN) {
			if (k == KEY_LEFT) {
				pg = pages[pg].left;
				sel = pg == PG_APPS ? was : 0;
				top = 0;
			} else if (k == KEY_RIGHT && pages[pg].right >= 0) {
				pg = pages[pg].right;
				sel = top = 0;
			}
		} else if (pg == PG_NOTES) {	/* T23c */
			if (k == KEY_LEFT || k == KEY_RIGHT) {
				pg = k == KEY_LEFT ? PG_HOME : PG_RUN;
				sel = top = 0;
				if (pg == PG_HOME) (void) dk_purge(&desk, alive);
			} else if (k == KEY_OK && desk.notes > 0) {
				nsel = sel;
				desk.n[nsel].unread = 0;
				pg = PG_NOTE;
				top = 0;
			} else if (k == KEY_BACK && desk.notes > 0)
				dk_clear(&desk, sel);
		} else if (pg == PG_NOTE) {	/* OK keeps it, Back clears it */
			if (k == KEY_OK || k == KEY_BACK) {
				if (k == KEY_BACK) dk_clear(&desk, nsel);
				pg = PG_NOTES;
				sel = nsel;
				top = 0;
			}
		} else if (pg == PG_SYS) {
			if (k == KEY_OK && sel == SYS_LAUNCHER) {
				pg = PG_SET;
				sel = top = 0;
			} else if (k == KEY_OK && sys_tab[sel].cmd[0] != 0) {
				struct app a;
				char cmd[sizeof sys_tab[0].cmd], user[8], name[9];

				strcpy(cmd, sys_tab[sel].cmd);
				strcpy(user, sys_tab[sel].user);
				strcpy(name, sys_tab[sel].name);
				a.label = name;
				a.user = user;
				a.cmd = cmd;
				note = run(&a);
				note_t = now_ticks();
				memset(&in, 0, sizeof in);
				bat_stale();
			} else if (k == KEY_OK) {
				note = "not there yet";
				note_t = now_ticks();
			} else if (k == KEY_BACK) {
				if (opt != saved) save_conf(conf_file);	/* only a change */
				pg = PG_APPS;
				sel = was;
				top = 0;
			}
		} else if (pg == PG_SET) {
			if (k == KEY_OK || k == KEY_LEFT || k == KEY_RIGHT)
				opt ^= set_bit[sel];
			else if (k == KEY_BACK) {
				pg = PG_SYS;
				sel = SYS_LAUNCHER;
				top = 0;
			}
		}
	}
}
