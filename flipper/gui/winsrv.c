/* winsrv - the window server of the desktop (spec K.8).
 *
 * A window app ("Type: window", "=" before its program in /etc/apps) runs
 * as the launcher's child with two pipes: the launcher writes events to
 * its fd 3 and reads its records from its fd 4.  The app describes a
 * screen (winrec.c); the launcher draws it in the one K.2 / R5.6a.7 style,
 * moves the cursor, changes values, edits times and dates, types text on
 * its keyboard and keeps the window's parent, all without the app: a key
 * never waits for it.  The app hears of OK on an item or an action, of
 * the values that changed (submit), of Back from a child window and of a
 * page of a long list.
 *
 * Never blocking on the app: the events are written without blocking (a
 * full pipe marks the app busy), the records are read only when poll()
 * says so, a busy mark appears in the title bar 150 ms after an event
 * that wants an answer, and Back always works: on the first window it
 * ends the app at once (close, both pipes closed); an app still alive 2 s
 * later gets SIGTERM (win_reap, from the launcher's loop).
 *
 * RAM: two windows (the current one and its parent, 2 x 658 B), a record
 * line (128 B) and the keyboard's text (64 B), all in .bss; nothing is
 * allocated.  The code runs from flash with the launcher.
 */
#include <lib.h>
#include <minix/callnr.h>
#include <sys/types.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "gui.h"
#include "winrec.h"
#include "winsrv.h"

#define MAXARGS		8
#define CMDMAX		64
#define ROW_Y(i)	(7 + (i) * 6)
#define VIS		8		/* rows on the screen, hint line below */
#define HINT_Y		58
#define BUSY_MS		150		/* no answer: the busy mark */
#define TERM_MS		2000		/* closed and still alive: SIGTERM */
#define NOTE_MS		2000
#define EVMAX		256		/* one event line, on the stack */

static struct wr_win wins[2];		/* the current window and its parent */
static char lbuf[WR_LINE];		/* the record being read */
static short llen;
static char lskip;			/* the rest of an overlong record */
static char kbt[WR_INMAX + 1];		/* the keyboard's text */
static pid_t gone_pid;			/* a closed app not yet ended */
static char gone_term;			/* signals sent: 1 SIGTERM, 2 SIGKILL */
static long gone_t;			/* ticks: look at it again then */
static long gone_sig;			/* ticks: the next signal then */

#define B_REPLACE	0		/* the same window again (or the first) */
#define B_CHILD		1		/* a new window over the current one */
#define B_PARENT	2		/* the kept parent again */
#define B_POP		3		/* the parent, which was not kept */

struct ses {
	const char *label;		/* the app's name in /etc/apps */
	const char *note;		/* on the message line */
	long note_t, wait_t;		/* ticks */
	pid_t pid;
	int evfd, recfd;
	signed char cur, par, bslot;	/* slots of wins[], -1: none */
	char bmode;			/* B_* of the window being built */
	unsigned char depth;		/* the app's windows open */
	char log;			/* GuiLog: on */
	char waiting;			/* an answer is due */
	char busy, appbusy;		/* the mark: no answer / "busy" */
	char edit;			/* field edit: the field + 1 */
	char kb;			/* the keyboard is open */
	char popping;			/* Back to a parent not kept */
	signed char pagedir;		/* a page asked for: +1 down, -1 up */
	char dirty;			/* draw again */
	char ended;
	unsigned char kx, ky, shift;	/* the keyboard's cursor */
	unsigned char keep_sel, keep_top;
	unsigned short keep_first;
};

static long ticks(void)
{
	struct tms t;

	return (long) times(&t);
}

static long ms_ticks(long ms)
{
	return (ms * CLK_TCK + 999) / 1000;
}

/* ms from now to tick t (0 if past) */
static int ms_until(long t)
{
	long d = t - ticks();

	return d <= 0 ? 0 : (int) (d * 1000 / CLK_TCK) + 1;
}

/*===========================================================================*
 *				Events to the app			     *
 *===========================================================================*/
static void logline(struct ses *s, const char *dir, const char *l, int n)
{
	if (!s->log) return;
	(void) write(2, dir, 5);
	(void) write(2, l, (unsigned) n);
	if (n == 0 || l[n - 1] != '\n') (void) write(2, "\n", 1);
}

/* One event, written without blocking: a full pipe (the app does not
 * read) only marks it busy. */
static void send(struct ses *s, const char *e, int n)
{
	logline(s, "gui> ", e, n);
	if (s->evfd < 0) return;
	if (write(s->evfd, e, (unsigned) n) != n) {
		s->busy = 1;
		s->dirty = 1;
	}
}

