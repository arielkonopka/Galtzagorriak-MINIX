/* alert - show a notification on the LCD, ring, wait for a button
 * (Galtzagorriak MINIX, spec F.2, R7b.4, 7b.1, 7b.2).  Started by timed
 * and by libapp's notify_post().
 *
 * Usage:	alert [-c CATEGORY] [-f FLAGS] [-z MINUTES ID] [-u UID]
 *		      [-t TONE] [-r MINUTES] [-R] [-S SECONDS] [-1]
 *		      [-v VOLUME] [-l APP ID] TITLE TEXT
 *	-c	alarm, reminder or info (default info)
 *	-f	what notify_flags() allowed: 1 sound, 2 vibro, 4 LED,
 *		16 screen (blink the backlight), added
 *	-z	Up or Down snoozes: timer _ID fires again in MINUTES
 *	-u	the timer belongs to UID (alert started by root)
 *	-t	the tone: a built-in name (libapp tune_names) or an RTTTL or
 *		.fmf file (as root: only one of UID's or a world-readable one)
 *	-r	ring for MINUTES (1-60; default alarm 10, else 1)
 *	-R	escalating (Mode: escalating; else steady)
 *	-S	escalating: SECONDS per stage (5-120, default 30)
 *	-1	single tone (Sound: single): beeps of the tone's first note
 *		instead of the melody
 *	-v	the speaker volume, 1-100 (default 100; libapp's
 *		notify_volume(): an alarm's own, else the policy's)
 *	-l	OK appends "TIME ID" to APP_DATA/APP/log, written as UID
 *		(the reminder log of spec 7b.1); a snooze timer _ID logs ID
 *
 * Shows title, text and the time; OK = done (exit 0), Back = dismiss
 * (1), Up/Down = snooze (2).  It rings for the ring time, then gives up
 * (3: missed).  The backlight stays on while it shows (#196).
 *
 * The rhythm: a ring starts every few seconds, timed from the start of
 * the one before.  Escalating (-R): every 5, 5, 4, 4, 3, 3, 2, 2 s, then
 * every second (owner, 2026-11-17).  Steady: an alarm every second,
 * others every 5 s.  A ring is never shorter than its sound.  Escalating
 * also goes by stages of -S seconds (default 10, so the rhythm and the
 * volume reach full together, about 30 s in): stages 0 and 1 ring one
 * beep of the tone, stage 2 the full sound (the melody, or three beeps
 * when single), stage 3 adds vibro, LED and screen flash on top (without
 * sound these come from the start).  The volume (-v) holds for the whole
 * ring when steady; escalating ramps it from a fifth of -v at the start
 * to all of -v at the end of the third stage (/dev/speaker's volume note:
 * a cubic curve, so the low end is quiet indeed).
 *
 * One blocking loop, no busy waits: a note goes to /dev/speaker in pieces
 * of at most PIECE ms (the write returns when it has played), and the
 * time to the next ring is a poll() on /dev/buttons with a ms timeout
 * (DEV_SELECT, R4b.6; alarm() had whole seconds).  Between pieces it
 * looks at the buttons, so a key ends the alert within PIECE ms.  Vibro
 * pulses end by themselves (the kernel's ms timer), the LED pattern runs
 * in the LED chip, and the screen flash writes the backlight on and off.
 * The backlight is set back as it was (level, timeout, forced mode) when
 * the alert ends.  One line also goes to standard error (the console log
 * when timed started it).
 *
 * It takes the screen and the buttons from whatever app has them (R7b.2,
 * FGIOCPREEMPT), waits its turn while another alert shows (EBUSY, until
 * its ring time is over), and puts the picture under it back before
 * FGIOCRELEASE; the preempted app keeps running.  A button read that
 * ends with 0 bytes was a preemption, not an error.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/times.h>
#include <minix/fg.h>
#include "gui.h"
#include "app.h"

#define GID_NOGROUP	99		/* /etc/group nogroup */
#define PIECE	100			/* ms of tune per /dev/speaker write */
#define TUNE_MS	3000			/* at most this much tune per ring */
#define STAGE_S	10			/* escalating: default s per stage */

static unsigned char tune[4 * TUNE_MAX];
static int ntune, shown, btn = -1, bl = -1, bl_lit = -1;
static unsigned char bl_old[4];		/* level, timeout, forced mode */
static unsigned f0;			/* Hz of the single tone */
static int vol = 100;			/* -v */
static int ends = KEY_OK | KEY_BACK;	/* the keys that end it (+ snooze) */

