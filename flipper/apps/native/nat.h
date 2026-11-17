/* nat.h - small helpers shared by the native apps remind and alarm
 * (spec F.4, 7b.1, 7b.2): their own user, a field form, times.
 * nat_core.c is pure (host-tested); nat_ui.c draws with libfuri. */
#ifndef NAT_H
#define NAT_H

#include <sys/types.h>
#include <time.h>

/* "HH:MM" of minutes after midnight (0..1440) into b[6]; returns b. */
char *nat_hm(char *b, int min);
/* The 7 day letters of a Su..Sa mask, Monday first ("MTWTF--"). */
char *nat_days(char *b, int mask);
/* Monday-first position 0..6 -> the bit (bit 0 Sunday). */
int nat_daybit(int pos);
/* Local midnight of the day of t, and of `off` days after it. */
time_t nat_midnight(time_t t, int off);
/* A small unsigned number into b (returns b). */
char *nat_num(char *b, unsigned v);

/* Run as the app's own user (spec A.3) from here on: started as root
 * (by the launcher, /etc/apps "Remind:root:..."), after it has opened the
 * screen, the buttons and the backlight it keeps the descriptors and
 * becomes user `name` with its group (timers).  0, or -1 (then exit:
 * never run on as root). */
int nat_become(const char *name);

/* A form of fields: a label and a value per line, a cursor. */
#define NAT_ROWS	5		/* rows in the window under the title */
struct nat_form {
	int n, cur, top;
	const char *label[16];
	char value[16][24];
};
void nat_form_move(struct nat_form *f, int d);	/* cursor +-1, scroll */

/* An app log (libapp app_log(): "TIME ID" lines, oldest first): the
 * number of lines in each of the `days` local days up to the one that
 * starts at day0 (counts[days - 1] is that day's).  Reads the file from
 * its end, only as far back as the window: a long log costs no more.
 * The lines counted, or -1. */
int nat_log_days(int fd, time_t day0, int days, int *counts);

#endif /* NAT_H */