static void ev(struct ses *s, const char *verb, const char *a, const char *b)
{
	char e[EVMAX];
	int n = (int) strlen(verb), k;

	memcpy(e, verb, (size_t) n);
	if (a != NULL) {
		e[n++] = '\t';
		if ((k = wr_esc(e + n, EVMAX - n - 2, a)) < 0) return;
		n += k;
	}
	if (b != NULL) {
		e[n++] = '\t';
		if ((k = wr_esc(e + n, EVMAX - n - 2, b)) < 0) return;
		n += k;
	}
	e[n++] = '\n';
	send(s, e, n);
}

/* An answer is due: the busy mark if it does not come within 150 ms. */
static void expect(struct ses *s)
{
	s->waiting = 1;
	s->wait_t = ticks();
}

/* The changed values of w, in as many submit lines as they need. */
static void submit(struct ses *s, struct wr_win *w)
{
	char e[EVMAX];
	int from = 0, n;

	while (from < w->nrows && (n = wr_submit(w, e, EVMAX, &from)) > 0)
		send(s, e, n);
	wr_commit(w);
}

/* A LIVE window (settings, K.7): every change goes to the app at once,
 * so it can apply it (the backlight, the volume); a time or date row when
 * its field edit ends.  The app does not answer, so no busy mark. */
static void live(struct ses *s)
{
	if (s->cur >= 0 && wins[s->cur].live) submit(s, &wins[s->cur]);
}

/*===========================================================================*
 *				Ending					     *
 *===========================================================================*/
static char *status_note(int st)
{
	if (WIFEXITED(st) && WEXITSTATUS(st) == 127) return "not found";
	if (WIFEXITED(st) && WEXITSTATUS(st) == 126) return "cannot start";
	if (WIFSIGNALED(st) && WTERMSIG(st) != SIGTERM
	    && WTERMSIG(st) != SIGKILL) return "app crashed";
	return NULL;
}

/* The window is gone at once; the app gets "close" (if it may still
 * read) and EOF on both pipes.  If it has not ended, win_reap() sends it
 * SIGTERM 2 s later. */
static char *end(struct ses *s, int say_close)
{
	int st;
	pid_t r;

	if (s->ended) return NULL;
	s->ended = 1;
	if (say_close) ev(s, "close", NULL, NULL);
	if (s->evfd >= 0) close(s->evfd);
	if (s->recfd >= 0) close(s->recfd);
	s->evfd = s->recfd = -1;
	if (s->pid <= 0) return NULL;
	r = waitpid(s->pid, &st, WNOHANG);
	if (r == s->pid) return status_note(st);
	win_reap(1);			/* an older one: its time is up */
	gone_pid = s->pid;
	gone_term = 0;
	gone_t = gone_sig = ticks() + ms_ticks(TERM_MS);
	return NULL;
}

/* ms until win_reap() has something to do for a closed app, -1: none. */
int win_pending(void)
{
	if (gone_pid <= 0) return -1;
	return ms_until(gone_t);
}

/* A closed app: reaped as soon as it has ended; SIGTERM when its 2 s are
 * over (or now: before another app starts), SIGKILL 2 s after that if it
 * ignores it.  After a signal it is looked at every 100 ms until it is
 * gone, so no zombie waits for the next key. */
void win_reap(int now)
{
	int st;
	pid_t r;

	if (gone_pid <= 0) return;
	r = waitpid(gone_pid, &st, WNOHANG);
	if (r == gone_pid || (r < 0 && errno == ECHILD)) {
		gone_pid = 0;
		return;
	}
	if (!now && ticks() - gone_t < 0) return;
	if (gone_term < 2 && (now || ticks() - gone_sig >= 0)) {
		(void) kill(gone_pid, gone_term == 0 ? SIGTERM : SIGKILL);
		gone_term++;
		gone_sig = ticks() + ms_ticks(TERM_MS);
	}
	gone_t = ticks() + ms_ticks(100);	/* look again */
}

/*===========================================================================*
 *				Records from the app			     *
 *===========================================================================*/
static int first_sel(struct wr_win *w, int from, int dir)
{
	int i;

	for (i = from; i >= 0 && i < w->nrows; i += dir)
		if (wr_selectable(w, i)) return i;
	return -1;
}

static void fix_sel(struct wr_win *w, int sel)
{
	int i;

	if (sel >= w->nrows) sel = w->nrows - 1;
	if (sel < 0) sel = 0;
	if ((i = first_sel(w, sel, 1)) < 0) i = first_sel(w, sel, -1);
	w->sel = (unsigned char) (i < 0 ? 0 : i);
	if (w->top > w->sel) w->top = w->sel;
	if (w->sel >= w->top + VIS) w->top = (unsigned char) (w->sel - VIS + 1);
	if (w->top > 0 && w->top + VIS > w->nrows)
		w->top = (unsigned char) (w->nrows > VIS ? w->nrows - VIS : 0);
	if (w->kind != WK_LIST) w->top = 0;
}