/* The rhythm, escalating: s from one ring's start to the next (owner,
 * 2026-11-17), then RHY_END.  Steady: STEADY_MS(cat). */
static const unsigned char rhythm[] = { 5, 5, 4, 4, 3, 3, 2, 2 };
#define RHY_END		1000
#define STEADY_MS(cat)	((cat) == TC_ALARM ? 1000L : 5000L)
#define SCR_MS		300		/* screen flash: on per ring */

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
	if (ntune < 1) {			/* nothing at all: one beep */
		tune[0] = 2000 & 0xFF; tune[1] = 2000 >> 8;
		tune[2] = 200; tune[3] = 0;
		ntune = 1;
	}
	for (i = 0, f0 = 0; i < ntune && f0 == 0; i++)	/* the single tone */
		f0 = tune[4*i] | tune[4*i+1] << 8;
	if (f0 == 0) f0 = 2000;
}

/* Note i of a ring's sound (full: the melody, or three beeps when
 * single; else one beep of the tone): its Hz in *hz (0 a rest), its ms
 * returned, 0 after the last. */
static unsigned rnote(int full, int single, int i, unsigned *hz)
{
	*hz = 0;
	if (!full) {
		if (i > 0) return 0;
		*hz = f0;
		return 150;
	}
	if (single) {
		if (i > 4) return 0;
		*hz = i & 1 ? 0 : f0;
		return i & 1 ? 100 : 200;
	}
	if (i >= ntune) return 0;
	*hz = tune[4*i] | tune[4*i+1] << 8;
	return tune[4*i+2] | tune[4*i+3] << 8;
}

/* Wait up to ms for a key (only a pure wait when the screen is not ours):
 * the key, or 0.  poll() rounds up to the clock tick.  If the buttons
 * fail, they are dropped and later waits are pure timeouts: never spin.
 * A read of 0 bytes (a preemption, R7b.2) is no key. */
static int wait_key(int ms)
{
	struct pollfd p;
	int n;

	p.fd = btn;
	p.events = POLLIN;
	p.revents = 0;
	if ((n = poll(&p, btn >= 0 ? 1 : 0, ms)) < 0 && btn >= 0
	    && errno != EINTR) {
		close(btn);
		btn = -1;
	}
	return n > 0 && shown ? gui_key_nowait() & ends : 0;
}

/* The backlight: on (255) or off, in auto mode with the timeout kept;
 * -1: back as it was (bl_old). */
static void screen(int on)
{
	unsigned char b[4];

	if (bl < 0 || on == bl_lit) return;
	if (on < 0) memcpy(b, bl_old, 4);
	else {
		b[0] = on ? 255 : 0;
		b[1] = bl_old[1]; b[2] = bl_old[2];
		b[3] = 0;			/* forced off: not now */
	}
	(void) write(bl, b, 4);
	bl_lit = on;
}

static void bl_save(void)
{
	unsigned char b[4];
	unsigned long st;

	if ((bl = open("/dev/backlight", O_RDWR)) < 0) return;
	if (read(bl, b, 4) != 4) {
		close(bl);
		bl = -1;
		return;
	}
	st = b[0] | b[1] << 8 | (unsigned long) b[2] << 16
					| (unsigned long) b[3] << 24;
	bl_old[0] = st >> 16 & 0xFF;		/* hal.h HAL_BL_* */
	if (bl_old[0] == 0) bl_old[0] = 255;
	bl_old[1] = st & 0xFF;
	bl_old[2] = st >> 8 & 0x3F;
	bl_old[3] = st >> 14 & 3;
	if (bl_old[3] > 2) bl_old[3] = 0;
}

static long ms_since(clock_t t0)
{
	struct tms tb;

	return (long) (times(&tb) - t0) * 1000L / CLK_TCK;
}

static void led(const char *pattern);

/* Ring for ring_ms (see the top): the key that ended it (OK, Back, or
 * Up/Down when snz), or 0. */
