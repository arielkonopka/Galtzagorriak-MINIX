/* SPDX-License-Identifier: GPL-3.0-or-later
 * badusb-gui - Bad USB, the window app (spec R11.3.4, K.8).
 *
 *	list	Settings, then the scripts in /ext/badusb (/home/flipper/
 *		badusb on MINIX), the .txt files, paged (ROWS=N)
 *	conf	a confirm window: the script, the layout, "Connect the USB
 *		cable to the target. Run?"; nothing runs without it
 *	run	a text window: the state (waiting for the host, running,
 *		press OK, done, error at line N, stopped) and "Line n / total"
 *	set	Layout (en-US and the .kl files), Default delay; saved in
 *		/etc/flipper/badusb.conf only when a value changed (K.7)
 *
 * The script runs in the badusb command, a child with its progress
 * records (badusb -p) on a pipe; the app sleeps in poll() on that pipe
 * and on its events (0 wakeups while nothing happens).  Back on the run
 * window sends the child SIGTERM: it lets every key go and ends within
 * 100 ms; the app then shows the list again.  OK on "Continue" answers a
 * WAIT_FOR_BUTTON_PRESS (a line on the child's stdin).
 *
 * The launcher starts it as the user badusb (group usb: the HID device
 * files), "Bad USB:badusb:@usb:=/usr/bin/badusb-gui" in /etc/apps.  No
 * stdio, no malloc, no opendir() (the directory is read in 16 B entries),
 * no signal handler: no MM gap (K.8).
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "gm.h"
#include "libhid.h"

#define SDIR	"/home/flipper/badusb"		/* /ext/badusb */
#define LDIR1	"/usr/share/hid/layouts"
#define LDIR2	SDIR "/assets/layouts"
#define CONF	"/etc/flipper/badusb.conf"
#define BADUSB	"/usr/bin/badusb"
#define NAMEMAX	60				/* V3 entries (spec O.4) */
#define PAGE	16				/* rows of a list window */
#define MAXDELAY 1000				/* the Default delay row, ms */
#define SHOW_MS	250				/* "Line n" at most this often */
#define TERM_MS	500				/* SIGTERM -> SIGKILL */
#define OPTS	128				/* the layout options */

enum { W_LIST, W_CONF, W_RUN, W_SET };
enum { S_HOST, S_RUN, S_WAIT, S_DONE, S_ERROR, S_STOP };

static struct {
	char script[NAMEMAX + 1];	/* the chosen one */
	char layout[HID_KL_NAME + 1];	/* settings: "" = en-US */
	short delay;
	char cur;			/* W_*: the window the desktop shows */
	short first;			/* the list's first row on the page */
	/* the run */
	pid_t pid;			/* 0: none */
	int pfd, ifd;			/* its progress (read), its stdin */
	char st;			/* S_* */
	char dirty;			/* the run window is out of date */
	int line, total, skipped;
	char msg[48];			/* the error */
	clock_t shown;			/* ticks: the run window last sent */
	char pb[64];			/* progress bytes, a line not ended */
	int pn;
} a;

static long ticks(void)
{
	struct tms t;

	return (long) times(&t);
}

static char *num(char *t, long v)		/* t[12] */
{
	char *p = t + 11;
	unsigned long u = (unsigned long) (v < 0 ? -v : v);

	*p = 0;
	do *--p = (char) ('0' + u % 10); while ((u /= 10) > 0);
	if (v < 0) *--p = '-';
	return p;
}

/* ---- directories, read as raw entries (no opendir: its 1 KB malloc) ---- */

/* The n-th file (from 0) in dir whose name ends in ext, into name, or the
 * count of them when n < 0.  The count, 1 found, 0 not found, -1 no dir.
 * Entries are 16 B (V1/V2: 2-byte inode, 14 characters) or 64 B (a MINIX
 * V3 /home: 4-byte inode, 60 characters, spec O.4), told apart as libc's
 * readdir does: the first entry is ".", whose name is at byte 4 on V3. */