/* "win": which slot the new window is built in.  The window on the
 * screen stays usable while another slot fills. */
static void start(struct ses *s, char **f, int n, int len)
{
	const char *id = n > 1 ? f[1] : "";
	int slot;

	if (s->cur < 0) {
		slot = 0;
		s->bmode = B_REPLACE;
	} else if (s->popping) {
		slot = 1 - s->cur;
		s->bmode = B_POP;
		s->par = -1;
	} else if (strcmp(id, wr_id(&wins[s->cur])) == 0) {
		slot = s->cur;
		s->bmode = B_REPLACE;
		s->keep_sel = wins[slot].sel;
		s->keep_top = wins[slot].top;
		s->keep_first = wins[slot].first;
		s->edit = 0;
		s->kb = 0;
	} else if (s->par >= 0 && strcmp(id, wr_id(&wins[s->par])) == 0) {
		slot = s->par;
		s->bmode = B_PARENT;
	} else {
		slot = 1 - s->cur;
		s->bmode = B_CHILD;
		s->par = -1;			/* its slot is reused */
	}
	wr_start(&wins[slot], f, n, len);
	s->bslot = (signed char) slot;
}

/* "show": the window built is complete. */
static void shown(struct ses *s)
{
	int slot = s->bslot;
	struct wr_win *w = &wins[slot];

	s->bslot = -1;
	switch (s->bmode) {
	case B_REPLACE:
		if (s->cur == slot && s->pagedir != 0
		    && w->first != s->keep_first) {	/* a new page */
			w->top = 0;
			fix_sel(w, s->pagedir > 0 ? 0 : w->nrows - 1);
		} else if (s->cur == slot) {		/* the same, updated */
			w->top = s->keep_top;
			fix_sel(w, s->keep_sel);
		} else
			fix_sel(w, 0);
		if (s->cur < 0) s->depth = 1;
		s->cur = (signed char) slot;
		break;
	case B_PARENT:
		s->cur = (signed char) slot;
		s->par = -1;
		if (s->depth > 1) s->depth--;
		fix_sel(w, w->sel);
		break;
	case B_POP:
		s->cur = (signed char) slot;
		s->par = -1;
		s->popping = 0;
		fix_sel(w, 0);
		break;
	default:				/* B_CHILD */
		s->par = s->cur;
		s->cur = (signed char) slot;
		s->depth++;
		fix_sel(w, 0);
		break;
	}
	s->waiting = s->busy = 0;
	s->pagedir = 0;
	s->dirty = 1;
}

/* A window dropped at "show" (malformed, too big). */
static void dropped(struct ses *s)
{
	int slot = s->bslot;

	s->bslot = -1;
	s->waiting = s->busy = 0;
	s->pagedir = 0;
	s->popping = 0;
	s->note = "bad window";
	s->note_t = ticks();
	s->dirty = 1;
	if (slot == s->cur) {		/* it replaced the one on the screen */
		if (s->par >= 0) {
			s->cur = s->par;
			s->par = -1;
			if (s->depth > 1) s->depth--;
		} else
			s->cur = -1;
	}
}

/* One record (no '\n'; len counts it).  -1: the app closed its window. */
static int record(struct ses *s, char *line, int len)
{
	char *f[WR_FIELDS];
	struct wr_win *w = s->bslot >= 0 ? &wins[s->bslot] : NULL;
	int n, r;

	logline(s, "gui< ", line, len - 1);
	n = wr_split(line, f, WR_FIELDS);
	r = n < 0 ? WR_BAD : wr_record(w, f, n, len);
	switch (r) {
	case WR_START:
		start(s, f, n, len);
		break;
	case WR_SHOW:
		shown(s);
		break;
	case WR_BAD:
		if (w != NULL && w->bad && w->st == WS_EMPTY) dropped(s);
		if (s->log) (void) write(2, "gui! dropped\n", 13);
		break;
	case WR_BUSY:
		s->appbusy = 1;
		s->dirty = 1;
		break;
	case WR_IDLE:			/* also: "done, nothing to show" */
		s->appbusy = 0;
		s->waiting = s->busy = 0;
		s->dirty = 1;
		break;
	case WR_CLOSE:
		return -1;
	}
	return 0;
}

/* Records are there (or the app is gone): read what came, handle every
 * complete line.  -1: the window ends. */
