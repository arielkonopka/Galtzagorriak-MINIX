/* timed - fire the due timers of /var/timers, arm the RTC alarm for the
 * next one, exit (Galtzagorriak MINIX, spec F.2, R7b.3, R7b.4).
 *
 * Usage:	/usr/lib/timed [-v] [-n SECONDS]
 *	-v	say what it fires (with the pid it started) and what it
 *		arms
 *	-n	take SECONDS since 1970 (UTC) as the time now: tests, and
 *		QEMU, which has no RTC alarm
 *
 * INIT runs it at boot and on SIGRTC (the alarm fired, or the clock was
 * set), at most one at a time; nothing of it stays in RAM.  For each
 * timer file (libapp, flipper/lib/app/include/app.h) it looks at the
 * occurrences since its last run (/var/timers/.last): the latest one on
 * time (at most APP_GRACE late) fires; a missed one (power-off, the clock
 * moved forward) fires once or is skipped, as Missed: says.  Then the
 * earliest next time of all timers goes into RTC Alarm A.
 *
 * notify: /usr/lib/alert shows it, as the policy (/etc/notify.conf, the
 * owner's ~/.config/notify) allows.  run: the program is started as the
 * file's owner with --timer ID.  A file that is not a plain file of mode
 * 0600 with one link is ignored: the spool is writable by group timers,
 * and timed runs as root.  /var/timers is on the SD card's root file
 * system; .last is the one write per run (none to internal flash).
 *
 * A program or alert that cannot start (no memory hole, no such program)
 * does not lose its timer (#272): the file gets "Try: N" and "Retry: T",
 * T = now + 60 s, up to 10 tries (APP_TRIES), then the timer is dropped
 * ("gave up").  A repeating timer keeps its schedule; only the occurrence
 * that failed is tried again.  One small write to the card per try.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "app.h"

static int verbose;
static char tzenv[64] = "TZ=";
static char henv[48] = "HOME=";
static char *runenv[] = { henv, "PATH=/bin:/usr/bin", tzenv, NULL };

static void say(const char *a, const char *b, time_t n)
{
	char nb[24], *p = nb + sizeof nb;
	unsigned long long v = (unsigned long long) n;

	write(2, "timed: ", 7);
	write(2, a, strlen(a));
	if (b != NULL) write(2, b, strlen(b));
	if (n != 0) {
		*--p = 0;
		do *--p = '0' + (int) (v % 10); while ((v /= 10) != 0);
		*--p = ' ';
		write(2, p, strlen(p));
	}
	write(2, "\n", 1);
}

static time_t number(const char *s)
{
	time_t v = 0;

	while (*s >= '0' && *s <= '9') v = v * 10 + *s++ - '0';
	return *s == 0 || *s == '\n' ? v : -1;
}

/* INIT starts us with an empty environment: the zone is /etc/TZ (its
 * first line that is not a comment; spec I.1), else UTC. */
static void zone(void)
{
	char buf[128], *k, *p;

	if (getenv("TZ") != NULL) {
		strncpy(tzenv + 3, getenv("TZ"), sizeof tzenv - 4);
		return;
	}
	strcpy(tzenv + 3, "UTC0");
	if (ff_load("/etc/TZ", buf, sizeof buf) >= 0)
		for (p = buf; *p != 0; p = k) {
			for (k = p; *k != 0 && *k != '\n'; k++) ;
			if (*k == '\n') *k++ = 0;
			if (*p != '#' && *p != 0 && strlen(p) < sizeof tzenv - 4) {
				strcpy(tzenv + 3, p);
				break;
			}
		}
	putenv(tzenv);
}

/* Starts what the timer says.  0: it could not start (try again), 1: it
 * was started, held back, or cannot ever start. */
static int fire(struct app_timer *t, struct stat *st, time_t now)
{
	struct notify_policy p;
	struct passwd *pw;
	char why[88];
	volatile int err;
	int f, pid;

	if ((pw = getpwuid(st->st_uid)) == NULL) {
		say(t->id, ": no such user, not fired", 0);
		return 1;
	}
	if (t->action == TA_RUN) {
		strcpy(henv + 5, "/");
		if (strlen(pw->pw_dir) < sizeof henv - 5)
			strcpy(henv + 5, pw->pw_dir);
		/* vfork: the child runs in this frame and we wait until its
		 * exec or _exit, so what it stores in err we see here.  An
		 * exec that fails (no memory hole: EAGAIN; no such program)
		 * must not pass for "fired" (#253). */
		err = 0;
		if ((pid = vfork()) == 0) {
			if (setgid(pw->pw_gid) == 0 && setuid(st->st_uid) == 0
			    && getuid() == st->st_uid)
				execle(t->prog, t->prog, "--timer", t->id,
							(char *) NULL, runenv);
			err = errno != 0 ? errno : EPERM;
			_exit(127);
		}
		if (pid >= 0 && err != 0) {
			errno = err;
			pid = -1;
		}
	} else {
		notify_policy(&p, pw->pw_dir);
		if ((f = notify_flags(&p, t->cat, t->flags, now)) == 0) {
			if (verbose) say(t->id, ": held (do not disturb)", 0);
			return 1;
		}
		pid = app_alert(t, t->cat, f, notify_volume(&p, t, t->cat),
							(int) st->st_uid);
	}
	if (pid < 0) {
		int k = t->tries + 1;

		strcpy(why, ": cannot start it: ");
		strncat(why, strerror(errno), 30);
		strcat(why, " (try ");
		if (k >= 10) strcat(why, "1");
		why[strlen(why) + 1] = 0;
		why[strlen(why)] = '0' + k % 10;
		strcat(why, "/10)");
		say(t->id, why, 0);
		return 0;
	}
	if (verbose) say(t->id, ": fired, pid", pid);
	return 1;
}