static int ring(int flags, int cat, int ramp, int single, long stage,
						long ring_ms, int snz)
{
	struct tms tb;
	clock_t t0 = times(&tb);
	char vb[4];
	unsigned char p[8];
	unsigned hz, d;
	long t, start = 0, per, q, scr_off = -1;
	int spk = -1, vib = -1, key = 0, n = 0, st, other, led_on = 0;
	int v, sv = -1, i, np, full;

	if (snz) ends |= KEY_UP | KEY_DOWN;	/* other keys: ring on */

	if ((flags & NF_SOUND)
	    && (spk = open("/dev/speaker", O_WRONLY)) < 0) flags &= ~NF_SOUND;
	if (flags & NF_VIBRO) vib = open("/dev/vibro", O_WRONLY);
	while (key == 0 && (t = ms_since(t0)) < ring_ms) {
		/* A ring starts: its stage, its rhythm, its volume. */
		start = t;
		st = ramp ? (int) (t / stage) : 3;
		if (st > 3) st = 3;
		if (!ramp) per = STEADY_MS(cat);
		else per = n < (int) sizeof rhythm ? rhythm[n] * 1000L : RHY_END;
		n++;
		full = st >= 2;
		v = vol;			/* a fifth of vol up to vol */
		if (ramp && t < 3 * stage) {
			v = vol / 5 + (int) ((vol - vol / 5) * t / (3 * stage));
			v = v / 5 * 5;
			if (v < 1) v = 1;
		}
		/* The other outputs: with the sound from stage 3 (steady:
		 * at once), without it from the start. */
		other = !(flags & NF_SOUND) || st >= 3;
		if (other && (flags & NF_LED) && !led_on) {
			led(cat == TC_ALARM ? "fast red\n" : "pulse blue\n");
			led_on = 1;
		}
		if (other && vib >= 0) {
			q = cat != TC_ALARM ? 200 : full ? 400 : 150;
			vb[0] = '0' + q / 100;
			vb[1] = '0' + q / 10 % 10;
			vb[2] = '0' + q % 10;
			(void) write(vib, vb, 3);	/* ends by itself */
		}
		/* The light on at every ring: a write restarts its idle
		 * timeout, so no OK goes to waking it (#196). */
		if (shown || (other && (flags & NF_SCREEN))) {
			if (bl >= 0) {
				bl_lit = -1;
				screen(1);
			} else gui_backlight(255);
		}
		if (other && (flags & NF_SCREEN) && bl >= 0)
			scr_off = start + SCR_MS;
		/* The sound, in pieces with a look at the buttons between. */
		for (i = 0; spk >= 0 && key == 0
		     && (d = rnote(full, single, i, &hz)) != 0; i++) {
			while (d > 0 && key == 0 && spk >= 0) {
				q = d < PIECE ? d : PIECE;
				np = 0;
				if (v != sv) {	/* a volume note first */
					p[0] = p[1] = 0xFF;
					p[2] = v; p[3] = 0;
					np = 4;
					sv = v;
				}
				p[np] = hz & 0xFF; p[np+1] = hz >> 8;
				p[np+2] = q & 0xFF; p[np+3] = q >> 8;
				np += 4;
				if (hz == 0) {		/* a rest: wait */
					np -= 4;
					if (np > 0) (void) write(spk, p, np);
					key = wait_key((int) q);
				} else if (write(spk, p, np) != np) {
					close(spk);	/* gone: no sound */
					spk = -1;
				} else if (shown) key = gui_key_nowait() & ends;
				d -= q;
				if (scr_off >= 0 && ms_since(t0) >= scr_off) {
					screen(0);
					scr_off = -1;
				}
			}
		}
		/* Wait for a key until the next ring (from this one's start,
		 * never before the sound has ended), the screen flash off on
		 * the way. */
		t = ms_since(t0);
		if (start + per < t) per = t - start;
		while (key == 0 && (t = ms_since(t0)) < start + per
		       && t < ring_ms) {
			q = start + per;
			if (q > ring_ms) q = ring_ms;
			if (scr_off >= 0) {
				if (t >= scr_off) {
					screen(0);
					scr_off = -1;
				} else if (scr_off < q) q = scr_off;
			}
			if (q - t < 1) q = t + 1;
			key = wait_key((int) (q - t));
		}
	}
	if (led_on) led("off\n");
	if (spk >= 0) close(spk);
	if (vib >= 0) {
		(void) write(vib, "0", 1);
		close(vib);
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
static int o_ring, o_ramp, o_step, o_single, o_vol;

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
	t.step = o_step;
	t.single = o_single;
	t.volume = o_vol;
	t.id[0] = '_';
	strncpy(t.id + (id[0] != '_'), id, APP_ID_MAX - 1);
	t.id[APP_ID_MAX] = 0;
	t.kind = TK_ONCE;
	t.at = time((time_t *) 0) + 60L * min;
	t.cat = cat;
	t.missed = 1;
	t.flags = flags & NF_OUT;
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
	struct tm *tm;
	time_t now = time((time_t *) 0);
	int i, cat = TC_INFO, flags = NF_ALL, min = 0, uid = -1, ringmin = 0;
	int fg = -1, k, r = 3, ramp = 0, step = 0, single = 0, saved = 0;
	long ring_ms;
	static unsigned char under[GUI_W * GUI_H / 8];	/* the old picture */

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
		} else if (strcmp(argv[i], "-S") == 0 && i + 1 < argc) {
			step = atoi(argv[++i]);
			if (step < 5 || step > 120) step = 0;
		} else if (strcmp(argv[i], "-1") == 0) {
			single = 1;
		} else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc) {
			vol = atoi(argv[++i]);
			if (vol < 1 || vol > 100) vol = 100;
			o_vol = vol;
		} else if (strcmp(argv[i], "-l") == 0 && i + 2 < argc) {
			logapp = argv[++i];
			logid = argv[++i];
			if (!app_id_ok(logapp) || !app_id_ok(logid))
				logapp = NULL;
		} else break;
	}
	if (argc - i != 2) {
		put(2, "usage: alert [-c CATEGORY] [-f FLAGS] [-z MINUTES ID] "
		       "[-u UID] [-t TONE] [-r MINUTES] [-R] [-S SECONDS] "
		       "[-1] [-v VOLUME] [-l APP ID] TITLE TEXT\n");
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
	o_step = step;
	o_single = single;
	flags &= NF_OUT;
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

	if (ringmin == 0) ringmin = cat == TC_ALARM ? 10 : 1;
	ring_ms = 60000L * ringmin;

	/* R7b.2: take the screen and the buttons from whoever has them; while
	 * another alert shows (EBUSY), wait for our turn, a second at a time,
	 * within our ring time.  The picture under us is read now and put
	 * back at the end, so the preempted app need not redraw.  No screen
	 * (no /dev/lcd): ring only. */
	if ((fg = open("/dev/lcd", O_RDWR)) >= 0) {
		while ((k = ioctl(fg, FGIOCPREEMPT, (void *) 0)) < 0
		       && errno == EBUSY && time((time_t *) 0) - now
						< ring_ms / 1000 - 1)
			sleep(1);
		shown = k == 0;
		if (shown && read(fg, (char *) under, sizeof under)
							== sizeof under)
			saved = 1;
		ring_ms -= (time((time_t *) 0) - now) * 1000L;
	}
	if (shown && gui_open() == 0) {
		gui_title(title);
		gui_text_big((GUI_W - 5 * 4 * 2) / 2, 10, now_s, 2);
		wrap(text, 26, 4);
		gui_text(0, GUI_H - 6, id != NULL ? "Back:off OK:done ^v:snooze"
						  : "Back:off  OK:done", 1);
		gui_flush();
	} else shown = 0;
	if (shown) btn = open("/dev/buttons", O_RDONLY);	/* to poll */
	bl_save();				/* to set it back at the end */

	k = ring_ms < 1000 ? 0 : ring(flags, cat, ramp, single,
			(step ? step : STAGE_S) * 1000L, ring_ms, id != NULL);
	if (k & KEY_OK) r = 0;
	else if (k & KEY_BACK) r = 1;
	else if (k & (KEY_UP | KEY_DOWN))
		r = snooze(id, min, cat, flags, uid, title, text) == 0 ? 2 : 1;
	if (bl >= 0) {
		screen(-1);			/* as it was */
		close(bl);
	}
	if (btn >= 0) close(btn);
	if (shown) {
		if (!saved) {
			gui_clear();
			gui_flush();
		}
		gui_close();
		if (saved) {			/* the picture under the alert */
			lseek(fg, 0L, SEEK_SET);
			(void) write(fg, (char *) under, sizeof under);
		}
		(void) ioctl(fg, FGIOCRELEASE, (void *) 0);
	}
	if (fg >= 0) close(fg);
	if (r == 0) done(logapp, logid, uid, now);
	return r;
}
