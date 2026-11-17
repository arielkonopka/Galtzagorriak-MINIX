/* selftest - the hardware self test (spec 7b.3, owner request 2026-11-14)
 *
 *	selftest		the menu (from the launcher): All tests, then
 *				one line per module; OK runs, Back quits
 *	selftest -a [-n N] [-k]	from a shell: every automatic module, N loops
 *				(default 1), -k keeps looping after a failure;
 *				exit status: the number of failures
 *
 * The modules are programs in /usr/lib/selftest/ (st.h).  Each runs as a
 * child that gets the screen and the buttons (FGIOCSET) while it runs; its
 * result line comes through a pipe and is appended, with the time, to
 * /home/flipper/apps_data/selftest/log.  Nothing here stays in RAM after
 * Back; a module is in RAM only while it runs.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <minix/fg.h>
#include "gui.h"
#include "st.h"

#define NMOD	12

static struct mod {
	char name[14];
	char desc[26];
	char autom;		/* runs in All tests */
} mods[NMOD];
static int nmods, gui;

static void load(void)
{
	static char path[48];
	struct dirent *e;
	DIR *d;
	int fd, n, l;
	char line[40];

	if ((d = opendir(ST_DIR)) == NULL) return;
	while ((e = readdir(d)) != NULL && nmods < NMOD) {
		l = strlen(e->d_name);
		if (l < 3 || l > 15 || strcmp(e->d_name + l - 2, ".d") != 0)
			continue;
		sprintf(path, "%s/%s", ST_DIR, e->d_name);
		if ((fd = open(path, O_RDONLY)) < 0) continue;
		n = read(fd, line, sizeof line - 1);
		close(fd);
		if (n < 3 || (line[0] != 'a' && line[0] != 'i')) continue;
		line[n] = 0;
		line[strcspn(line, "\n")] = 0;
		memcpy(mods[nmods].name, e->d_name, l - 2);
		mods[nmods].name[l - 2] = 0;
		strncpy(mods[nmods].desc, line + 2, sizeof mods[0].desc - 1);
		mods[nmods].autom = line[0] == 'a';
		nmods++;
	}
	closedir(d);
}

static void logit(const char *name, const char *res)
{
	static char line[96];
	struct tm *tm;
	time_t now = time((time_t *) 0);
	int fd;

	(void) mkdir(ST_LOGDIR, 0755);
	if ((fd = open(ST_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644)) < 0)
		return;
	tm = localtime(&now);
	sprintf(line, "%04d-%02d-%02d %02d:%02d:%02d %s %s\n",
		tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
		tm->tm_hour, tm->tm_min, tm->tm_sec, name, res);
	(void) write(fd, line, strlen(line));
	close(fd);
}

/* Hand the screen and the buttons to pid (0: take them back, if we can). */
static void owner(int pid)
{
	int fd;

	if ((fd = open("/dev/lcd", O_RDONLY)) < 0) return;
	if (pid == 0) pid = getpid();
	(void) ioctl(fd, FGIOCSET, &pid);
	close(fd);
}

/* Run one module; its result line in res (64 B).  0: PASS. */
static int run(struct mod *m, int autom, char *res)
{
	static char path[48];
	int out[2], go[2], pid, st, n = 0, r;
	char c;

	strcpy(res, "FAIL cannot start");
	if (pipe(out) != 0) return 1;
	if (pipe(go) != 0) {
		close(out[0]); close(out[1]);
		return 1;
	}
	sprintf(path, "%s/%s", ST_DIR, m->name);
	if (gui) gui_close();
	if ((pid = fork()) == 0) {
		close(out[0]);
		close(go[1]);
		(void) read(go[0], &c, 1);	/* the screen is ours now */
		close(go[0]);
		dup2(out[1], 1);
		close(out[1]);
		execl(path, m->name, autom ? "-a" : (char *) 0, (char *) 0);
		_exit(127);
	}
	close(out[1]);
	close(go[0]);
	if (pid > 0) owner(pid);
	(void) write(go[1], "g", 1);
	close(go[1]);
	while (pid > 0 && n < 63 && (r = read(out[0], res + n, 63 - n)) > 0)
		n += r;
	close(out[0]);
	if (n > 0) {
		res[n] = 0;
		res[strcspn(res, "\n")] = 0;
	}
	if (pid > 0) {
		while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
		if (n == 0) strcpy(res, WIFSIGNALED(st) ? "FAIL stopped"
							: "FAIL no result");
	}
	owner(0);
	if (gui) (void) gui_open();
	logit(m->name, res);
	return strncmp(res, "PASS", 4) != 0 && strncmp(res, "SKIP", 4) != 0;
}

static void draw(int sel)
{
	int i, top = sel > 7 ? sel - 7 : 0;

	gui_clear();
	gui_title("Self test");
	for (i = top; i <= nmods && i < top + 8; i++)
		gui_text(2, 8 + (i - top) * 7,
			i == 0 ? "All tests" : mods[i - 1].desc, i == sel);
	gui_flush();
}

static void show(const char *a, const char *b, const char *c)
{
	gui_clear();
	gui_title("Self test");
	gui_center(18, a, 0);
	gui_center(28, b, 0);
	gui_center(52, c, 0);
	gui_flush();
}

/* All tests: every automatic module, loop after loop. */
static int all(int loops, int keep, int verbose)
{
	static char res[64], line[40], line2[40];
	int i, l, fails = 0, failed;

	for (l = 0; loops == 0 || l < loops; l++) {
		failed = 0;
		for (i = 0; i < nmods; i++) {
			if (!mods[i].autom) continue;
			if (gui && gui_key_nowait() == KEY_BACK) return fails;
			if (run(&mods[i], 1, res) != 0) failed++;
			if (verbose) printf("%s: %s\n", mods[i].name, res);
			if (gui) {
				sprintf(line, "Loop %d: %s", l + 1, mods[i].name);
				strncpy(line2, res, sizeof line2 - 1);
				line2[sizeof line2 - 1] = 0;
				show(line, line2, "Back: stop");
			}
		}
		fails += failed;
		if (failed && !keep) break;
	}
	return fails;
}

int main(int argc, char **argv)
{
	static char res[64], line[40];
	int i, k, sel = 0, loops = 1, keep = 0, cli = 0, f;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-a") == 0) cli = 1;
		else if (strcmp(argv[i], "-k") == 0) keep = 1;
		else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
			loops = atoi(argv[++i]);
		else {
			fprintf(stderr, "Usage: selftest [-a [-n loops] [-k]]\n");
			return 2;
		}
	}
	load();
	if (cli) {
		f = all(loops, keep, 1);
		printf("selftest: %d failure(s)\n", f);
		return f > 125 ? 125 : f;
	}
	signal(SIGINT, SIG_IGN);
	if (gui_open() < 0) return 1;
	gui = 1;
	for (;;) {
		draw(sel);
		k = gui_key();
		if (k == KEY_UP) sel = sel > 0 ? sel - 1 : nmods;
		else if (k == KEY_DOWN) sel = sel < nmods ? sel + 1 : 0;
		else if (k == KEY_BACK) break;
		else if (k == KEY_OK) {
			if (sel == 0) {
				f = all(0, 0, 0);
				sprintf(line, "%d failure(s)", f);
				show("All tests stopped", line, "any key");
			} else {
				(void) run(&mods[sel - 1], 0, res);
				show(mods[sel - 1].desc, res, "any key");
			}
			(void) gui_key();
		}
	}
	gui_clear();
	gui_flush();
	gui_close();
	return 0;
}
