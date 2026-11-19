/* desktop - the module records of the desktop (spec K.3, K.6; T23a).
 *
 * Modules (separate processes, C or shell through guimod) write one
 * record per write() to the desktop's FIFO /var/run/gui.  A record is
 * one TSV line (K.1 escapes \t \n \\ in the text):
 *
 *	tile	NAME UID PID TEXT	the module's tile on the Home page
 *	status	NAME UID PID TEXT	its short text on the status line
 *	notify	NAME UID PID TEXT [BODY]	a notification: TEXT its row on
 *					the Notifications page, BODY the
 *					rest, shown when it is opened
 *	close	NAME UID PID		its tiles and status go
 *
 * A module is its uid plus its name: it changes only its own entries.
 * PID is the module's process: when it has exited (dk_purge, kill(pid, 0)
 * in the launcher) its entries go, and its records are dropped.  The
 * table is fixed: 16 entries of 48 B, nothing allocated.  The last 8
 * notifications are kept (T23c), the newest first, 88 B each: a ninth
 * pushes the oldest out.  notify_post() (libapp) sends its info and
 * reminder notifications here as well.
 *
 * Pure functions, no system calls: the launcher reads the FIFO and gives
 * the lines here; tests/host/desktest.c tests them on the PC.
 */
#include <string.h>
#include "desktop.h"

#define NFIELDS		6

/* Split at the tabs in place, undo the escapes; the number of fields or
 * -1 for more than max.  Tabs and newlines in a field become spaces: a
 * tile is one line. */
static int split(char *line, char **f, int max)
{
	char *r = line, *w = line;
	int n = 0;

	f[n++] = w;
	for (; *r != 0; r++) {
		if (*r == '\t') {
			*w++ = 0;
			if (n == max) return -1;
			f[n++] = w;
		} else if (*r == '\\' && r[1] != 0) {
			r++;
			*w++ = *r == 't' || *r == 'n' ? ' ' : *r;
		} else if (*r == '\r' && r[1] == 0)
			;			/* a CR LF line end */
		else if ((unsigned char) *r < ' ')
			*w++ = ' ';		/* control characters */
		else
			*w++ = *r;
	}
	*w = 0;
	return n;
}

/* An unsigned decimal number up to max, or -1. */
static long num(const char *s, long max)
{
	long x = 0;
	int d = 0;

	for (; *s >= '0' && *s <= '9'; s++, d++)
		if ((x = x * 10 + *s - '0') > max) return -1;
	return d == 0 || *s != 0 ? -1 : x;
}

/* Parse one record (no '\n'; changed in place).  The verb, or -1 for a
 * malformed record: unknown verb, wrong number of fields, a name that is
 * empty or longer than DK_NAME, a bad uid or pid.  A text longer than
 * DK_TEXT is cut when it is stored. */
int dk_parse(char *line, struct dk_rec *r)
{
	char *f[NFIELDS];
	int n = split(line, f, NFIELDS);
	long uid, pid;
	size_t ln;

	if (n < 4) return -1;
	r->verb = strcmp(f[0], "tile") == 0 ? DV_TILE
		: strcmp(f[0], "status") == 0 ? DV_STATUS
		: strcmp(f[0], "notify") == 0 ? DV_NOTIFY
		: strcmp(f[0], "close") == 0 ? DV_CLOSE : -1;
	if (r->verb < 0) return -1;
	if (r->verb == DV_NOTIFY ? n != 5 && n != 6
	    : n != (r->verb == DV_CLOSE ? 4 : 5)) return -1;
	ln = strlen(f[1]);
	if (ln == 0 || ln > DK_NAME) return -1;
	if ((uid = num(f[2], 65535L)) < 0) return -1;
	if ((pid = num(f[3], 32767L)) <= 0) return -1;
	r->name = f[1];
	r->uid = (unsigned short) uid;
	r->pid = (short) pid;
	r->text = n >= 5 ? f[4] : "";
	r->body = n == 6 ? f[5] : "";
	return r->verb;
}

static void copy(char *dst, const char *src, int max)
{
	int i;

	for (i = 0; i < max && src[i] != 0; i++) dst[i] = src[i];
	dst[i] = 0;
}

/* The entry of this module and kind, or -1. */
static int find(const struct desk *d, const struct dk_rec *r, int kind)
{
	int i;

	for (i = 0; i < DK_NTILES; i++)
		if (d->t[i].kind == kind && d->t[i].uid == r->uid
		    && strcmp(d->t[i].name, r->name) == 0) return i;
	return -1;
}

/* The first status entry, or -1 (when the one shown went). */
static int first_status(const struct desk *d)
{
	int i;

	for (i = 0; i < DK_NTILES; i++)
		if (d->t[i].kind == DV_STATUS) return i;
	return -1;
}

static int drop(struct desk *d, int i)
{
	int c = d->t[i].kind == DV_STATUS ? DC_STATUS : DC_TILES;

	d->t[i].kind = 0;
	if (d->st == i) d->st = (signed char) first_status(d);
	return c;
}

/* Apply a parsed record (the caller has checked that its pid is alive).
 * What changed: DC_* bits, 0 for nothing. */
