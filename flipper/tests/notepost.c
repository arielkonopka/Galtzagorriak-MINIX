/* notepost - post notifications through libapp's notify_post() (T23c,
 * spec K.6; tools/nattest.py "notes").
 *
 * Usage:	notepost [-r] N
 *
 * Posts N notifications, "Note 1" .. "Note N", each with the text
 * "Text of note I", as info (-r: reminders).  notify_post() sends them to
 * the desktop's Notifications page and starts an alert for each unless
 * do-not-disturb holds it back (the test sets Dnd so that none rings).
 * Exit status: the number of posts that failed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"

int main(int argc, char **argv)
{
	struct app_timer t;
	int cat = TC_INFO, n, i, bad = 0;

	if (argc > 1 && strcmp(argv[1], "-r") == 0) {
		cat = TC_REMINDER;
		argv++;
		argc--;
	}
	if (argc != 2 || (n = atoi(argv[1])) <= 0) {
		fprintf(stderr, "usage: notepost [-r] N\n");
		return 2;
	}
	memset(&t, 0, sizeof t);		/* no sound, vibro or LED */
	for (i = 1; i <= n; i++) {
		char title[16], text[32];

		sprintf(title, "Note %d", i);
		sprintf(text, "Text of note %d", i);
		if (notify_post(cat, title, text, &t) < 0) bad++;
	}
	return bad;
}
