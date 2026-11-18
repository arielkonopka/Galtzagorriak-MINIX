/* gm - window apps (spec K.8): describe a screen, the desktop draws it.
 *
 * The launcher starts a window app (a "Type: window" package, "=" before
 * the program in /etc/apps) with two pipes: events arrive on fd 3,
 * records go to the desktop on fd 4.  The app writes a window and waits
 * for an event; the desktop moves the cursor, changes values and opens
 * the keyboard by itself.  Static library: no malloc, no stdio; the
 * records are built in the caller's buffer (128 B on the stack will do).
 *
 *	char buf[128], ev[128], *f[GM_FIELDS];
 *
 *	if (gm_init(buf, sizeof buf) < 0) exit(1);
 *	gm_win("main", "list", "Demo", 0, 0);
 *	gm_bool("vib", "Vibro", 1);
 *	gm_item("about", "About", "");
 *	gm_show();
 *	for (;;) switch (gm_event(ev, sizeof ev, f, GM_FIELDS)) {
 *	case GM_OK: ... f[1] the window, f[2] the row's key
 *	case GM_SUBMIT: ... f[2..] KEY=VALUE up to a NULL, see gm_kv()
 *	case GM_CLOSE: exit(0);
 *	}
 *
 * Records and events: docs/manual (window apps), spec K.8.
 */
#ifndef GM_H
#define GM_H

#define GM_EVFD		3		/* events from the desktop */
#define GM_RECFD	4		/* records to the desktop */
#define GM_FIELDS	20		/* fields of one event, enough */

/* gm_event() */
#define GM_NONE		0		/* unknown or malformed: ignore it */
#define GM_OK		1		/* ok WIN KEY: OK on an item/action */
#define GM_SUBMIT	2		/* submit WIN KEY=VALUE ...: changed rows */
#define GM_BACK		3		/* back WIN: Back from a child window */
#define GM_PAGE		4		/* page WIN FIRST: send that page */
#define GM_REDRAW	5		/* redraw: send the current window again */
#define GM_CLOSE	6		/* close, or the desktop is gone: exit */

int gm_init(char *buf, int size);
void gm_win(const char *id, const char *kind, const char *title, int rows,
	    int first);
void gm_item(const char *key, const char *label, const char *right);
void gm_int(const char *key, const char *label, int min, int max, int step,
	    int val, const char *unit);
void gm_enum(const char *key, const char *label, int index, const char *opts);
void gm_bool(const char *key, const char *label, int on);
void gm_time(const char *key, const char *label, int h, int m);
void gm_date(const char *key, const char *label, int y, int m, int d);
void gm_days(const char *key, const char *label, int mask);
void gm_input(const char *key, const char *label, const char *text, int max);
void gm_line(const char *text);
void gm_action(const char *key, const char *label);
int gm_show(void);
int gm_busy(int busy);
int gm_event(char *buf, int size, char **f, int nf);
char *gm_kv(char *pair, char **val);

#endif /* GM_H */
