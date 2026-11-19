/* io.c - libapp: the timer spool, the RTC alarm, alerts, the policy files
 * and settings (spec F.2, R7b.4).  See app.h.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "app.h"
#ifdef __minix
#include <sys/ioctl.h>
#include <minix/rtc.h>
#endif

/* a/b/c into buf (c may be NULL); -1 if it does not fit. */
static int mkpath(char *buf, int size, const char *a, const char *b,
							const char *c)
{
	int la = strlen(a), lb = strlen(b), lc = c ? strlen(c) + 1 : 0;

	if (la + 1 + lb + lc >= size) return -1;
	memcpy(buf, a, la);
	buf[la] = '/';
	memcpy(buf + la + 1, b, lb + 1);
	if (c != NULL) {
		buf[la + 1 + lb] = '/';
		memcpy(buf + la + 2 + lb, c, lc);
	}
	return 0;
}

/* Read a whole small file into buf (NUL-terminated): bytes or -1, also
 * when it is bigger than size - 1. */
int ff_load(const char *path, char *buf, int size)
{
	int fd, n, m = 0;

	if ((fd = open(path, O_RDONLY)) < 0) return -1;
	while (m < size && (n = read(fd, buf + m, size - m)) > 0) m += n;
	close(fd);
	if (n < 0 || m >= size) return -1;
	buf[m] = 0;
	return m;
}

/* Write buf as dir/name: a new file next to it, then rename() (the old
 * version or the new one, never half of it). */
int app_save(const char *dir, const char *name, const char *buf, int mode)
{
	char tmp[64], path[64], tn[12];
	unsigned p = (unsigned) getpid() % 100000;
	int fd, n = strlen(buf), i;

	tn[0] = '.'; tn[1] = 'n';
	for (i = 6; i > 1; i--, p /= 10) tn[i] = '0' + p % 10;
	tn[7] = 0;
	if (mkpath(tmp, sizeof tmp, dir, tn, NULL) < 0
	    || mkpath(path, sizeof path, dir, name, NULL) < 0) {
		errno = ENAMETOOLONG;
		return -1;
	}
	(void) unlink(tmp);
	if ((fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, mode)) < 0) return -1;
	if (write(fd, buf, n) != n || close(fd) < 0 || rename(tmp, path) < 0) {
		i = errno;
		(void) unlink(tmp);
		errno = i;
		return -1;
	}
	return 0;
}

int timer_read(const char *id, struct app_timer *t)
{
	char path[40], buf[APP_FILE_MAX];

	if (!app_id_ok(id) || mkpath(path, sizeof path, APP_TIMERS, id, NULL)
	    || ff_load(path, buf, sizeof buf) < 0) return -1;
	strcpy(t->id, id);
	return timer_parse(t, buf);
}

/* Arm RTC Alarm A.  exact (timed): due as given, 0 clears it.  Else (an
 * app added a timer) only if due is earlier than the alarm set now; timed
 * runs then and arms the real next time.  Times beyond the alarm's reach
 * are armed at its end (timed runs and arms again). */
int app_arm(time_t due, int exact)
{
#ifdef __minix
	struct rtc_alarm a;
	time_t now = time((time_t *) 0);
	int fd, r;

	if (due == 0 && !exact) return 0;
	if ((fd = open("/dev/rtc", O_RDONLY)) < 0) return -1;
	r = ioctl(fd, RTCIOCGALARM, &a);
	if (r == 0 && !exact && (a.flags & RTC_ARMED) && (time_t) a.due > now
	    && (time_t) a.due <= due) {
		close(fd);
		return 0;
	}
	if (due != 0 && due <= now) due = now + 1;
	if (due > now + APP_ALARM_MAX) due = now + APP_ALARM_MAX;
	a.due = (unsigned long) due;
	a.lead = 0;				/* keep the stored lead */
	if (r == 0) r = ioctl(fd, RTCIOCSALARM, &a);
	close(fd);
	return r;
#else
	errno = ENOTTY;
	return -1;
#endif
}

int timer_add(struct app_timer *t)
{
	char buf[APP_FILE_MAX];

	if (!app_id_ok(t->id) || timer_format(t, buf, sizeof buf) < 0) {
		errno = EINVAL;
		return -1;
	}
	if (app_save(APP_TIMERS, t->id, buf, 0600) < 0) return -1;
	(void) app_arm(timer_next(t, time((time_t *) 0)), 0);
	return 0;
}