static int records(struct ses *s)
{
	char *nl;
	int n, used;

	n = read(s->recfd, lbuf + llen, (unsigned) (WR_LINE - llen));
	if (n <= 0) return n < 0 && errno == EINTR ? 0 : -1;
	llen = (short) (llen + n);
	for (;;) {
		nl = memchr(lbuf, '\n', (size_t) llen);
		if (nl == NULL) {
			if (llen == WR_LINE) {	/* overlong: dropped, and its
						 * window with it */
				lskip = 1;
				llen = 0;
				if (s->bslot >= 0) wins[s->bslot].bad = 1;
				if (s->log) (void) write(2, "gui! overlong\n", 14);
			}
			return 0;
		}
		*nl = 0;
		used = (int) (nl - lbuf) + 1;
		if (lskip) lskip = 0;
		else if (record(s, lbuf, used) < 0) return -1;
		memmove(lbuf, lbuf + used, (size_t) (llen - used));
		llen = (short) (llen - used);
	}
}

/*===========================================================================*
 *				Keys					     *
 *===========================================================================*/
/* The keyboard: 4 rows of 16 characters, then Aa Del Spc Done. */
static const char kb_rows[4][17] = {
	"abcdefghijklmnop", "qrstuvwxyz.,-_'/", "0123456789:;!?@#",
	"&*()+=<>[]%$\"~^|"
};
#define KB_CMDY	4

static void kb_open(struct ses *s, struct wr_win *w)
{
	strncpy(kbt, wr_third(w, w->sel), WR_INMAX);
	kbt[WR_INMAX] = 0;
	s->kb = 1;
	s->kx = s->ky = 0;
	s->shift = 0;
}

static void kb_key(struct ses *s, int k)
{
	struct wr_win *w = &wins[s->cur];
	int n = (int) strlen(kbt), max = w->row[w->sel].val, c;

	switch (k) {
	case KEY_UP: s->ky = (unsigned char) ((s->ky + KB_CMDY) % (KB_CMDY + 1)); break;
	case KEY_DOWN: s->ky = (unsigned char) ((s->ky + 1) % (KB_CMDY + 1)); break;
	case KEY_LEFT:
		s->kx = (unsigned char) (s->ky == KB_CMDY ? (s->kx / 4 + 3) % 4 * 4
					 : (s->kx + 15) % 16);
		break;
	case KEY_RIGHT:
		s->kx = (unsigned char) (s->ky == KB_CMDY ? (s->kx / 4 + 1) % 4 * 4
					 : (s->kx + 1) % 16);
		break;
	case KEY_BACK:
		s->kb = 0;			/* cancel */
		break;
	case KEY_OK:
		if (s->ky < KB_CMDY) {
			c = kb_rows[s->ky][s->kx];
			if (s->shift && c >= 'a' && c <= 'z') c -= 'a' - 'A';
			if (n < max && n < WR_INMAX) {
				kbt[n] = (char) c;
				kbt[n + 1] = 0;
			}
			break;
		}
		switch (s->kx / 4) {
		case 0: s->shift = (unsigned char) !s->shift; break;
		case 1: if (n > 0) kbt[n - 1] = 0; break;
		case 2:
			if (n < max && n < WR_INMAX) {
				kbt[n] = ' ';
				kbt[n + 1] = 0;
			}
			break;
		default:
			if (wr_set_text(w, w->sel, kbt) < 0) {
				s->note = "too long";
				s->note_t = ticks();
			}
			s->kb = 0;
		}
		break;
	}
	s->dirty = 1;
}

/* OK on a window without a cursor (text, clock, confirm): its first
 * action or item, "yes" on a confirm without one. */
static void act(struct ses *s, struct wr_win *w)
{
	int i;

	for (i = 0; i < w->nrows; i++)
		if (w->row[i].kind == RK_ACTION || w->row[i].kind == RK_ITEM) {
			ev(s, "ok", wr_id(w), wr_key(w, i));
			expect(s);
			return;
		}
	if (w->kind == WK_CONFIRM) {
		ev(s, "ok", wr_id(w), "yes");
		expect(s);
	}
}

static void page(struct ses *s, struct wr_win *w, int first, int dir)
{
	char t[8];
	int i = 7;

	submit(s, w);
	t[i] = 0;
	do t[--i] = (char) ('0' + first % 10); while ((first /= 10) > 0 && i > 0);
	ev(s, "page", wr_id(w), t + i);
	s->pagedir = (signed char) dir;
	expect(s);
}

/* Up/Down in a list: the next selectable row; at the end of a page of a
 * long list the next page; a short list wraps round. */
