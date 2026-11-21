/* gm - window apps (spec K.8); see gm.h.  No malloc, no stdio: 16 bytes
 * of state, the records built in the caller's buffer and written with
 * write(2) whole lines at a time (one write <= PIPE_BUF, so a record never
 * arrives cut in two). */
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include "gm.h"

#define GM_WMAX		512		/* PIPE_BUF: one write */

static char *gb;			/* the caller's record buffer */
static short gsize, gused;
static char gerr;			/* a write failed (the desktop is gone) */
static short ev_len, ev_used;		/* gm_event's bytes in its buffer */

int gm_init(char *buf, int size)
{
	gb = buf;
	gsize = (short) (size > GM_WMAX ? GM_WMAX : size);
	gused = 0;
	gerr = 0;
	ev_len = ev_used = 0;
	/* the desktop gone: write() fails with EPIPE and gm_show() returns
	 * -1, so the app can end in order */
	signal(SIGPIPE, SIG_IGN);
	if (fcntl(GM_EVFD, F_GETFL) < 0 || fcntl(GM_RECFD, F_GETFL) < 0)
		return -1;			/* not started by the desktop */
	return 0;
}

static int flush(void)
{
	int n, at = 0;

	while (at < gused) {
		n = write(GM_RECFD, gb + at, (unsigned) (gused - at));
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) {
			gerr = 1;
			break;
		}
		at += n;
	}
	gused = 0;
	return gerr ? -1 : 0;
}

/* One field, escaped (\t \n \\), at p: the end, or NULL if it does not
 * fit before end. */
static char *field(char *p, char *end, const char *s)
{
	for (; *s != 0; s++) {
		int e = *s == '\t' || *s == '\n' || *s == '\\';

		if (p + e >= end) return NULL;
		if (e) {
			*p++ = '\\';
			*p++ = *s == '\t' ? 't' : *s == '\n' ? 'n' : '\\';
		} else
			*p++ = *s;
	}
	return p;
}

/* A record of n fields; when the buffer is full, what is in it goes out
 * first.  A record longer than the whole buffer is dropped. */
static void rec(const char **f, int n)
{
	int pass, i;
	char *p, *end;

	if (gb == NULL) return;
	for (pass = 0; pass < 2; pass++) {
		p = gb + gused;
		end = gb + gsize;
		for (i = 0; i < n && p != NULL; i++) {
			if (i > 0) {
				if (p >= end) p = NULL;
				else *p++ = '\t';
			}
			if (p != NULL) p = field(p, end, f[i]);
		}
		if (p != NULL && p < end) {
			*p++ = '\n';
			gused = (short) (p - gb);
			return;
		}
		if (gused == 0) return;		/* never fits */
		flush();
	}
}

static char *num(char *t, int v, int width)
{
	char *p = t + 11;
	unsigned u = (unsigned) (v < 0 ? -v : v);
	int n = 0;

	*p = 0;
	do { *--p = (char) ('0' + u % 10); n++; } while ((u /= 10) > 0);
	while (n++ < width) *--p = '0';
	if (v < 0) *--p = '-';
	return p;
}

static void win(const char *id, const char *kind, const char *title,
		int rows, int first, int live)
{
	char r[18], fi[18], a[12];
	const char *f[7];
	int n = 4;

	f[0] = "win"; f[1] = id; f[2] = kind; f[3] = title;
	if (rows > 0) {
		memcpy(r, "ROWS=", 5);
		strcpy(r + 5, num(a, rows, 1));
		f[n++] = r;
		memcpy(fi, "FIRST=", 6);
		strcpy(fi + 6, num(a, first, 1));
		f[n++] = fi;
	}
	if (live) f[n++] = "LIVE";
	rec(f, n);
}

void gm_win(const char *id, const char *kind, const char *title, int rows,
	    int first)
{
	win(id, kind, title, rows, first, 0);
}

/* A LIVE window (settings): the desktop sends every change at once. */
void gm_win_live(const char *id, const char *kind, const char *title,
		 int live)
{
	win(id, kind, title, 0, 0, live);
}