int timer_remove(const char *id)
{
	char path[40];

	if (!app_id_ok(id) || mkpath(path, sizeof path, APP_TIMERS, id, NULL)) {
		errno = EINVAL;
		return -1;
	}
	return unlink(path);		/* timed re-arms at the next alarm */
}

/* Call fn for every timer the caller can read; fn returns nonzero to
 * stop.  The number of timers seen, or -1. */
int timer_list(int (*fn)(struct app_timer *t, void *arg), void *arg)
{
	struct app_timer t;
	struct dirent *e;
	DIR *d;
	int n = 0;

	if ((d = opendir(APP_TIMERS)) == NULL) return -1;
	while ((e = readdir(d)) != NULL) {
		if (!app_id_ok(e->d_name) || timer_read(e->d_name, &t) < 0)
			continue;
		n++;
		if (fn != NULL && fn(&t, arg)) break;
	}
	closedir(d);
	return n;
}

void notify_policy(struct notify_policy *p, const char *home)
{
	char buf[APP_FILE_MAX], path[64];

	p->volume = 80;
	p->vibro = p->led = 1;
	p->dnd_from = p->dnd_to = -1;
	if (ff_load(APP_NOTIFY_CONF, buf, sizeof buf) >= 0) notify_parse(p, buf);
	if (home != NULL && *home != 0
	    && mkpath(path, sizeof path, home, ".config", "notify") == 0
	    && ff_load(path, buf, sizeof buf) >= 0) notify_parse(p, buf);
}

/* Start the alert program for a notification: t gives the title, text,
 * id and snooze (t->id empty: none), flags what notify_flags() allowed;
 * vol the speaker volume (notify_volume(), 1-100); uid >= 0: the owner of
 * a snooze timer (alert started by root).
 * Returns the pid, or -1 (errno: also when alert could not start). */
int app_alert(const struct app_timer *t, int cat, int flags, int vol,
								int uid)
{
	char fb[4], zb[4], rb[4], sb[4], vb[4], ub[8], *argv[30];
	volatile int err;
	int n = 0, pid, i;

	/* alert's -f: 1 sound, 2 vibro, 4 LED, 16 screen, as two digits.
	 * NF_SHOW (8) must not go along: it once made the flag '?', which
	 * atoi() read as 0, so nothing rang (#196, #109 AC7b.R1). */
	flags &= NF_OUT;
	fb[0] = '0' + flags / 10; fb[1] = '0' + flags % 10; fb[2] = 0;
	sb[0] = '0' + t->step / 100 % 10; sb[1] = '0' + t->step / 10 % 10;
	sb[2] = '0' + t->step % 10; sb[3] = 0;
	zb[0] = '0' + t->snooze / 10; zb[1] = '0' + t->snooze % 10; zb[2] = 0;
	rb[0] = '0' + t->ring / 10 % 10; rb[1] = '0' + t->ring % 10; rb[2] = 0;
	if (vol < 1 || vol > 100) vol = 100;
	vb[0] = '0' + vol / 100; vb[1] = '0' + vol / 10 % 10;
	vb[2] = '0' + vol % 10; vb[3] = 0;
	argv[n++] = "alert";
	argv[n++] = "-c";
	argv[n++] = (char *) app_cats[cat > 2 ? 0 : cat];
	argv[n++] = "-f";
	argv[n++] = fb;
	if (t->tone[0] != 0) {
		argv[n++] = "-t";
		argv[n++] = (char *) t->tone;
	}
	if (t->ring != 0) {
		argv[n++] = "-r";
		argv[n++] = rb;
	}
	if (t->ramp) argv[n++] = "-R";
	if (t->ramp && t->step != 0) {
		argv[n++] = "-S";
		argv[n++] = sb;
	}
	if (t->single) argv[n++] = "-1";
	if (vol != 100) {
		argv[n++] = "-v";
		argv[n++] = vb;
	}
	if (t->log[0] != 0 && t->id[0] != 0) {
		argv[n++] = "-l";
		argv[n++] = (char *) t->log;
		argv[n++] = (char *) t->id;
	}
	if (t->id[0] != 0 && t->snooze != 0) {
		argv[n++] = "-z";
		argv[n++] = zb;
		argv[n++] = (char *) t->id;
	}
	if (uid >= 0) {
		for (i = 6, ub[7] = 0; i >= 0; i--, uid /= 10)
			ub[i] = '0' + uid % 10;
		argv[n++] = "-u";
		argv[n++] = ub;
	}
	argv[n++] = (char *) t->title;
	argv[n++] = (char *) t->text;
	argv[n] = NULL;
	/* vfork: the child runs in this frame and we wait until its exec or
	 * _exit, so an exec that fails (no memory hole: EAGAIN) shows in
	 * err; it returns -1 then, not the pid of a child that ran nothing
	 * (#253).  volatile: in memory, not in a register of ours. */
	err = 0;
	if ((pid = vfork()) == 0) {
		execv(APP_ALERT, argv);
		err = errno != 0 ? errno : ENOEXEC;
		_exit(127);
	}
	if (pid >= 0 && err != 0) {
		errno = err;
		return -1;
	}
	return pid;
}

