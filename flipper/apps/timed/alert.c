/* alert - show a notification on the LCD, ring, wait for a button
 * (Galtzagorriak MINIX, spec F.2, R7b.4, 7b.1, 7b.2).  Started by timed
 * and by libapp's notify_post().
 *
 * Usage:	alert [-c CATEGORY] [-f FLAGS] [-z MINUTES ID] [-u UID]
 *		      [-t TONE] [-r MINUTES] [-R] [-l APP ID] TITLE TEXT
 *	-c	alarm, reminder or info (default info)
 *	-f	what notify_flags() allowed: 1 sound, 2 vibro, 4 LED, added
 *	-z	Up or Down snoozes: timer _ID fires again in MINUTES
 *	-u	the timer belongs to UID (alert started by root)
 *	-t	the tone: a built-in name (libapp tune_names) or an RTTTL or
 *		.fmf file (as root: only one of UID's or a world-readable one)
 *	-r	ring for MINUTES (1-60; default alarm 10, else 1)
 *	-R	ramp: single short beeps for the first 30 s, then the tune
 *	-l	OK appends "TIME ID" to APP_DATA/APP/log, written as UID
 *		(the reminder log of spec 7b.1); a snooze timer _ID logs ID
 *
 * Shows title, text and the time; OK = done (exit 0), Back = dismiss
 * (1), Up/Down = snooze (2).  It rings every 5 s (the tune, vibro, the
 * LED pattern runs in the LED chip) for the ring time, then gives up
 * (3: missed).  The tune goes to /dev/speaker one note piece (at most
 * PIECE ms) per write, with a look at the buttons in between, so a key
 * stops it within a piece.  One line also goes to standard error (the
 * console log when timed started it).
 *
 * It takes the screen only when no app holds it (R4b.8).  Taking it from
 * the foreground app is R7b.2 (FG_PREEMPT, not there yet): until then an
 * alert over a running app only rings and logs, and the app's screen
 * stays as it is.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <minix/fg.h>
#include "gui.h"
#include "app.h"

#define GID_NOGROUP	99		/* /etc/group nogroup */
#define STEP	5			/* s between rings */
#define PIECE	100			/* ms of tune per /dev/speaker write */
#define TUNE_MS	3000			/* at most this much tune per ring */

static unsigned char tune[4 * TUNE_MAX];
static int ntune, shown;

static void onalrm(int sig)
{
	(void) sig;			/* interrupts the button read */
}

static void put(int fd, const char *s)
{
	(void) write(fd, s, strlen(s));
}

/* Printable ASCII only, for the console line and the LCD. */
static void clean(char *s)
{
	for (; *s != 0; s++) if (*s < 32 || *s > 126) *s = '?';
}

/* Load the tone: a built-in one, or a file.  As root (started by timed)
 * a file is read only if it is UID's or world-readable, and a plain file
 * (not a device or FIFO that could block). */
static void load_tune(const char *name, int cat, int uid)
{
	char buf[APP_FILE_MAX];
	const char *src = NULL;
	struct stat st;
	int fd, m = 0, r = 0, ms = 0, i;

	if (name != NULL && name[0] == '/') {
		if (stat(name, &st) == 0 && S_ISREG(st.st_mode)
		    && (fd = open(name, O_RDONLY | O_NONBLOCK)) >= 0) {
			if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)
			    && (getuid() != 0 || uid < 0 || st.st_uid == uid
				|| (st.st_mode & S_IROTH))) {
				while (m < (int) sizeof buf - 1 && (r = read(fd,
					buf + m, sizeof buf - 1 - m)) > 0) m += r;
				buf[m] = 0;
				if (r >= 0) src = buf;
			}
			close(fd);
		}
	} else if (name != NULL) src = tune_builtin(name);
	if (src == NULL || (ntune = tune_parse(src, tune, TUNE_MAX)) <= 0)
		ntune = tune_parse(tune_builtin(cat == TC_ALARM ? "beep"
							: "chime"), tune, TUNE_MAX);
	for (i = 0; i < ntune; i++) {		/* at most TUNE_MS a ring */
		ms += tune[4*i+2] | tune[4*i+3] << 8;
		if (ms > TUNE_MS) { ntune = i > 0 ? i : 1; break; }
	}
}

/* Play notes [0, n) in pieces; a key pressed meanwhile is returned (and
 * ends the tune), else 0. */
