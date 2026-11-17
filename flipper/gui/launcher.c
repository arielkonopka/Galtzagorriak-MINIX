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
 * Pages (R5.6a.4): the app list, and Left to the settings page (icons and
 * the hint line on or off, /etc/launcher.conf).  A page is an entry of
 * pages[]: its name, rows, OK and hint text; later pages (the desktop,
 * spec K.6) are new entries.  Left and Right act on the release of a key
 * pressed alone (gui_input), so the backlight gesture Left+Up (R6.10)
 * never turns a page.  The look is spec R5.6a.7 ("Pine meets modern UX");
 * with icons and hints off it is the old screen, pixel for pixel.
 *
 *	launcher [-a apps] [-c conf]	other files, for the QEMU tests
 */
#include <lib.h>
#include <minix/callnr.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/times.h>
#include <minix/fg.h>
#include <fcntl.h>
#include <grp.h>
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
	char title[14];
	char hints[22];
	signed char left, right;		/* page index, -1: none */
};

#define PG_APPS	0
#define PG_SET	1
static const struct page pages[] = {
	{ "GALTZAGORRIAK", "<Setup  OK Run", PG_SET, -1 },
	{ "LAUNCHER", "OK Toggle  Back Done", -1, -1 },
};

static int page_rows(int pg)
{
	return pg == PG_APPS ? napps : 3;
}

static const char set_name[3][8] = { "Icons", "Hints", "Battery" };
static const unsigned char set_bit[3] = { OPT_ICONS, OPT_HINTS, OPT_BATT };

/* Draw row i of page pg at y. */
static void page_row(int pg, int i, int y, int inv)
{
	int on;

	if (pg == PG_SET) {
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

/* Draw a page: title (with the position on the app list), rows, the
 * scroll mark, and the hint line or a notice in its place. */
static void draw(int pg, int sel, int top, const char *note)
{
	const struct page *p = &pages[pg];
	int hints = (opt & OPT_HINTS) != 0;
	int vis = GUI_ROWS - 1 - hints, n = page_rows(pg), i, y;
	char pos[12], *t;

	gui_clear();
	gui_title(p->title);
	if (opt & OPT_BATT) {
		bat_read();
		bat_draw();
	}
	if (hints && pg == PG_APPS && n > 0) {
		t = pos_text(pos, sel + 1, n);
		gui_text(GUI_W - (int) strlen(t) * 4 - 1, 1, t, 1);
	}
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
	if (hints) {
		gui_hline(0, GUI_W - 1, HINT_Y - 2, 1);
		gui_text(1, HINT_Y, note != NULL ? note : p->hints, 0);
	}
	gui_flush();
}

/*===========================================================================*
 *				Running an app				     *
 *===========================================================================*/
/* Run an app as its user and wait for it.  NULL, or a notice to show. */
static char *run(struct app *ap)
{
	char cmd[CMDMAX], *argv[MAXARGS + 1], *p;
	struct passwd *pw;
	struct group *gr;
	int argc = 0, st, fg;
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
	gr = getgrnam("gui");		/* before: stdio state is ours */
	{	/* R12.22: the app goes low in RAM, idle images stay high, so a
		 * swapped one finds its addresses free again; never swapped */
		message m;

		m.m1_i2 = MI_FGEXEC;
		(void) _syscall(MM, MEMINFO, &m);
	}
	if ((pid = vfork()) == 0) {	/* R2c.2; the child only sets ids,
					 * changes directory and execs */
		if (pw->pw_uid != 0) {
			if (setgid(gr != NULL ? gr->gr_gid : pw->pw_gid) < 0
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

int main(int argc, char **argv)
{
	struct gui_in in;
	int pg = PG_APPS, sel = 0, top = 0, k, n, vis, i, was = 0, saved = 0;
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
	bat_stale();				/* read at the first frame */
	memset(&in, 0, sizeof in);
	for (;;) {
		n = page_rows(pg);
		vis = GUI_ROWS - 1 - ((opt & OPT_HINTS) != 0);
		if (sel >= n) sel = n > 0 ? n - 1 : 0;
		if (sel < top) top = sel;
		if (sel >= top + vis) top = sel - vis + 1;
		if (top > 0 && top + vis > n) top = n > vis ? n - vis : 0;
		draw(pg, sel, top, note);
		/* a notice: the hint line back after NOTE_MS or at a key; a
		 * closed window app: its SIGTERM when its 2 s are over */
		win_reap(0);
		i = win_pending();
		if (note != NULL && (i < 0 || i > NOTE_MS)) i = NOTE_MS;
		k = gui_input(&in, KEY_LEFT | KEY_RIGHT, i);
		if (k == 0 && note != NULL && win_pending() >= 0) {
			win_reap(0);	/* only the window app's time */
			continue;
		}
		note = NULL;
		if (k == 0) continue;
		if (k == KEY_UP) sel = sel > 0 ? sel - 1 : (n > 0 ? n - 1 : 0);
		else if (k == KEY_DOWN) sel = sel < n - 1 ? sel + 1 : 0;
		else if (pg == PG_APPS) {
			if (k == KEY_OK && napps > 0) {
				note = run(&apps[sel]);
				memset(&in, 0, sizeof in);	/* the app had them */
				load_conf(conf_file);
				load_apps(apps_file);
				bat_stale();
			} else if (k == KEY_LEFT && pages[pg].left >= 0) {
				was = sel;
				saved = opt;
				pg = pages[pg].left;
				sel = top = 0;
			}
		} else if (pg == PG_SET) {
			if (k == KEY_OK || k == KEY_LEFT || k == KEY_RIGHT)
				opt ^= set_bit[sel];
			else if (k == KEY_BACK) {
				if (opt != saved) save_conf(conf_file);	/* only a change */
				pg = PG_APPS;
				sel = was;
				top = 0;
			}
		}
	}
}