/* The desktop's record of a notification (spec K.3, T23c):
 * "notify\tCAT\tUID\tPID\tTITLE\tTEXT\n", CAT "info" or "reminder", tabs,
 * newlines and backslashes escaped (K.1).  The text is cut so that the
 * record fits in size (at most APP_DESK_LINE, one write() to the FIFO).
 * Its length, or -1 (an alarm: not for the desktop; no room). */
int notify_record(char *buf, int size, int cat, unsigned uid, int pid,
				const char *title, const char *text)
{
	char num[2][12];
	const char *part[6];
	int n = 0, k, i;

	if (cat != TC_INFO && cat != TC_REMINDER) return -1;
	if (size > APP_DESK_LINE) size = APP_DESK_LINE;
	for (k = 0; k < 2; k++) {
		char *q = num[k] + 11;
		unsigned v = k == 0 ? uid : (unsigned) pid;

		*q = 0;
		do *--q = (char) ('0' + v % 10); while ((v /= 10) > 0);
		part[2 + k] = q;
	}
	part[0] = "notify";
	part[1] = cat == TC_INFO ? "info" : "reminder";
	part[4] = title;
	part[5] = text;
	for (k = 0; k < 6; k++) {
		if (k > 0) {
			if (n >= size - 1) return -1;
			buf[n++] = '\t';
		}
		for (i = 0; part[k][i] != 0; i++) {
			char c = part[k][i], e = c == '\t' ? 't'
				: c == '\n' ? 'n' : c == '\\' ? '\\' : 0;

			if (n + 1 + (e != 0) > size - 1) {
				if (k < 5) return -1;
				break;		/* the text: cut */
			}
			if (e != 0) {
				buf[n++] = '\\';
				buf[n++] = e;
			} else
				buf[n++] = c;
		}
	}
	buf[n++] = '\n';
	return n;
}

/* Send it to the desktop's FIFO, never waiting: no desktop, a full FIFO
 * or no right to it (an app outside group gui) and it is not listed. */
static void notify_desk(int cat, const char *title, const char *text)
{
	char rec[APP_DESK_LINE];
	int n = notify_record(rec, sizeof rec, cat, (unsigned) getuid(),
			      (int) getpid(), title, text), fd;

	if (n < 0 || (fd = open(APP_DESK, O_WRONLY | O_NONBLOCK)) < 0) return;
	(void) write(fd, rec, (size_t) n);
	close(fd);
}

/* Post a notification now (an app; the alert runs as the caller).
 * opts: flags, snooze and id (NULL: everything, no snooze).  Info and
 * reminders also go to the desktop's Notifications page (T23c, spec K.6),
 * even while do-not-disturb holds the alert back; alarms only ring (K.4).
 * The alert's pid, 0 when do-not-disturb holds it back, -1 on errors. */
