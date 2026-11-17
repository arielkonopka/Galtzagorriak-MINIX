/* app.h - libapp, the shared services of native apps (spec F.2, R7b.4).
 *
 * Timers: one file per timer in /var/timers (FlipperFormat "Key: value",
 * mode 0600, owned by the app's user; the directory is sticky).  Nothing
 * waits in RAM: the RTC alarm holds the next time, INIT runs
 * /usr/lib/timed on SIGRTC, timed fires what is due, arms the alarm again
 * and exits.  A timer file:
 *
 *	Filetype: Galtzagorriak Timer
 *	Version: 1
 *	Kind: interval			once | daily | interval
 *	Days: Mo Tu We Th Fr		all | weekdays | weekend | Su Mo ... Sa
 *	Start: 09:00			interval: the window, local time
 *	End: 17:00
 *	Every: 30			interval: minutes, from Start
 *	Inclusive: yes			interval: End itself fires too
 *	Category: reminder		alarm | reminder | info
 *	Missed: skip			skip | fire-once
 *	Action: notify			notify | run
 *	Title: Water
 *	Text: Drink a glass of water
 *	Out: sound,vibro,led,screen	what the alert uses (any of the
 *					four, or "none"); "screen" blinks
 *					the backlight.  Older files: Sound:,
 *					Vibro:, Led: yes | no
 *	Sound: multi			single (one tone, beeping) | multi
 *					(the melody of Tone:); yes | no
 *					switch the sound as in Out:
 *	Snooze: 5			notify: minutes, 0 = no snooze
 *	Tone: chime			a built-in tone (tune_builtin) or
 *					an absolute path to an RTTTL or
 *					.fmf file (the owner's, or world-
 *					readable)
 *	Ring: 10			minutes the alert rings, 1-60
 *					(0/missing: alarm 10, else 1)
 *	Mode: escalating		steady: every output at full from
 *					the start.  escalating: sparse
 *					beeps, denser beeps, the full
 *					sound, then vibro, LED and screen
 *					on top, one stage per Step:
 *					(without sound: the other outputs
 *					go sparse, denser, full).  "Ramp:
 *					yes" in older files = escalating
 *	Step: 30			escalating: seconds per stage, 5-120
 *	Volume: 100			1-100 (/dev/speaker's cubic curve).
 *					Alarm: its volume (default 100,
 *					whatever the policy says); other
 *					categories: at most the policy's.
 *					Escalating ramps from a fifth of
 *					it to all of it over the three
 *					sound stages; steady: it throughout
 *	Log: remind			OK on the alert appends "TIME ID"
 *					to APP_DATA/<Log>/log, written as
 *					the timer's owner (spec 7b.1)
 *
 * once: "At: <seconds since 1970, UTC>"; daily: "Time: HH:MM" and Days:.
 * run: "Program: /usr/bin/app", started as the file's owner with
 * "--timer <id>".  The file name is the timer id: 1-14 characters of
 * [A-Za-z0-9._-], not starting with '.'.
 *
 * Notification policy: /etc/notify.conf, then ~/.config/notify (same
 * format): Volume: 0-100 (0: no sound; the volume of reminders and
 * info, not of alarms), Vibro: yes|no, Led: yes|no,
 * Dnd: 22:00-07:00 (do not disturb, local time; "Dnd: off").  An alarm
 * always rings (every output as the alarm itself says); reminders and
 * info are not shown during do-not-disturb.
 *
 * Settings: APP_DATA/<app>/settings, "Key: value" lines.
 */
#ifndef LIBAPP_APP_H
#define LIBAPP_APP_H

#include <sys/types.h>
#include <limits.h>
#include <time.h>

#ifndef APP_ROOT
#define APP_ROOT	""		/* host tests: a scratch directory */
#endif
#define APP_TIMERS	APP_ROOT "/var/timers"
#define APP_LAST	APP_TIMERS "/.last"	/* timed's last run (root) */
#define APP_NOTIFY_CONF	APP_ROOT "/etc/notify.conf"
#define APP_DATA	APP_ROOT "/home/flipper/apps_data"	/* /ext (R5.3) */
#define APP_ALERT	"/usr/lib/alert"
#define APP_TIMED	"/usr/lib/timed"

#define APP_ID_MAX	14		/* NAME_MAX of the V2 file system */
#define APP_FILE_MAX	512		/* a timer or settings file */
#define APP_GRACE	60		/* s late that still counts as on time */
#define APP_ALARM_MAX	(27L * 24 * 3600)	/* the RTC alarm's reach */

/* Kinds, categories, actions. */
#define TK_ONCE		1
#define TK_DAILY	2
#define TK_INTERVAL	3
#define TC_INFO		0
#define TC_REMINDER	1
#define TC_ALARM	2
#define TA_NOTIFY	0
#define TA_RUN		1
/* Notification flags (struct app_timer.flags, notify_post opts). */
#define NF_SOUND	0x01
#define NF_VIBRO	0x02
#define NF_LED		0x04
#define NF_SHOW		0x08		/* notify_flags(): show it at all */
#define NF_ALL		(NF_SOUND | NF_VIBRO | NF_LED)
#define NF_SCREEN	0x10		/* blink the backlight (7b.2) */
#define NF_OUT		(NF_ALL | NF_SCREEN)	/* every output */