static void move(struct ses *s, struct wr_win *w, int dir)
{
	int i = w->sel, n;

	for (n = 0; n <= w->nrows; n++) {
		i += dir;
		if (i < 0 || i >= w->nrows) {
			if (w->total > 0) {	/* a long list */
				if (dir > 0 && w->first + w->nrows < w->total)
					page(s, w, w->first + w->nrows, 1);
				else if (dir < 0 && w->first > 0)	/* a page
						 * as long as this one */
					page(s, w, w->first > w->nrows
					     ? w->first - w->nrows : 0, -1);
				return;
			}
			i = dir > 0 ? -1 : w->nrows;
			continue;
		}
		if (wr_selectable(w, i)) {
			w->sel = (unsigned char) i;
			fix_sel(w, i);
			return;
		}
	}
}

/* Back always works: out of a field edit, to the parent window, or (on
 * the first window) the app ends at once. */
static char *back(struct ses *s)
{
	struct wr_win *w;

	if (s->edit) {
		s->edit = 0;
		live(s);
		return NULL;
	}
	if (s->cur < 0 || s->depth <= 1) {
		if (s->cur >= 0 && s->bslot != s->cur) submit(s, &wins[s->cur]);
		return end(s, 1);
	}
	w = &wins[s->cur];
	if (s->bslot != s->cur) submit(s, w);
	ev(s, "back", wr_id(w), NULL);
	s->depth--;
	if (s->par >= 0 && s->bslot != s->par) {	/* kept: at once */
		s->cur = s->par;
		s->par = -1;
		fix_sel(&wins[s->cur], wins[s->cur].sel);
	} else {			/* the app sends it again */
		ev(s, "redraw", NULL, NULL);
		s->popping = 1;
		expect(s);
	}
	if (s->bslot >= 0) s->bslot = -1;	/* a window half built: gone */
	return NULL;
}

static char *key(struct ses *s, int k)
{
	struct wr_win *w;
	struct wr_row *r;
	int dir = k == KEY_UP || k == KEY_LEFT ? -1 : 1, nf;

	s->dirty = 1;
	if (s->kb) {
		kb_key(s, k);
		return NULL;
	}
	if (k == KEY_BACK) return back(s);
	if (s->cur < 0 || s->bslot == s->cur || s->popping) return NULL;
	w = &wins[s->cur];
	if (w->kind != WK_LIST) {
		if (k == KEY_OK) act(s, w);
		else if (w->kind == WK_TEXT && (k == KEY_UP || k == KEY_DOWN)) {
			int t = w->top + dir;

			if (t >= 0 && t + VIS <= w->nrows) w->top = (unsigned char) t;
		}
		return NULL;
	}
	if (w->nrows == 0) return NULL;
	r = &w->row[w->sel];
	nf = wr_fields(r->kind);
	if (s->edit) {			/* a field of a time, date, days row */
		if (k == KEY_UP || k == KEY_DOWN)
			wr_field(w, w->sel, s->edit - 1, -dir);
		else if (k == KEY_LEFT || k == KEY_RIGHT)
			s->edit = (char) ((s->edit - 1 + dir + nf) % nf + 1);
		else if (k == KEY_OK) {
			s->edit = 0;
			live(s);
		}
		return NULL;
	}
	switch (k) {
	case KEY_UP: case KEY_DOWN:
		move(s, w, dir);
		break;
	case KEY_LEFT: case KEY_RIGHT:
		if (wr_change(w, w->sel, dir) == 0) live(s);
		break;
	case KEY_OK:
		switch (r->kind) {
		case RK_ITEM: case RK_ACTION:
			submit(s, w);
			ev(s, "ok", wr_id(w), wr_key(w, w->sel));
			expect(s);
			break;
		case RK_INT: case RK_ENUM: case RK_BOOL:
			if (wr_change(w, w->sel, 1) == 0) live(s);
			break;
		case RK_INPUT:
			kb_open(s, w);
			break;
		default:
			if (nf > 0) s->edit = 1;
		}
		break;
	}
	return NULL;
}

/*===========================================================================*
 *				Drawing					     *
 *===========================================================================*/
/* at most n characters of t at x, y */
static int text_n(int x, int y, const char *t, int n, int inv)
{
	char b[33];
	int i;

	if (n > 32) n = 32;
	for (i = 0; i < n && t[i] != 0; i++)
		b[i] = t[i] == '\t' || t[i] == '\n' ? ' ' : t[i];
	b[i] = 0;
	return gui_text(x, y, b, inv);
}

static void center_n(int y, const char *t)
{
	int n = (int) strlen(t);

	if (n > 32) n = 32;
	text_n((GUI_W - n * 4) / 2, y, t, n, 0);
}

static const unsigned char hourglass[5] = { 0xF8, 0x50, 0x20, 0x50, 0xF8 };

