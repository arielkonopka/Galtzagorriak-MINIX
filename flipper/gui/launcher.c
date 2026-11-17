/* launcher - the main screen of Galtzagorriak MINIX.
 *
 * Started as root by /etc/rc; it takes over the LCD from the boot screen
 * and shows the apps listed in /etc/apps, one per line:
 *
 *	Label:user:/path/to/program arg ...
 *
 * UP/DOWN choose, OK starts.  The app runs as the given user (e.g.
 * "nobody"), in group "gui" so it may use the screen and buttons; only
 * apps marked "root" keep root's rights.  The launcher waits until the app
 * exits (BACK in every app), then redraws.
 */
#include <lib.h>
#include <minix/callnr.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <minix/fg.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "gui.h"

#define MAXAPPS	16
#define MAXARGS	8
#define VISIBLE	(GUI_ROWS - 1)		/* rows below the title */

#define CMDMAX	64			/* longest command line kept */

/* One app: label, user and command, packed in one malloc'd string
 * (R2a.5: was 16 fixed 100-byte entries in .bss). */
struct app {
	char *label;
	char *user;
	char *cmd;
} apps[MAXAPPS];
int napps;

static void free_apps(void)
{
	while (napps > 0) free(apps[--napps].label);
}

static void load_apps(void)
{
	FILE *f;
	char line[128], *a, *b, *m;
	size_t la, lu, lc;

	free_apps();
	/* the card's copy, else the default in /rom (spec L.3) */
	if ((f = cfopen("/etc/apps", "r")) == NULL) return;
	while (napps < MAXAPPS && fgets(line, sizeof line, f) != NULL) {
		line[strcspn(line, "\n")] = 0;
		if (line[0] == '#' || line[0] == 0) continue;
		if ((a = strchr(line, ':')) == NULL) continue;
		*a++ = 0;
		if ((b = strchr(a, ':')) == NULL) continue;
		*b++ = 0;
		la = strlen(line);
		if (la > 23) line[la = 23] = 0;
		lu = strlen(a);
		if (lu > 11) a[lu = 11] = 0;
		lc = strlen(b);
		if (lc > CMDMAX - 1) b[lc = CMDMAX - 1] = 0;
		if ((m = malloc(la + lu + lc + 3)) == NULL) break;
		apps[napps].label = strcpy(m, line);
		apps[napps].user = strcpy(m + la + 1, a);
		apps[napps].cmd = strcpy(m + la + lu + 2, b);
		napps++;
	}
	fclose(f);
}

static void draw(int sel, int top)
{
	int i, y;

	gui_clear();
	gui_title("GALTZAGORRIAK");
	for (i = 0; i < VISIBLE && top + i < napps; i++) {
		y = 7 + i * 6;
		if (top + i == sel) gui_fill(0, y - 1, GUI_W - 1, y + 5, 1);
		gui_text(6, y, apps[top + i].label, top + i == sel);
		if (top + i == sel) gui_text(1, y, ">", 1);
	}
	if (napps == 0) gui_center(30, "no apps in /etc/apps", 0);
	gui_flush();
}

/* Run an app as its user and wait for it. */
static void run(struct app *ap)
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
	if (argc == 0) return;

	if ((pw = getpwnam(ap->user)) == NULL) {
		gui_clear();
		gui_center(24, "unknown user:", 0);
		gui_center(32, ap->user, 0);
		gui_flush();
		sleep(2);
		return;
	}
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
}

int main(void)
{
	int sel = 0, top = 0, k;

	signal(SIGHUP, SIG_IGN);
	signal(SIGINT, SIG_IGN);
	if (gui_open() < 0) {
		fprintf(stderr, "launcher: cannot open /dev/lcd or /dev/buttons\n");
		return 1;
	}
	load_apps();
	for (;;) {
		if (sel < top) top = sel;
		if (sel >= top + VISIBLE) top = sel - VISIBLE + 1;
		draw(sel, top);
		k = gui_key() & ~KEY_LONG;
		if (k == KEY_UP && sel > 0) sel--;
		else if (k == KEY_UP) sel = napps - 1;
		else if (k == KEY_DOWN && sel < napps - 1) sel++;
		else if (k == KEY_DOWN) sel = 0;
		else if (k == KEY_OK && napps > 0) {
			run(&apps[sel]);
			load_apps();
			if (sel >= napps) sel = 0;
		}
	}
}