static int play(int fd, int n)
{
	unsigned char p[4];
	unsigned ms, d;
	int i, k;

	for (i = 0; i < n; i++) {
		ms = tune[4*i+2] | tune[4*i+3] << 8;
		p[0] = tune[4*i]; p[1] = tune[4*i+1];
		for (; ms > 0; ms -= d) {
			d = ms > PIECE ? PIECE : ms;
			p[2] = d & 0xFF; p[3] = d >> 8;
			if (write(fd, p, 4) != 4) return 0;
			if (shown && (k = gui_key_nowait()) != 0) return k;
		}
	}
	return 0;
}

/* One ring (number k of this alert): vibro and the tune.  Ramp: the
 * first 6 rings (30 s) only the tune's first note, short vibro. */
static int ring(int flags, int cat, int ramp, int k)
{
	int fd, key = 0, soft = ramp && k < 6;

	if ((flags & NF_VIBRO) && (fd = open("/dev/vibro", O_WRONLY)) >= 0) {
		put(fd, soft ? "150" : cat == TC_ALARM ? "400" : "200");
		close(fd);
	}
	if ((flags & NF_SOUND) && (fd = open("/dev/speaker", O_WRONLY)) >= 0) {
		key = play(fd, soft ? 1 : ntune);
		close(fd);
	}
	return key;
}

static void led(const char *pattern)
{
	int fd;

	if ((fd = open("/dev/led", O_WRONLY)) >= 0) {
		put(fd, pattern);
		close(fd);
	}
}

/* The text in lines of GUI_COLS, broken at spaces where it can be. */
static void wrap(const char *s, int y, int rows)
{
	char line[GUI_COLS + 1];
	int n, cut;

	while (*s != 0 && rows-- > 0) {
		n = strlen(s);
		if (n > GUI_COLS) {
			for (cut = GUI_COLS; cut > 0 && s[cut] != ' '; cut--) ;
			n = cut > 0 ? cut : GUI_COLS;
		}
		memcpy(line, s, n);
		line[n] = 0;
		gui_text(0, y, line, 0);
		y += 7;
		s += n;
		while (*s == ' ') s++;
	}
}

/* What a snooze timer keeps besides the text (set by main()). */
static char *o_tone, *o_log;
static int o_ring, o_ramp;

static int snooze(const char *id, int min, int cat, int flags, int uid,
				const char *title, const char *text)
{
	struct app_timer t;
	char path[40];

	memset(&t, 0, sizeof t);
	if (o_tone != NULL && strlen(o_tone) < sizeof t.tone)
		strcpy(t.tone, o_tone);
	if (o_log != NULL) strcpy(t.log, o_log);
	t.ring = o_ring;
	t.ramp = o_ramp;
	t.id[0] = '_';
	strncpy(t.id + (id[0] != '_'), id, APP_ID_MAX - 1);
	t.id[APP_ID_MAX] = 0;
	t.kind = TK_ONCE;
	t.at = time((time_t *) 0) + 60L * min;
	t.cat = cat;
	t.missed = 1;
	t.flags = flags & NF_ALL;
	t.snooze = min;
	t.days = 0x7F;
	strncpy(t.title, title, sizeof t.title - 1);
	strncpy(t.text, text, sizeof t.text - 1);
	if (timer_add(&t) < 0) return -1;
	if (uid >= 0 && getuid() == 0) {
		strcpy(path, APP_TIMERS "/");
		strcat(path, t.id);
		(void) chown(path, uid, 0);
	}
	return 0;
}

/* OK: one line "TIME ID" in the app's log, written as the timer's
 * owner (as root a link planted in the owner's directory must not make
 * us write elsewhere).  Called last: it drops root for good.  The group:
 * the app directory's when the owner made it, else nogroup (no
 * getpwuid(): its buffers would cost 0.8 KB of RAM). */
static void done(const char *app, const char *id, int uid, time_t now)
{
	char dir[48];
	struct stat st;
	gid_t gid = GID_NOGROUP;

	if (app == NULL) return;
	if (getuid() == 0 && uid > 0) {
		strcpy(dir, APP_DATA "/");
		strcat(dir, app);	/* app_id_ok(): at most 14 characters */
		if (stat(dir, &st) == 0 && st.st_uid == (uid_t) uid)
			gid = st.st_gid;
		if (setgid(gid) < 0 || setuid(uid) < 0
		    || getuid() != (uid_t) uid) return;
	}
	if (id[0] == '_') id++;			/* a snooze counts for it */
	if (app_log(app, id, now) < 0) put(2, "alert: cannot write the log\n");
}