/* A pad: keys to the app raw (GM_KEY), REPEAT=ms for the repeats. */
void gm_win_pad(const char *id, const char *title, int repeat_ms)
{
	char r[20], a[12];
	const char *f[5];

	f[0] = "win"; f[1] = id; f[2] = "pad"; f[3] = title;
	if (repeat_ms > 0) {
		memcpy(r, "REPEAT=", 7);
		strcpy(r + 7, num(a, repeat_ms, 1));
		f[4] = r;
	}
	rec(f, repeat_ms > 0 ? 5 : 4);
}

void gm_item(const char *key, const char *label, const char *right)
{
	const char *f[4];

	f[0] = "item"; f[1] = key; f[2] = label; f[3] = right;
	rec(f, right != NULL && *right != 0 ? 4 : 3);
}

void gm_int(const char *key, const char *label, int min, int max, int step,
	    int val, const char *unit)
{
	char a[12], b[12], c[12], d[12];
	const char *f[8];

	f[0] = "int"; f[1] = key; f[2] = label;
	f[3] = num(a, min, 1); f[4] = num(b, max, 1); f[5] = num(c, step, 1);
	f[6] = num(d, val, 1); f[7] = unit;
	rec(f, unit != NULL && *unit != 0 ? 8 : 7);
}

void gm_enum(const char *key, const char *label, int index, const char *opts)
{
	char a[12];
	const char *f[5];

	f[0] = "enum"; f[1] = key; f[2] = label; f[3] = num(a, index, 1);
	f[4] = opts;
	rec(f, 5);
}

void gm_bool(const char *key, const char *label, int on)
{
	const char *f[4];

	f[0] = "bool"; f[1] = key; f[2] = label; f[3] = on ? "1" : "0";
	rec(f, 4);
}

void gm_time(const char *key, const char *label, int h, int m)
{
	char t[6];
	const char *f[4];

	t[0] = (char) ('0' + h / 10 % 10); t[1] = (char) ('0' + h % 10);
	t[2] = ':';
	t[3] = (char) ('0' + m / 10 % 10); t[4] = (char) ('0' + m % 10);
	t[5] = 0;
	f[0] = "time"; f[1] = key; f[2] = label; f[3] = t;
	rec(f, 4);
}

void gm_date(const char *key, const char *label, int y, int m, int d)
{
	char t[12], a[12];
	const char *f[4];

	strcpy(t, num(a, y, 4));
	t[4] = '-';
	strcpy(t + 5, num(a, m, 2));
	t[7] = '-';
	strcpy(t + 8, num(a, d, 2));
	f[0] = "date"; f[1] = key; f[2] = label; f[3] = t;
	rec(f, 4);
}

void gm_days(const char *key, const char *label, int mask)
{
	char a[12];
	const char *f[4];

	f[0] = "days"; f[1] = key; f[2] = label; f[3] = num(a, mask & 127, 1);
	rec(f, 4);
}

void gm_input(const char *key, const char *label, const char *text, int max)
{
	char a[12];
	const char *f[5];

	f[0] = "input"; f[1] = key; f[2] = label; f[3] = text;
	f[4] = num(a, max, 1);
	rec(f, 5);
}

void gm_line(const char *text)
{
	const char *f[2];

	f[0] = "line"; f[1] = text;
	rec(f, 2);
}

void gm_action(const char *key, const char *label)
{
	const char *f[3];

	f[0] = "action"; f[1] = key; f[2] = label;
	rec(f, 3);
}

/* "show": the window is complete; everything goes to the desktop.  0, or
 * -1 if the desktop is gone (exit then). */
int gm_show(void)
{
	const char *f[1];

	f[0] = "show";
	rec(f, 1);
	return flush();
}

/* "run LABEL FILE" (issue #413): the desktop ends this app's window and
 * starts the app of /etc/apps called LABEL with FILE (an absolute path)
 * as its argument, as Archive's Open.  0, or -1 if the desktop is gone.
 * The app ends after it (its events stop: GM_CLOSE). */
int gm_run(const char *label, const char *file)
{
	const char *f[3];

	f[0] = "run"; f[1] = label; f[2] = file;
	rec(f, 3);
	return flush();
}