static void title(struct ses *s, struct wr_win *w)
{
	char t[33], pos[12], *q;
	const char *src = w != NULL && *wr_title(w) != 0 ? wr_title(w) : s->label;
	int i, a, b;

	for (i = 0; i < 26 && src[i] != 0; i++)
		t[i] = (char) (src[i] >= 'a' && src[i] <= 'z' ? src[i] - 32 : src[i]);
	t[i] = 0;
	gui_title(t);
	if (s->busy || s->appbusy) {
		gui_bits8(GUI_W - 7, 1, hourglass, 5, 1);
		return;
	}
	if (w == NULL || w->kind != WK_LIST) return;
	a = w->first + w->sel + 1;
	b = w->total > 0 ? w->total : w->nrows;
	if (b <= VIS) return;
	q = pos + 11;			/* "a/b" on the right */
	*q = 0;
	do *--q = (char) ('0' + b % 10); while ((b /= 10) > 0);
	*--q = '/';
	do *--q = (char) ('0' + a % 10); while ((a /= 10) > 0);
	gui_text(GUI_W - (int) strlen(q) * 4 - 1, 1, q, 1);
}

/* The chars of field f in a value ("[HH:MM]" etc.): first and count. */
static void field_at(int kind, int f, int *at, int *n)
{
	static const unsigned char tm[2][2] = { { 0, 2 }, { 3, 2 } };
	static const unsigned char dt[3][2] = { { 0, 4 }, { 5, 2 }, { 8, 2 } };

	if (kind == RK_TIME) { *at = tm[f][0]; *n = tm[f][1]; }
	else if (kind == RK_DATE) { *at = dt[f][0]; *n = dt[f][1]; }
	else { *at = f; *n = 1; }
}

static void row(struct ses *s, struct wr_win *w, int i, int y, int sel)
{
	struct wr_row *r = &w->row[i];
	char v[WR_INMAX + 4];
	int vl, vx = GUI_W - 1, lx = 6, room;

	if (r->kind == RK_LINE) {
		text_n(1, y, wr_label(w, i), 31, 0);
		return;
	}
	if (sel) gui_fill(0, y - 1, GUI_W - 1, y + 5, 1);
	if (sel && (r->kind == RK_ITEM || r->kind == RK_ACTION))
		gui_text(1, y, ">", 1);
	v[0] = '[';
	vl = wr_value(w, i, v + 1, (int) sizeof v - 2, 0);
	if (vl > 14) {			/* a long text: its start and ".." */
		vl = 14;
		v[13] = v[14] = '.';
	}
	if (r->kind == RK_ITEM) {	/* RIGHT: plain */
		if (vl > 0) vx = GUI_W - 2 - vl * 4;
		text_n(vx, y, v + 1, vl, sel);
	} else if (r->kind != RK_ACTION) {
		v[vl + 1] = ']';
		v[vl + 2] = 0;
		vl += 2;
		vx = GUI_W - 2 - vl * 4;
		text_n(vx, y, v, vl, sel);
		if (sel && s->edit) {	/* the field being edited: white */
			int at, n;

			field_at(r->kind, s->edit - 1, &at, &n);
			gui_fill(vx + (at + 1) * 4 - 1, y - 1,
				 vx + (at + 1 + n) * 4 - 1, y + 5, 0);
			text_n(vx + (at + 1) * 4, y, v + 1 + at, n, 0);
		}
	}
	room = (vx - lx) / 4 - 1;
	text_n(lx, y, wr_label(w, i), room, sel);
}

static void hints(struct ses *s, struct wr_win *w, char *h)
{
	const char *t = "Back";
	int i;

	h[0] = 0;
	if (s->kb) t = "OK Type  Back Cancel";
	else if (w == NULL) ;
	else if (w->kind == WK_LIST && w->nrows > 0) {
		struct wr_row *r = &w->row[w->sel];

		if (s->edit)
			t = r->kind == RK_DAYS ? "<> Day  ^v Toggle  OK Done"
					       : "<> Field  ^v Change  OK Done";
		else switch (r->kind) {
		case RK_ITEM: t = "OK Open  Back"; break;
		case RK_INT: case RK_ENUM: case RK_BOOL: t = "<> Change  Back"; break;
		case RK_TIME: case RK_DATE: case RK_DAYS: t = "OK Edit  Back"; break;
		case RK_INPUT: t = "OK Type  Back"; break;
		case RK_ACTION:
			strcpy(h, "OK ");
			strncat(h, wr_label(w, w->sel), 16);
			t = "  Back";
			break;
		}
	} else if (w->kind != WK_LIST) {
		for (i = 0; i < w->nrows; i++)
			if (w->row[i].kind == RK_ACTION || w->row[i].kind == RK_ITEM)
				break;
		if (i < w->nrows || w->kind == WK_CONFIRM) {
			strcpy(h, "OK ");
			strncat(h, i < w->nrows ? wr_label(w, i) : "Yes", 12);
			strcat(h, "  ");
		}
		t = w->kind == WK_CONFIRM ? "Back No"
		  : w->kind == WK_TEXT && w->nrows > VIS ? "^v Scroll  Back" : "Back";
	}
	strcat(h, t);
}