int main(int argc, char **argv)
{
	char now_s[6], *id = NULL, *title, *text, *tone = NULL;
	char *logapp = NULL, *logid = NULL;
	struct sigaction sa;
	struct tm *tm;
	time_t now = time((time_t *) 0), end;
	int i, cat = TC_INFO, flags = NF_ALL, min = 0, uid = -1, ringmin = 0;
	int fd, k, me = getpid(), r = 3, ramp = 0, n;

	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
			if ((cat = app_cat(argv[++i])) < 0) cat = TC_INFO;
		} else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
			flags = atoi(argv[++i]);
		} else if (strcmp(argv[i], "-z") == 0 && i + 2 < argc) {
			min = atoi(argv[++i]);
			id = argv[++i];
			if (min < 1 || min > 60 || !app_id_ok(id)) id = NULL;
		} else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
			uid = atoi(argv[++i]);
		} else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
			tone = argv[++i];
		} else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
			ringmin = atoi(argv[++i]);
			if (ringmin < 1 || ringmin > 60) ringmin = 0;
		} else if (strcmp(argv[i], "-R") == 0) {
			ramp = 1;
		} else if (strcmp(argv[i], "-l") == 0 && i + 2 < argc) {
			logapp = argv[++i];
			logid = argv[++i];
			if (!app_id_ok(logapp) || !app_id_ok(logid))
				logapp = NULL;
		} else break;
	}
	if (argc - i != 2) {
		put(2, "usage: alert [-c CATEGORY] [-f FLAGS] [-z MINUTES ID] "
		       "[-u UID] [-t TONE] [-r MINUTES] [-R] [-l APP ID] "
		       "TITLE TEXT\n");
		return 2;
	}
	title = argv[i];
	text = argv[i + 1];
	clean(title);
	clean(text);
	o_tone = tone;
	o_log = logapp;
	o_ring = ringmin;
	o_ramp = ramp;
	load_tune(tone, cat, uid);
	tm = localtime(&now);
	now_s[0] = '0' + tm->tm_hour / 10; now_s[1] = '0' + tm->tm_hour % 10;
	now_s[2] = ':';
	now_s[3] = '0' + tm->tm_min / 10; now_s[4] = '0' + tm->tm_min % 10;
	now_s[5] = 0;

	put(2, "alert: ");
	put(2, now_s);
	put(2, " ");
	put(2, app_cats[cat]);
	put(2, ": ");
	put(2, title);
	put(2, ": ");
	put(2, text);
	put(2, "\n");

	/* The screen, if nobody holds it. */
	if ((fd = open("/dev/lcd", O_RDONLY)) >= 0) {
		shown = ioctl(fd, FGIOCSET, &me) == 0;
		close(fd);
	}
	if (shown && gui_open() == 0) {
		gui_title(title);
		gui_text_big((GUI_W - 5 * 4 * 2) / 2, 10, now_s, 2);
		wrap(text, 26, 4);
		gui_text(0, GUI_H - 6, id != NULL ? "Back:off OK:done ^v:snooze"
						  : "Back:off  OK:done", 1);
		gui_flush();
		gui_backlight(255);
	} else shown = 0;

	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sa.sa_handler = onalrm;
	sigaction(SIGALRM, &sa, NULL);
	if (flags & NF_LED) led(cat == TC_ALARM ? "fast red\n" : "pulse blue\n");
	if (ringmin == 0) ringmin = cat == TC_ALARM ? 10 : 1;
	end = now + 60L * ringmin;
	for (n = 0; time((time_t *) 0) < end; n++) {
		k = ring(flags, cat, ramp, n);
		if (!shown) {
			sleep(STEP);	/* no buttons: ring until the end */
			continue;
		}
		if (k == 0) {
			alarm(STEP);
			k = gui_key();
			alarm(0);
		}
		if (k & KEY_OK) { r = 0; break; }
		if (k & KEY_BACK) { r = 1; break; }
		if ((k & (KEY_UP | KEY_DOWN)) && id != NULL) {
			r = snooze(id, min, cat, flags, uid, title, text) == 0
				? 2 : 1;
			break;
		}
	}
	if (flags & NF_LED) led("off\n");
	if (shown) {
		gui_clear();
		gui_flush();
		gui_close();
	}
	if (r == 0) done(logapp, logid, uid, now);
	return r;		/* the screen is free again when we exit */
}