int dk_apply(struct desk *d, const struct dk_rec *r)
{
	struct dk_tile *t;
	int i, c = 0;

	switch (r->verb) {
	case DV_TILE:
	case DV_STATUS:
		if ((i = find(d, r, r->verb)) < 0) {
			for (i = 0; i < DK_NTILES && d->t[i].kind != 0; i++) ;
			if (i == DK_NTILES) return DC_FULL;
			t = &d->t[i];
			t->kind = (char) r->verb;
			t->uid = r->uid;
			copy(t->name, r->name, DK_NAME);
			t->text[0] = 0;
			t->text[1] = 1;		/* != any text: changed */
		}
		t = &d->t[i];
		t->pid = r->pid;
		if (r->verb == DV_STATUS) {
			c = d->st != i
			    || strncmp(t->text, r->text, DK_STATUS) != 0
			    ? DC_STATUS : 0;
			d->st = (signed char) i;
		} else if (strncmp(t->text, r->text, DK_TEXT) != 0
			   || t->text[1] == 1)
			c = DC_TILES;
		copy(t->text, r->text, DK_TEXT);
		return c;
	case DV_NOTIFY: {
		struct dk_note *e = &d->n[0];

		memmove(d->n + 1, d->n, sizeof d->n - sizeof d->n[0]);
		if (d->notes < DK_NNOTES) d->notes++;
		e->uid = r->uid;
		e->hm = 0;
		e->unread = 1;
		copy(e->name, r->name, DK_NAME);
		copy(e->title, r->text, DK_TITLE);
		/* no BODY and a TEXT too long for the row (guimod notify):
		 * the whole TEXT is shown when it is opened */
		copy(e->body, r->body[0] == 0 && strlen(r->text) > DK_TITLE
			      ? r->text : r->body, DK_BODY);
		return DC_NOTE | DC_STATUS;
	}
	case DV_CLOSE:
		for (i = 0; i < DK_NTILES; i++)
			if (d->t[i].kind != 0 && d->t[i].uid == r->uid
			    && strcmp(d->t[i].name, r->name) == 0)
				c |= drop(d, i);
		return c;
	}
	return 0;
}

/* Drop the entries of modules that have exited (alive(pid) == 0).  What
 * changed, as dk_apply.  At most DK_NTILES calls of alive. */
int dk_purge(struct desk *d, int (*alive)(int pid))
{
	int i, c = 0;

	for (i = 0; i < DK_NTILES; i++)
		if (d->t[i].kind != 0 && !alive(d->t[i].pid)) c |= drop(d, i);
	return c;
}

/* The number of tiles, and tile i of them (the Home page's rows). */
int dk_tiles(const struct desk *d)
{
	int i, n = 0;

	for (i = 0; i < DK_NTILES; i++)
		if (d->t[i].kind == DV_TILE) n++;
	return n;
}

const struct dk_tile *dk_tile(const struct desk *d, int i)
{
	int j;

	for (j = 0; j < DK_NTILES; j++)
		if (d->t[j].kind == DV_TILE && i-- == 0) return &d->t[j];
	return (const struct dk_tile *) 0;
}

/* The notifications not opened yet (the status line's count). */
int dk_unread(const struct desk *d)
{
	int i, n = 0;

	for (i = 0; i < d->notes; i++) n += d->n[i].unread != 0;
	return n;
}

/* Clear notification i (0: the newest); the later ones move up. */
void dk_clear(struct desk *d, int i)
{
	if (i < 0 || i >= d->notes) return;
	d->notes--;
	memmove(d->n + i, d->n + i + 1,
		(size_t) (d->notes - i) * sizeof d->n[0]);
}

/* Build a record in buf (guimod, C modules): its length with the '\n',
 * or -1 if it does not fit in size (at most DK_LINE, the '\n' included).  The text is
 * escaped (\t \n \\); text NULL: a close record. */
int dk_record(char *buf, int size, const char *verb, const char *name,
	      unsigned uid, int pid, const char *text)
{
	char num_b[2][6];
	const char *part[4];
	int n = 0, i, k;
	unsigned v;

	if (size > DK_LINE) size = DK_LINE;
	for (k = 0; k < 2; k++) {
		char *q = num_b[k] + 5;

		v = k == 0 ? uid : (unsigned) pid;
		*q = 0;
		do *--q = (char) ('0' + v % 10); while ((v /= 10) > 0);
		part[2 + k] = q;
	}
	part[0] = verb;
	part[1] = name;
	for (k = 0; k < 4; k++) {
		for (i = 0; part[k][i] != 0; i++) {
			if (n >= size - 1) return -1;
			buf[n++] = part[k][i];
		}
		if (n >= size - 1) return -1;
		buf[n++] = '\t';
	}
	if (text == (const char *) 0)
		n--;				/* no text field */
	else
		for (; *text != 0; text++) {
			char c = *text == '\t' ? 't' : *text == '\n' ? 'n'
				: *text == '\\' ? '\\' : 0;

			if (n >= size - 1 - (c != 0)) return -1;
			if (c != 0) {
				buf[n++] = '\\';
				buf[n++] = c;
			} else
				buf[n++] = *text;
		}
	buf[n++] = '\n';
	return n;
}