/* "busy" / "idle": a long job (the desktop marks the window) */
int gm_busy(int busy)
{
	const char *f[1];

	f[0] = busy ? "busy" : "idle";
	rec(f, 1);
	return flush();
}

/* Split an event line at its tabs, undo the escapes. */
static int split(char *s, char **f, int nf)
{
	char *w = s;
	int n = 1;

	f[0] = w;
	for (; *s != 0; s++) {
		if (*s == '\t') {
			*w++ = 0;
			if (n == nf) return n;
			f[n++] = w;
		} else if (*s == '\\' && s[1] != 0) {
			s++;
			*w++ = *s == 't' ? '\t' : *s == 'n' ? '\n' : *s;
		} else
			*w++ = *s;
	}
	*w = 0;
	return n;
}

/* The next event (waits for it: the app sleeps in read, 0 wakeups).  buf
 * is the same buffer at every call (it keeps the bytes of the next
 * events); f gets the fields (f[0] the event, f[1] the window) and a
 * NULL after the last (at most nf - 1 fields), valid until the next call.
 * An overlong event is dropped. */
int gm_event(char *buf, int size, char **f, int nf)
{
	char *nl;
	int n, skip = 0;

	if (ev_used > 0) {			/* the line returned last time */
		memmove(buf, buf + ev_used, (unsigned) (ev_len - ev_used));
		ev_len = (short) (ev_len - ev_used);
		ev_used = 0;
	}
	for (;;) {
		nl = memchr(buf, '\n', (unsigned) ev_len);
		if (nl != NULL) {
			ev_used = (short) (nl - buf + 1);
			*nl = 0;
			if (skip) {		/* the end of an overlong one */
				skip = 0;
				memmove(buf, buf + ev_used,
					(unsigned) (ev_len - ev_used));
				ev_len = (short) (ev_len - ev_used);
				ev_used = 0;
				continue;
			}
			break;
		}
		if (ev_len >= size - 1) {	/* overlong: drop it */
			skip = 1;
			ev_len = 0;
		}
		n = read(GM_EVFD, buf + ev_len, (unsigned) (size - 1 - ev_len));
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) {			/* the desktop is gone */
			ev_len = 0;
			f[0] = buf;
			f[1] = NULL;
			buf[0] = 0;
			return GM_CLOSE;
		}
		ev_len = (short) (ev_len + n);
	}
	n = split(buf, f, nf - 1);
	f[n] = NULL;
	if (strcmp(f[0], "ok") == 0 && n >= 3) return GM_OK;
	if (strcmp(f[0], "submit") == 0 && n >= 2) return GM_SUBMIT;
	if (strcmp(f[0], "back") == 0 && n >= 2) return GM_BACK;
	if (strcmp(f[0], "page") == 0 && n >= 3) return GM_PAGE;
	if (strcmp(f[0], "redraw") == 0) return GM_REDRAW;
	if (strcmp(f[0], "close") == 0) return GM_CLOSE;
	if (strcmp(f[0], "key") == 0 && n >= 3) return GM_KEY;
	return GM_NONE;
}

/* Whether gm_event() holds a whole event line in buf (its buffer)
 * already, so that the next call returns it without reading fd 3.  An
 * app that poll()s fd 3 together with other files asks this first. */
int gm_pending(const char *buf)
{
	return ev_len - ev_used > 0
		&& memchr(buf + ev_used, '\n', (size_t) (ev_len - ev_used))
		   != NULL;
}

/* An event within ms?  One already read (a whole line after the one
 * returned last), or the pipe readable or closed. */
int gm_wait(const char *buf, int ms)
{
	struct pollfd p;
	int r;

	if (gm_pending(buf)) return 1;
	for (;;) {
		p.fd = GM_EVFD;
		p.events = POLLIN;
		p.revents = 0;
		r = poll(&p, 1, ms);
		if (r < 0 && errno == EINTR) continue;
		return r != 0;		/* an error too: gm_event says close */
	}
}

/* "KEY=VALUE" of a submit: the key, *val the value (in place). */
char *gm_kv(char *pair, char **val)
{
	char *e = strchr(pair, '=');

	if (e == NULL) {
		*val = pair + strlen(pair);
		return pair;
	}
	*e = 0;
	*val = e + 1;
	return pair;
}