struct app_timer {
	char id[APP_ID_MAX + 1];
	unsigned char kind, cat, action;
	unsigned char missed;		/* 1: fire-once, 0: skip */
	unsigned char days;		/* bit 0 Sunday .. bit 6 Saturday */
	unsigned char incl;		/* interval: End is an occurrence */
	unsigned char flags;		/* NF_SOUND | NF_VIBRO | NF_LED |
					 * NF_SCREEN */
	unsigned char snooze;		/* minutes */
	unsigned short start, end;	/* minutes after local midnight;
					 * daily: start = Time */
	unsigned short every;		/* interval: minutes */
	time_t at;			/* once: UTC */
	char title[24];
	char text[64];
	char prog[48];			/* run */
	char tone[40];			/* "" = the category's default */
	unsigned char ring;		/* minutes, 0 = default */
	unsigned char ramp;		/* 1: Mode: escalating */
	unsigned char step;		/* escalating: s per stage, 0 = 30 */
	unsigned char single;		/* 1: Sound: single (one tone) */
	unsigned char volume;		/* 1-100, 0 = the default */
	char log[APP_ID_MAX + 1];	/* the app whose log OK appends to */
};

/* What timer_due() says about the occurrences in (last, now]. */
#define TD_NONE		0		/* none */
#define TD_FIRE		1		/* the latest is on time: fire it */
#define TD_LATE		2		/* missed, Missed: fire-once */
#define TD_SKIP		3		/* missed, Missed: skip */

struct notify_policy {
	unsigned char volume;		/* 0-100 */
	unsigned char vibro, led;	/* 0 or 1 */
	short dnd_from, dnd_to;		/* minutes, -1: no window */
};

/* Timer files (the parsing and the time rules are pure: host-tested). */
int app_id_ok(const char *id);
int timer_parse(struct app_timer *t, char *buf);	/* 0 or -1 */
int timer_format(const struct app_timer *t, char *buf, int size);
time_t timer_next(const struct app_timer *t, time_t after);	/* 0: none */
time_t timer_prev(const struct app_timer *t, time_t upto);	/* 0: none */
int timer_due(const struct app_timer *t, time_t last, time_t now);

int timer_read(const char *id, struct app_timer *t);	/* 0 or -1 */
int timer_add(struct app_timer *t);			/* 0 or -1 */
int timer_remove(const char *id);
int timer_list(int (*fn)(struct app_timer *t, void *arg), void *arg);
int app_arm(time_t due, int exact);	/* RTC Alarm A; see io.c */

/* Notifications. */
void notify_policy(struct notify_policy *p, const char *home);
void notify_parse(struct notify_policy *p, char *buf);
int notify_flags(const struct notify_policy *p, int cat, int want,
								time_t now);
int notify_post(int cat, const char *title, const char *text,
				const struct app_timer *opts);	/* pid, 0, -1 */
int notify_volume(const struct notify_policy *p, const struct app_timer *t,
								int cat);
int app_alert(const struct app_timer *t, int cat, int flags, int vol,
								int uid);
int app_cat(const char *s);		/* category name -> TC_*, -1 */
extern const char *const app_cats[3];

/* Tunes for /dev/speaker (tune.c): 4 bytes a note, Hz and ms, 16-bit
 * little endian.  tune_parse() takes RTTTL ("name:d=4,o=5,b=100:8e,c6")
 * or the Flipper Music Format (BPM:, Duration:, Octave:, Notes: lines)
 * and returns the number of notes (at most max), -1 if it is neither. */
#define TUNE_MAX	64		/* notes per /dev/speaker write */
int tune_parse(const char *src, unsigned char *out, int max);
const char *tune_builtin(const char *name);	/* RTTTL text or NULL */
extern const char *const tune_names[];		/* NULL-terminated */

/* Settings (APP_DATA/<app>/settings). */
int settings_get(const char *app, const char *key, char *val, int size);
int settings_set(const char *app, const char *key, const char *val);

/* App logs (APP_DATA/<app>/log): one "TIME ID" line per event, TIME in
 * seconds since 1970 (UTC).  app_log() appends one (making the directory,
 * mode 700, when it is missing): 0 or -1. */
int app_log(const char *app, const char *id, time_t when);

/* The screen came back after an alert (R7b.2 SIGWINCH; -1 until then). */
int on_redraw(void (*fn)(int));

/* FlipperFormat helpers. */
char *ff_next(char **pos, char **val);	/* the next "Key: value" line */
int ff_hm(const char *s);		/* "HH:MM" -> minutes, -1 */
int ff_load(const char *path, char *buf, int size);	/* bytes, -1 */
int app_save(const char *dir, const char *name, const char *buf, int mode);

#endif /* LIBAPP_APP_H */