static int dir_scan(const char *dir, const char *ext, int n, char *name)
{
	char b[256];				/* 16 V2 or 4 V3 entries */
	int fd, i, k, len, el = (int) strlen(ext), count = 0, esz = 0, nl;
	char nm[NAMEMAX + 1];

	if ((fd = open(dir, O_RDONLY)) < 0) return -1;
	while ((k = (int) read(fd, b, sizeof b)) > 0) {
		if (esz == 0) esz = k >= 6 && b[4] == '.' && b[5] == 0 ? 64 : 16;
		nl = esz - (esz == 64 ? 4 : 2);
		for (i = 0; i + esz <= k; i += esz) {
			if (b[i] == 0 && b[i + 1] == 0 && (esz == 16
					|| (b[i + 2] == 0 && b[i + 3] == 0)))
				continue;		/* inode 0: free */
			memcpy(nm, b + i + esz - nl, nl);
			nm[nl] = 0;
			len = (int) strlen(nm);
			if (len <= el || strcmp(nm + len - el, ext) != 0
			    || nm[0] == '.')
				continue;
			if (count++ == n) {
				close(fd);
				strcpy(name, nm);
				return 1;
			}
		}
	}
	close(fd);
	return n < 0 ? count : 0;
}

/* ---- settings ---- */

static void conf_read(void)
{
	char b[96], *l, *nl, *v;
	int fd, n;

	a.layout[0] = 0;
	a.delay = 0;
	if ((fd = open(CONF, O_RDONLY)) < 0) return;
	n = (int) read(fd, b, sizeof b - 1);
	close(fd);
	if (n <= 0) return;
	b[n] = 0;
	for (l = b; *l != 0; l = nl) {
		if ((nl = strchr(l, '\n')) != NULL) *nl++ = 0;
		else nl = l + strlen(l);
		if ((v = strchr(l, ':')) == NULL) continue;
		*v++ = 0;
		while (*v == ' ') v++;
		if (strcmp(l, "Layout") == 0 && strlen(v) <= HID_KL_NAME
		    && strchr(v, '/') == NULL)
			strcpy(a.layout, v);
		else if (strcmp(l, "Delay") == 0) {
			n = atoi(v);
			if (n >= 0 && n <= MAXDELAY) a.delay = (short) n;
		}
	}
}

