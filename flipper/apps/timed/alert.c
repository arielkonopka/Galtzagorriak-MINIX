/* alert - show a notification on the LCD, ring, wait for a button
 * (Galtzagorriak MINIX, spec F.2, R7b.4).  Started by timed and by
 * libapp's notify_post().
 *
 * Usage:	alert [-c CATEGORY] [-f FLAGS] [-z MINUTES ID] [-u UID]
 *		      TITLE TEXT
 *	-c	alarm, reminder or info (default info)
 *	-f	what notify_flags() allowed: 1 sound, 2 vibro, 4 LED, added
 *	-z	Up or Down snoozes: timer _ID fires again in MINUTES
 *	-u	the snooze timer belongs to UID (alert started by root)
 *
 * Shows title, text and the time; OK = done (exit 0), Back = dismiss
 * (1), Up/Down = snooze (2).  It rings every 5 s (sound, vibro, the LED
 * pattern runs in the LED chip) for the ring time (alarm 10 min, else
 * 1 min), then gives up (3: missed).  One line also goes to standard
 * error (the console log when timed started it).
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

#define STEP	5			/* s between rings */

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

static void ring(int flags, int cat)
{
	static const unsigned char alarm[] = {	/* 2 kHz 100 ms x3, LE */
		0xD0, 0x07, 100, 0, 0, 0, 80, 0, 0xD0, 0x07, 100, 0, 0, 0, 80, 0,
		0xD0, 0x07, 100, 0 };
	static const unsigned char chime[] = { 0xDC, 0x05, 120, 0, 0xD0, 0x07,
		180, 0 };				/* 1.5 kHz, 2 kHz */
	int fd;

	if ((flags & NF_VIBRO) && (fd = open("/dev/vibro", O_WRONLY)) >= 0) {
		put(fd, cat == TC_ALARM ? "400" : "200");
		close(fd);
	}
	if ((flags & NF_SOUND) && (fd = open("/dev/speaker", O_WRONLY)) >= 0) {
		if (cat == TC_ALARM) (void) write(fd, alarm, sizeof alarm);
		else (void) write(fd, chime, sizeof chime);
		close(fd);
	}
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

static int snooze(const char *id, int min, int cat, int flags, int uid,
				const char *title, const char *text)
{
	struct app_timer t;
	char path[40];

	memset(&t, 0, sizeof t);
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

int main(int argc, char **argv)
{
	char now_s[6], *id = NULL, *title, *text;
	struct sigaction sa;
	struct tm *tm;
	time_t now = time((time_t *) 0), end;
	int i, cat = TC_INFO, flags = NF_ALL, min = 0, uid = -1;
	int fd, shown = 0, k, me = getpid(), r = 3;

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
		} else break;
	}
	if (argc - i != 2) {
		put(2, "usage: alert [-c CATEGORY] [-f FLAGS] [-z MINUTES ID] "
		       "[-u UID] TITLE TEXT\n");
		return 2;
	}
	title = argv[i];
	text = argv[i + 1];
	clean(title);
	clean(text);
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
	end = now + (cat == TC_ALARM ? 600 : 60);
	while (time((time_t *) 0) < end) {
		ring(flags, cat);
		/* Keep the light on while it rings: after the backlight's idle
		 * timeout the first press only turns it on again, so OK or Back
		 * seemed to do nothing (seen on the device, #109). */
		if (shown) gui_backlight(255);
		if (!shown) {
			sleep(STEP);	/* no buttons: ring until the end */
			continue;
		}
		alarm(STEP);
		k = gui_key();
		alarm(0);
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
	return r;		/* the screen is free again when we exit */
}