/* Write the timer file again with its retry state: a new file owned by
 * the timer's owner (it is started as that user), mode 600, then
 * rename().  Not app_save(): that file would be root's for a moment, and
 * a root-owned timer runs as root. */
static int rewrite(char *buf, const char *id, const struct app_timer *t,
							const struct stat *st)
{
	char tmp[40];
	int fd, n, e;

	if ((n = timer_format(t, buf, APP_FILE_MAX)) < 0) return -1;
	strcpy(tmp, APP_TIMERS "/.retry");
	(void) unlink(tmp);
	if ((fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0) return -1;
	if (write(fd, buf, n) != n || close(fd) < 0
	    || chown(tmp, st->st_uid, st->st_gid) < 0) {
		e = errno;
		(void) unlink(tmp);
		errno = e;
		return -1;
	}
	strcpy(buf, APP_TIMERS "/");
	strcat(buf, id);
	if (rename(tmp, buf) < 0) {
		e = errno;
		(void) unlink(tmp);
		errno = e;
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	static char path[40], buf[APP_FILE_MAX];
	struct app_timer t;
	struct stat st;
	struct dirent *e;
	DIR *d;
	time_t now = 0, last, next = 0, n;
	int i, due, fd, m, r = 0, go, had;
	static char next_id[APP_ID_MAX + 1];

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0) verbose = 1;
		else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc
			 && (now = number(argv[++i])) > 0) ;
		else {
			say("usage: timed [-v] [-n SECONDS]", NULL, 0);
			return 2;
		}
	}
	if (geteuid() != 0) {
		say("only root runs timed", NULL, 0);
		return 1;
	}
	zone();
	if (now == 0) now = time((time_t *) 0);
	last = ff_load(APP_LAST, buf, sizeof buf) > 0 ? number(buf) : -1;
	if (last <= 0) last = now - APP_GRACE;	/* first run */
	if (last > now) last = now;		/* the clock went back */

	if ((d = opendir(APP_TIMERS)) == NULL) {
		say(APP_TIMERS ": ", strerror(errno), 0);
		(void) app_arm(0, 1);
		return 1;
	}
	while ((e = readdir(d)) != NULL) {
		if (!app_id_ok(e->d_name)) continue;	/* ., .., .last */
		strcpy(path, APP_TIMERS "/");
		strcat(path, e->d_name);
		/* Checked on the open file (fstat), not the name: a group
		 * timers user could swap the name for a link to a root file
		 * between a stat() and the read.  The stat() first keeps
		 * timed from opening a device or FIFO linked in. */
		if (stat(path, &st) < 0 || !S_ISREG(st.st_mode)
		    || (fd = open(path, O_RDONLY | O_NONBLOCK)) < 0) {
			say(e->d_name, ": not a plain file of mode 600, ignored", 0);
			continue;
		}
		if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)
		    || (st.st_mode & 07777) != 0600 || st.st_nlink != 1) {
			close(fd);
			say(e->d_name, ": not a plain file of mode 600, ignored", 0);
			continue;
		}
		m = 0;
		while (m < (int) sizeof buf
		       && (r = read(fd, buf + m, sizeof buf - m)) > 0) m += r;
		close(fd);
		if (r < 0 || m >= (int) sizeof buf || (buf[m] = 0,
		    strcpy(t.id, e->d_name), timer_parse(&t, buf)) < 0) {
			say(e->d_name, ": bad timer file, ignored", 0);
			continue;
		}
		due = timer_due(&t, last, now);
		go = due == TD_FIRE || due == TD_LATE
		     || (t.retry != 0 && t.retry <= now);
		if (go) {
			had = t.tries != 0 || t.retry != 0;
			if (!fire(&t, &st, now)) {
				if (++t.tries >= APP_TRIES) {
					say(t.id, ": gave up after 10 tries", 0);
					t.tries = 0;
					t.retry = 0;
					if (t.kind == TK_ONCE) {
						(void) unlink(path);
						continue;
					}
				} else t.retry = now + APP_RETRY;
				had = 1;
			} else t.tries = 0, t.retry = 0;
			if (had && rewrite(buf, e->d_name, &t, &st) < 0)
				say(e->d_name, ": cannot save the retry", 0);
		} else if (due == TD_SKIP && verbose) say(t.id, ": missed, skipped", 0);
		if (t.kind == TK_ONCE && t.at <= now && t.retry == 0) {
			(void) unlink(path);	/* done with */
			continue;
		}
		n = timer_next(&t, now);
		if (t.retry != 0 && (n == 0 || t.retry < n)) n = t.retry;
		if (n != 0 && (next == 0 || n < next)) {
			next = n;
			strcpy(next_id, e->d_name);
		}
	}
	closedir(d);

	/* The last run, for the next one: what is missed since then. */
	{
		char nb[24], *p = nb + sizeof nb;
		unsigned long long v = (unsigned long long) now;

		*--p = 0;
		*--p = '\n';
		do *--p = '0' + (int) (v % 10); while ((v /= 10) != 0);
		if (app_save(APP_TIMERS, ".last", p, 0644) < 0)
			say(APP_LAST ": ", strerror(errno), 0);
	}
	if (app_arm(next, 1) < 0) {
		say(next ? "no RTC alarm, next at" : "no RTC alarm, no timers",
			NULL, next);
	} else if (verbose) {
		say(next ? "armed for " : "no timers", next ? next_id : NULL,
			next);
	}
	return 0;
}