/* Only on a change (K.7): one write when the settings window is left. */
static int conf_write(void)
{
	char b[96], t[12];
	int fd, n, ok;

	strcpy(b, "Layout: ");
	strcat(b, a.layout[0] ? a.layout : HID_KL_US);
	strcat(b, "\nDelay: ");
	strcat(b, num(t, a.delay));
	strcat(b, "\n");
	n = (int) strlen(b);
	if ((fd = open(CONF, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) return -1;
	ok = write(fd, b, (unsigned) n) == n;
	return close(fd) == 0 && ok ? 0 : -1;
}

/* The layout options "en-US|de-DE|..." into o[OPTS]; with want >= 0 the
 * name of option want goes into name (1 found).  The option count. */
static int layouts(char *o, int want, char *name, int *found)
{
	char nm[NAMEMAX + 1];
	const char *dirs[2];
	int d, i, n = 1, len;

	dirs[0] = LDIR1;
	dirs[1] = LDIR2;
	strcpy(o, HID_KL_US);
	if (want == 0) {
		strcpy(name, HID_KL_US);
		*found = 1;
	}
	for (d = 0; d < 2; d++)
		for (i = 0; dir_scan(dirs[d], ".kl", i, nm) == 1; i++) {
			len = (int) strlen(nm) - 3;
			nm[len] = 0;
			if (strcmp(nm, HID_KL_US) == 0) continue;
			/* the same name in both: once */
			{
				const char *p = o;
				int dup = 0, l;

				while (p != NULL && !dup) {
					l = strchr(p, '|') ? (int) (strchr(p, '|') - p)
							   : (int) strlen(p);
					dup = l == len && strncmp(p, nm, (size_t) l) == 0;
					p = strchr(p, '|');
					if (p != NULL) p++;
				}
				if (dup) continue;
			}
			if ((int) strlen(o) + len + 2 > OPTS) return n;
			strcat(o, "|");
			strcat(o, nm);
			if (want == n) {
				strcpy(name, nm);
				*found = 1;
			}
			n++;
		}
	return n;
}

/* The index of the current layout among the options (-1: not there). */
static int layout_index(const char *o)
{
	const char *cur = a.layout[0] ? a.layout : HID_KL_US, *p = o;
	int i = 0, l, len = (int) strlen(cur);

	while (p != NULL) {
		l = strchr(p, '|') ? (int) (strchr(p, '|') - p) : (int) strlen(p);
		if (l == len && strncmp(p, cur, (size_t) l) == 0) return i;
		p = strchr(p, '|');
		if (p != NULL) p++;
		i++;
	}
	return -1;
}

/* ---- windows ---- */

static void w_list(int first)
{
	char key[8], t[12], nm[NAMEMAX + 1];
	int n = dir_scan(SDIR, ".txt", -1, nm), i, row;

	if (n < 0) n = 0;
	if (first < 0 || first > n) first = 0;
	a.first = (short) first;
	gm_win("list", "list", "Bad USB", n + 1 > PAGE ? n + 1 : 0, first);
	for (row = first; row < first + PAGE && row <= n; row++) {
		if (row == 0) {
			gm_item("set", "Settings", a.layout[0] ? a.layout
							      : HID_KL_US);
			continue;
		}
		i = row - 1;
		if (dir_scan(SDIR, ".txt", i, nm) != 1) break;
		nm[strlen(nm) - 4] = 0;
		key[0] = 's';
		strcpy(key + 1, num(t, i));
		gm_item(key, nm, "");
	}
	if (n == 0) {
		gm_line("No scripts: put .txt files");
		gm_line("in /ext/badusb");
	}
	gm_show();
	a.cur = W_LIST;
}

static void w_conf(void)
{
	char l[40], nm[NAMEMAX + 1];

	strcpy(nm, a.script);
	nm[strlen(nm) - 4] = 0;
	gm_win("conf", "confirm", "Bad USB", 0, 0);
	gm_line(nm);
	strcpy(l, "Layout: ");
	strcat(l, a.layout[0] ? a.layout : HID_KL_US);
	gm_line(l);
	gm_line("Connect the USB cable");
	gm_line("to the target. Run?");
	gm_show();
	a.cur = W_CONF;
}

static void w_run(void)
{
	static const char state[][21] = { "Waiting for the host",
		"Running", "Press OK to go on", "Done", "Error", "Stopped" };
	char l[48], t[12], nm[NAMEMAX + 1];

	strcpy(nm, a.script);
	nm[strlen(nm) - 4] = 0;
	gm_win("run", "text", "Bad USB", 0, 0);
	gm_line(nm);
	if (a.st == S_ERROR && a.line > 0) {
		strcpy(l, "Error at line ");
		strcat(l, num(t, a.line));
		gm_line(l);
	} else
		gm_line(state[(int) a.st]);
	if (a.st == S_ERROR) gm_line(a.msg);
	else if (a.st == S_DONE && a.skipped > 0) {
		strcpy(l, num(t, a.skipped));
		strcat(l, " characters left out");
		gm_line(l);
	}
	if (a.line > 0 && a.st != S_ERROR) {
		strcpy(l, "Line ");
		strcat(l, num(t, a.line));
		strcat(l, " / ");
		strcat(l, num(t, a.total));
		gm_line(l);
	}
	if (a.st == S_WAIT) gm_action("go", "Continue");
	gm_show();
	a.cur = W_RUN;
	a.dirty = 0;
	a.shown = (clock_t) ticks();
}

static void w_set(void)
{
	char o[OPTS], nm[NAMEMAX + 1];
	int i, f = 0;

	(void) layouts(o, -1, nm, &f);
	if ((i = layout_index(o)) < 0) i = 0;
	gm_win("set", "list", "Settings", 0, 0);
	gm_enum("lay", "Layout", i, o);
	gm_int("dd", "Default delay", 0, MAXDELAY, 10, a.delay, "ms");
	gm_show();
	a.cur = W_SET;
}

/* ---- the run ---- */

static void run_start(void)
{
	char path[sizeof SDIR + NAMEMAX + 2], dd[12], *dp;
	int p[2], in[2];

	strcpy(path, SDIR "/");
	strcat(path, a.script);
	dp = num(dd, a.delay);
	a.st = S_HOST;
	a.line = a.total = a.skipped = 0;
	a.msg[0] = 0;
	a.pn = 0;
	if (pipe(p) < 0) {
		a.st = S_ERROR;
		strcpy(a.msg, "no pipe");
		return;
	}
	if (pipe(in) < 0) {
		close(p[0]);
		close(p[1]);
		a.st = S_ERROR;
		strcpy(a.msg, "no pipe");
		return;
	}
	if ((a.pid = fork()) == 0) {
		dup2(in[0], 0);
		dup2(p[1], 1);
		close(GM_EVFD);			/* the desktop's pipes stay ours */
		close(GM_RECFD);
		close(p[0]);
		close(p[1]);
		close(in[0]);
		close(in[1]);
		execl(BADUSB, "badusb", "-p", "-l", a.layout[0] ? a.layout
		      : HID_KL_US, "-d", dp, path, (char *) NULL);
		_exit(127);
	}
	close(p[1]);
	close(in[0]);
	if (a.pid < 0) {
		a.pid = 0;
		close(p[0]);
		close(in[1]);
		a.st = S_ERROR;
		strcpy(a.msg, "cannot start badusb");
		return;
	}
	a.pfd = p[0];
	a.ifd = in[1];
}

/* One progress record of badusb -p. */
static void record(char *l)
{
	char *sp;

	switch (l[0]) {
	case 'L':
		a.line = atoi(l + 2);
		sp = strchr(l + 2, ' ');
		if (sp != NULL) a.total = atoi(sp + 1);
		if (a.st != S_WAIT) a.st = S_RUN;
		break;
	case 'H': a.st = S_HOST; break;
	case 'W': a.st = S_WAIT; break;
	case 'R': a.st = S_RUN; break;
	case 'D':
		a.st = S_DONE;
		a.skipped = atoi(l + 2);
		break;
	case 'X': a.st = S_STOP; break;
	case 'E':
		a.st = S_ERROR;
		a.line = atoi(l + 2);
		sp = strchr(l + 2, ' ');
		strncpy(a.msg, sp != NULL ? sp + 1 : "", sizeof a.msg - 1);
		a.msg[sizeof a.msg - 1] = 0;
		break;
	default:
		return;
	}
	a.dirty = 1;
}

/* The child is gone: its status, the pipes closed. */
static void run_end(void)
{
	int status = 0;

	if (a.pid == 0) return;
	close(a.pfd);
	close(a.ifd);
	(void) waitpid(a.pid, &status, 0);
	a.pid = 0;
	if (a.st == S_HOST || a.st == S_RUN || a.st == S_WAIT) {
		if (WIFEXITED(status) && WEXITSTATUS(status) == 3)
			a.st = S_STOP;
		else {
			a.st = S_ERROR;
			strcpy(a.msg, WIFEXITED(status) && WEXITSTATUS(status)
			       == 127 ? "no " BADUSB : "badusb ended");
		}
	}
	a.dirty = 1;
}

/* What badusb wrote; at its end (EOF), run_end(). */
static void progress(void)
{
	char *nl;
	int n;

	n = (int) read(a.pfd, a.pb + a.pn, sizeof a.pb - 1 - (unsigned) a.pn);
	if (n < 0 && errno == EINTR) return;
	if (n <= 0) {
		run_end();
		return;
	}
	a.pn += n;
	a.pb[a.pn] = 0;
	while ((nl = strchr(a.pb, '\n')) != NULL) {
		*nl = 0;
		record(a.pb);
		a.pn -= (int) (nl + 1 - a.pb);
		memmove(a.pb, nl + 1, (unsigned) a.pn + 1);
	}
	if (a.pn == (int) sizeof a.pb - 1) a.pn = 0;	/* overlong: dropped */
}

/* Stop the script: SIGTERM (badusb lets every key go and ends within
 * 100 ms), SIGKILL if it is still there after TERM_MS. */
static void run_stop(void)
{
	struct pollfd p;
	long end;
	int left;

	if (a.pid == 0) return;
	(void) kill(a.pid, SIGTERM);
	end = ticks() + (TERM_MS * CLK_TCK + 999) / 1000;
	while (a.pid != 0) {
		left = (int) ((end - ticks()) * 1000 / CLK_TCK);
		if (left <= 0) {
			(void) kill(a.pid, SIGKILL);
			run_end();
			break;
		}
		p.fd = a.pfd;
		p.events = POLLIN;
		p.revents = 0;
		if (poll(&p, 1, left) > 0) progress();
	}
}

/* ---- events ---- */

static void ok(const char *win, const char *key)
{
	char nm[NAMEMAX + 1];

	if (strcmp(win, "list") == 0) {
		if (strcmp(key, "set") == 0) w_set();
		else if (key[0] == 's'
			 && dir_scan(SDIR, ".txt", atoi(key + 1), nm) == 1) {
			strcpy(a.script, nm);
			w_conf();
		} else
			w_list(a.first);	/* gone meanwhile */
	} else if (strcmp(win, "conf") == 0) {
		if (a.pid != 0) return;
		run_start();
		w_run();
	} else if (strcmp(win, "run") == 0 && strcmp(key, "go") == 0) {
		if (a.pid != 0 && a.st == S_WAIT) {
			(void) write(a.ifd, "\n", 1);
			a.st = S_RUN;
		}
		w_run();
	} else
		(void) gm_busy(0);
}

static void values(char **f)
{
	char o[OPTS], nm[HID_KL_NAME + 1], *k, *v;
	int i, x, found = 0, changed = 0;

	for (i = 2; i < GM_FIELDS && f[i] != NULL; i++) {
		k = gm_kv(f[i], &v);
		x = atoi(v);
		if (strcmp(k, "lay") == 0) {
			found = 0;
			(void) layouts(o, x, nm, &found);
			if (found && strcmp(nm, a.layout[0] ? a.layout
						: HID_KL_US) != 0) {
				strcpy(a.layout, strcmp(nm, HID_KL_US) == 0
				       ? "" : nm);
				changed = 1;
			}
		} else if (strcmp(k, "dd") == 0 && x >= 0 && x <= MAXDELAY
			   && x != a.delay) {
			a.delay = (short) x;
			changed = 1;
		}
	}
	if (changed) (void) conf_write();
}

static void redraw(void)
{
	switch (a.cur) {
	case W_CONF: w_conf(); break;
	case W_RUN: w_run(); break;
	case W_SET: w_set(); break;
	default: w_list(a.first);
	}
}

int main(int argc, char **argv)
{
	char buf[128], ev[128], *f[GM_FIELDS];
	struct pollfd p[2];
	int n, t, wait;

	(void) argv;
	static const char use[] = "badusb-gui: start Bad USB from the "
		"launcher (badusb FILE: run a script)\n";

	if (argc != 1 || gm_init(buf, sizeof buf) < 0) {
		(void) write(2, use, sizeof use - 1);
		return 1;
	}
	conf_read();
	w_list(0);
	for (;;) {
		if (!gm_pending(ev)) {
			/* the run window: at most every SHOW_MS while lines
			 * go by, at once when the state changes */
			wait = -1;
			if (a.dirty && a.cur == W_RUN) {
				t = (int) ((ticks() - (long) a.shown) * 1000 / CLK_TCK);
				if (a.st != S_RUN || t >= SHOW_MS) w_run();
				else wait = SHOW_MS - t;
			}
			p[0].fd = GM_EVFD;
			p[0].events = POLLIN;
			p[0].revents = 0;
			p[1].fd = a.pid != 0 ? a.pfd : -1;
			p[1].events = POLLIN;
			p[1].revents = 0;
			n = poll(p, a.pid != 0 ? 2 : 1, wait);
			if (n <= 0) continue;
			if (a.pid != 0 && p[1].revents != 0) progress();
			if (p[0].revents == 0) continue;
		}
		switch (gm_event(ev, sizeof ev, f, GM_FIELDS)) {
		case GM_OK:
			if (f[1] != NULL && f[2] != NULL) ok(f[1], f[2]);
			break;
		case GM_SUBMIT:
			if (f[1] != NULL && strcmp(f[1], "set") == 0) values(f);
			break;
		case GM_BACK:		/* the desktop shows the parent */
			if (f[1] == NULL) break;
			if (strcmp(f[1], "run") == 0) {
				run_stop();	/* SIGTERM, all keys up */
				w_list(a.first);
			} else
				a.cur = W_LIST;
			break;
		case GM_PAGE:
			if (f[1] != NULL && f[2] != NULL
			    && strcmp(f[1], "list") == 0)
				w_list(atoi(f[2]));
			else
				redraw();
			break;
		case GM_REDRAW:
			redraw();
			break;
		case GM_CLOSE:
			run_stop();
			return 0;
		}
	}
}