static void draw_kb(struct ses *s)
{
	struct wr_win *w = &wins[s->cur];
	static const char cmds[4][5] = { "Aa", "Del", "Spc", "Done" };
	char c[2];
	int n = (int) strlen(kbt), x, y, i;

	gui_clear();
	{	/* the row's label as the title */
		char t[27];

		for (i = 0; i < 26 && wr_label(w, w->sel)[i] != 0; i++) {
			int ch = wr_label(w, w->sel)[i];

			t[i] = (char) (ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
		}
		t[i] = 0;
		gui_title(t);
	}
	/* the text, its end if long, and the cursor */
	text_n(1, 8, n > 30 ? kbt + n - 30 : kbt, 30, 0);
	gui_hline(1 + (n > 30 ? 30 : n) * 4, 4 + (n > 30 ? 30 : n) * 4, 14, 1);
	c[1] = 0;
	for (y = 0; y < KB_CMDY; y++)
		for (x = 0; x < 16; x++) {
			int on = s->ky == y && s->kx == x;

			c[0] = kb_rows[y][x];
			if (s->shift && c[0] >= 'a' && c[0] <= 'z') c[0] -= 'a' - 'A';
			if (on) gui_fill(x * 8, 17 + y * 7, x * 8 + 7, 23 + y * 7, 1);
			gui_text(x * 8 + 2, 18 + y * 7, c, on);
		}
	for (i = 0; i < 4; i++) {
		int on = s->ky == KB_CMDY && s->kx / 4 == i;

		if (on) gui_fill(i * 32, 45, i * 32 + 31, 51, 1);
		gui_text(i * 32 + (32 - (int) strlen(cmds[i]) * 4) / 2, 46,
			 cmds[i], on);
	}
	gui_hline(0, GUI_W - 1, HINT_Y - 2, 1);
	gui_text(1, HINT_Y, s->note != NULL ? s->note : "OK Type  Back Cancel", 0);
	gui_flush();
}

static void draw(struct ses *s)
{
	struct wr_win *w = s->cur >= 0 ? &wins[s->cur] : NULL;
	char h[40];
	int i, y;

	s->dirty = 0;
	if (s->cur >= 0 && s->bslot == s->cur)
		return;			/* being sent again: the old frame stays */
	if (s->kb && w != NULL) {
		draw_kb(s);
		return;
	}
	gui_clear();
	title(s, w);
	if (w != NULL) switch (w->kind) {
	case WK_LIST:
	case WK_TEXT:
		for (i = 0; i < VIS && w->top + i < w->nrows; i++)
			row(s, w, w->top + i, ROW_Y(i),
			    w->kind == WK_LIST && w->top + i == w->sel);
		if (w->nrows > VIS || w->total > w->nrows) {	/* scroll mark */
			int all = w->total > w->nrows ? w->total : w->nrows;
			int at = w->first + w->top, h6 = VIS * 6;
			int len = h6 * VIS / all, off = h6 * at / all;

			int sy = ROW_Y(w->sel - w->top);

			if (len < 2) len = 2;
			for (i = 6 + off; i < 6 + off + len && i < 55; i++)
				if (i > 6) gui_pixel(GUI_W - 1, i, w->kind != WK_LIST
					  || i < sy - 1 || i > sy + 5);
		}
		break;
	case WK_CONFIRM:
		for (i = 0, y = 18; i < w->nrows && y < 50; i++)
			if (w->row[i].kind == RK_LINE) {
				center_n(y, wr_label(w, i));
				y += 7;
			}
		break;
	case WK_CLOCK: {
		time_t now = time((time_t *) NULL);
		struct tm *tm = localtime(&now);
		char t[6];

		t[0] = (char) ('0' + tm->tm_hour / 10);
		t[1] = (char) ('0' + tm->tm_hour % 10);
		t[2] = ':';
		t[3] = (char) ('0' + tm->tm_min / 10);
		t[4] = (char) ('0' + tm->tm_min % 10);
		t[5] = 0;
		gui_text_big(24, 10, t, 4);
		for (i = 0, y = 38; i < w->nrows && y < 52; i++)
			if (w->row[i].kind == RK_LINE) {
				center_n(y, wr_label(w, i));
				y += 7;
			}
		break;
	}
	}
	gui_hline(0, GUI_W - 1, HINT_Y - 2, 1);
	hints(s, w, h);
	gui_text(1, HINT_Y, s->note != NULL ? s->note : h, 0);
	gui_flush();
}

/*===========================================================================*
 *				The session				     *
 *===========================================================================*/
/* ms until the next thing to do without a key: the busy mark, the end
 * of a notice, the clock's next minute; -1: nothing (0 wakeups). */
static int timeout(struct ses *s)
{
	int t = -1, m;

	if (s->waiting && !s->busy) t = ms_until(s->wait_t + ms_ticks(BUSY_MS));
	if (s->note != NULL) {
		m = ms_until(s->note_t + ms_ticks(NOTE_MS));
		if (t < 0 || m < t) t = m;
	}
	if (s->cur >= 0 && wins[s->cur].kind == WK_CLOCK && !s->kb) {
		time_t now = time((time_t *) NULL);

		m = (int) (60 - now % 60) * 1000;
		if (t < 0 || m < t) t = m;
	}
	return t;
}

static void timed(struct ses *s)
{
	if (s->waiting && !s->busy
	    && ticks() - s->wait_t >= ms_ticks(BUSY_MS)) {
		s->busy = 1;
		s->dirty = 1;
	}
	if (s->note != NULL && ticks() - s->note_t >= ms_ticks(NOTE_MS)) {
		s->note = NULL;
		s->dirty = 1;
	}
	if (s->cur >= 0 && wins[s->cur].kind == WK_CLOCK) s->dirty = 1;
}

/* Start a window app and serve it until it ends or Back closes it.  NULL,
 * or a notice for the launcher's message line. */
char *win_run(const char *label, const char *cmd, struct passwd *pw, int log)
{
	char c[CMDMAX], *argv[MAXARGS + 1], *p, *note = NULL;
	struct ses s;
	struct gui_in in;
	int evp[2], recp[2], argc = 0, k, fd;

	strncpy(c, cmd, CMDMAX - 1);
	c[CMDMAX - 1] = 0;
	for (p = strtok(c, " "); p != NULL && argc < MAXARGS; p = strtok(NULL, " "))
		argv[argc++] = p;
	argv[argc] = NULL;
	if (argc == 0) return NULL;
	win_reap(1);			/* a closed one still alive: SIGTERM */
	if (pipe(evp) < 0) return "cannot start";
	if (pipe(recp) < 0) {
		close(evp[0]); close(evp[1]);
		return "cannot start";
	}
	memset(&s, 0, sizeof s);
	s.label = label;
	s.log = (char) log;
	s.cur = s.par = s.bslot = -1;
	s.evfd = evp[1];
	s.recfd = recp[0];
	llen = 0;
	lskip = 0;
	/* the desktop never blocks on the app (and a dead app's pipe gives
	 * EPIPE, not a signal: SIGPIPE is ignored by the launcher) */
	(void) fcntl(s.evfd, F_SETFL, O_NONBLOCK);
	{	/* R12.22: the app low in RAM, as a full-screen app */
		message m;

		m.m1_i2 = MI_FGEXEC;
		(void) _syscall(MM, MEMINFO, &m);
	}
	if ((s.pid = vfork()) == 0) {	/* the child: ids, fds 3 and 4, exec */
		if (pw->pw_uid != 0) {
			if (setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0)
				_exit(126);	/* never with root rights */
		}
		chdir(pw->pw_dir[0] != 0 ? pw->pw_dir : "/");
		if (dup2(evp[0], 3) < 0 || dup2(recp[1], 4) < 0) _exit(126);
		for (fd = 5; fd < OPEN_MAX; fd++) close(fd);
		signal(SIGPIPE, SIG_DFL);
		execv(argv[0], argv);
		_exit(127);
	}
	close(evp[0]);
	close(recp[1]);
	if (s.pid < 0) {
		close(s.evfd);
		close(s.recfd);
		return "cannot start";
	}
	signal(SIGPIPE, SIG_IGN);	/* EPIPE from a dead app instead */
	expect(&s);			/* its first window (the screen as it
					 * was until it comes or 150 ms pass) */
	memset(&in, 0, sizeof in);
	while (!s.ended) {
		if (s.dirty) draw(&s);
		k = gui_wait(&in, KEY_LEFT | KEY_RIGHT, timeout(&s), s.recfd);
		if (k == GUI_FD) {
			if (records(&s) < 0) note = end(&s, 0);
			continue;
		}
		if (k == 0) {
			timed(&s);
			continue;
		}
		if (s.note != NULL && !s.kb) s.note = NULL;
		note = key(&s, k);
		if (!s.ended && s.waiting) timed(&s);
	}
	signal(SIGPIPE, SIG_DFL);
	return note;
}
