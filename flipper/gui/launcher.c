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
#include <sys/types.h>
#include <sys/wait.h>
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

struct app {
	char label[24];
	char user[12];
	char cmd[64];
} apps[MAXAPPS];
int napps;

static void load_apps(void)
{
	FILE *f;
	char line[128], *a, *b;

	napps = 0;
	/* the card's copy, else the default in /rom (spec L.3) */
	if ((f = cfopen("/etc/apps", "r")) == NULL) return;
	while (napps < MAXAPPS && fgets(line, sizeof line, f) != NULL) {
		line[strcspn(line, "\n")] = 0;
		if (line[0] == '#' || line[0] == 0) continue;
		if ((a = strchr(line, ':')) == NULL) continue;
		*a++ = 0;
		if ((b = strchr(a, ':')) == NULL) continue;
		*b++ = 0;
		strncpy(apps[napps].label, line, sizeof apps[0].label - 1);
		strncpy(apps[napps].user, a, sizeof apps[0].user - 1);
		strncpy(apps[napps].cmd, b, sizeof apps[0].cmd - 1);
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
	char cmd[sizeof ap->cmd], *argv[MAXARGS + 1], *p;
	struct passwd *pw;
	struct group *gr;
	int argc = 0, st;
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
	gr = getgrnam("gui");		/* before: stdio state is ours */
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
	if (pid > 0) while (waitpid(pid, &st, 0) < 0) ;
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
