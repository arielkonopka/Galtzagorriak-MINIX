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
 *	Sound: yes			notify: yes | no (also Vibro:, Led:)
 *	Snooze: 5			notify: minutes, 0 = no snooze
 *
 * once: "At: <seconds since 1970, UTC>"; daily: "Time: HH:MM" and Days:.
 * run: "Program: /usr/bin/app", started as the file's owner with
 * "--timer <id>".  The file name is the timer id: 1-14 characters of
 * [A-Za-z0-9._-], not starting with '.'.
 *
 * Notification policy: /etc/notify.conf, then ~/.config/notify (same
 * format): Volume: 0-100 (0: no sound), Vibro: yes|no, Led: yes|no,
 * Dnd: 22:00-07:00 (do not disturb, local time; "Dnd: off").  An alarm
 * always rings (sound and vibro as the alarm itself says); reminders and
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

struct app_timer {
	char id[APP_ID_MAX + 1];
	unsigned char kind, cat, action;
	unsigned char missed;		/* 1: fire-once, 0: skip */
	unsigned char days;		/* bit 0 Sunday .. bit 6 Saturday */
	unsigned char incl;		/* interval: End is an occurrence */
	unsigned char flags;		/* NF_SOUND | NF_VIBRO | NF_LED */
	unsigned char snooze;		/* minutes */
	unsigned short start, end;	/* minutes after local midnight;
					 * daily: start = Time */
	unsigned short every;		/* interval: minutes */
	time_t at;			/* once: UTC */
	char title[24];
	char text[64];
	char prog[48];			/* run */
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
int app_alert(const struct app_timer *t, int cat, int flags, int uid);
int app_cat(const char *s);		/* category name -> TC_*, -1 */
extern const char *const app_cats[3];

/* Settings (APP_DATA/<app>/settings). */
int settings_get(const char *app, const char *key, char *val, int size);
int settings_set(const char *app, const char *key, const char *val);

/* The screen came back after an alert (R7b.2 SIGWINCH; -1 until then). */
int on_redraw(void (*fn)(int));

/* FlipperFormat helpers. */
char *ff_next(char **pos, char **val);	/* the next "Key: value" line */
int ff_hm(const char *s);		/* "HH:MM" -> minutes, -1 */
int ff_load(const char *path, char *buf, int size);	/* bytes, -1 */

#endif /* LIBAPP_APP_H */