int notify_post(int cat, const char *title, const char *text,
					const struct app_timer *opts)
{
	struct notify_policy p;
	struct app_timer t;
	int f;

	if (cat < 0 || cat > 2) {
		errno = EINVAL;
		return -1;
	}
	if (opts != NULL) t = *opts;
	else {
		memset(&t, 0, sizeof t);
		t.flags = NF_ALL;
	}
	strncpy(t.title, title, sizeof t.title - 1);
	t.title[sizeof t.title - 1] = 0;
	strncpy(t.text, text, sizeof t.text - 1);
	t.text[sizeof t.text - 1] = 0;
	notify_desk(cat, t.title, t.text);
	notify_policy(&p, getenv("HOME"));
	if ((f = notify_flags(&p, cat, t.flags, time((time_t *) 0))) == 0)
		return 0;
	return app_alert(&t, cat, f, notify_volume(&p, &t, cat), -1);
}

static int settings_path(char *path, int size, const char *app)
{
	if (!app_id_ok(app)
	    || mkpath(path, size, APP_DATA, app, "settings") < 0) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

int settings_get(const char *app, const char *key, char *val, int size)
{
	char path[64], buf[APP_FILE_MAX], *k, *v, *pos = buf;

	if (settings_path(path, sizeof path, app) < 0
	    || ff_load(path, buf, sizeof buf) < 0) return -1;
	while ((k = ff_next(&pos, &v)) != NULL)
		if (strcmp(k, key) == 0) {
			if ((int) strlen(v) >= size) break;
			strcpy(val, v);
			return 0;
		}
	errno = ENOENT;
	return -1;
}

/* Set key to val in the app's settings (one line each; '#' comments are
 * not kept).  The directory is made (mode 700) when it is missing. */
int settings_set(const char *app, const char *key, const char *val)
{
	char path[64], dir[48], old[APP_FILE_MAX], new[APP_FILE_MAX];
	char *k, *v, *pos = old;
	int n = 0, l;

	if (settings_path(path, sizeof path, app) < 0
	    || strpbrk(key, ":\n") != NULL || *key == 0 || *key == '#'
	    || strchr(val, '\n') != NULL) {
		errno = EINVAL;
		return -1;
	}
	(void) mkpath(dir, sizeof dir, APP_DATA, app, NULL);
	if (ff_load(path, old, sizeof old) < 0) old[0] = 0;
	new[0] = 0;
	for (;;) {
		if ((k = ff_next(&pos, &v)) == NULL) {
			k = (char *) key;
			v = (char *) val;
		} else if (strcmp(k, key) == 0) continue;
		l = strlen(k) + 2 + strlen(v) + 1;
		if (n + l >= (int) sizeof new) {
			errno = EFBIG;
			return -1;
		}
		strcpy(new + n, k);
		strcat(new + n, ": ");
		strcat(new + n, v);
		strcat(new + n, "\n");
		n += l;
		if (k == key) break;
	}
	if (mkdir(dir, 0700) < 0 && errno != EEXIST) return -1;
	return app_save(dir, "settings", new, 0600);
}

int app_log(const char *app, const char *id, time_t when)
{
	char path[64], dir[48], line[40], *p = line + 24;
	unsigned long long v = (unsigned long long) when;
	int fd, n, r;

	if (!app_id_ok(app) || !app_id_ok(id)
	    || mkpath(path, sizeof path, APP_DATA, app, "log") < 0
	    || mkpath(dir, sizeof dir, APP_DATA, app, NULL) < 0) {
		errno = EINVAL;
		return -1;
	}
	*p = 0;
	do *--p = '0' + (int) (v % 10); while ((v /= 10) != 0);
	n = 24 - (p - line);
	memmove(line, p, n);
	line[n++] = ' ';
	strcpy(line + n, id);
	n += strlen(id);
	line[n++] = '\n';
	if (mkdir(dir, 0700) < 0 && errno != EEXIST) return -1;
	if ((fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0600)) < 0)
		return -1;
	r = write(fd, line, n) == n ? 0 : -1;	/* one write: one line */
	if (close(fd) < 0) r = -1;
	return r;
}

/* The screen came back (an alert closed): R7b.2 sends SIGWINCH.  Until
 * that kernel part is there this says ENOSYS; redraw on the next key. */
int on_redraw(void (*fn)(int))
{
#ifdef SIGWINCH
	return signal(SIGWINCH, fn) == SIG_ERR ? -1 : 0;
#else
	(void) fn;
	errno = ENOSYS;
	return -1;
#endif
}
