/* winrec - the window records of spec K.8: a window app describes its
 * screens in TSV records, the desktop (the launcher) keeps them here,
 * draws them, moves the cursor and changes the values.  Pure functions,
 * no I/O and no allocation: the launcher feeds them the lines it reads
 * from the app, the host test (flipper/tests/host/wintest.c) feeds them
 * every row type and every way to get one wrong.
 *
 * Records, one per line, fields separated by tabs; \t \n \\ escape a tab,
 * a newline and a backslash inside a field (spec K.1):
 *
 *	win ID KIND TITLE [ROWS=N [FIRST=K]] [LIVE] [REPEAT=MS]
 *						KIND list|text|clock|confirm|pad
 *	item KEY LABEL [RIGHT]			opens something (OK)
 *	int KEY LABEL MIN MAX STEP VALUE [UNIT]	values changed in the desktop
 *	enum KEY LABEL INDEX OPT|OPT|...
 *	bool KEY LABEL 0|1
 *	time KEY LABEL HH:MM
 *	date KEY LABEL YYYY-MM-DD		2000-2127
 *	days KEY LABEL MASK			bit 0 Monday ... bit 6 Sunday
 *	input KEY LABEL TEXT MAX		MAX <= 63: the desktop's keyboard
 *	line TEXT				a static line
 *	action KEY LABEL			a button row
 *	show					the window is complete
 *	busy, idle, close
 *
 * A pad window has only line rows: its keys go to the app raw ("key KEY
 * press|release|repeat", winsrv.c), a repeat every REPEAT ms (10-1000)
 * while one is held.
 *
 * A window holds at most WR_ROWS rows and WR_BYTES bytes of records (win
 * to show); more, or more strings than its pool holds, drops the whole
 * window at show.  A malformed row is dropped alone.
 */
#include <string.h>
#include "winrec.h"

/* Verbs: no table of pointers (a PIC program's pointers would be
 * relocated into RAM), one string of 7-byte names instead. */
static const char verbs[] =
	"win\0\0\0\0" "show\0\0\0" "busy\0\0\0" "idle\0\0\0" "close\0\0"
	"item\0\0\0" "int\0\0\0\0" "enum\0\0\0" "bool\0\0\0" "time\0\0\0"
	"date\0\0\0" "days\0\0\0" "input\0\0" "line\0\0\0" "action\0";
#define V_WIN	0
#define V_SHOW	1
#define V_BUSY	2
#define V_IDLE	3
#define V_CLOSE	4
#define V_ROW	5			/* V_ROW + RK_x - 1 */
#define NVERBS	15

static const char kinds[] = "list\0\0\0\0" "text\0\0\0\0" "clock\0\0\0"
			    "confirm\0" "pad\0\0\0\0\0";
#define NKINDS	5

static int verb(const char *s)
{
	int i;

	for (i = 0; i < NVERBS; i++)
		if (strcmp(s, verbs + 7 * i) == 0) return i;
	return -1;
}

/* Split a record (no '\n') at its tabs, in place, and undo the escapes.
 * The number of fields, or -1 for more than max fields. */
int wr_split(char *line, char **f, int max)
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
			*w++ = *r == 't' ? '\t' : *r == 'n' ? '\n' : *r;
		} else if (*r == '\r' && r[1] == 0)
			;			/* a CR LF line end */
		else
			*w++ = *r;
	}
	*w = 0;
	return n;
}

/* A decimal number, - allowed, -32768..32767: 0, or -1 if malformed. */
static int num(const char *s, int *v)
{
	int neg = *s == '-', d = 0;
	long x = 0;

	if (neg) s++;
	for (; *s >= '0' && *s <= '9'; s++, d++) {
		x = x * 10 + *s - '0';
		if (x > 32768L) return -1;
	}
	if (d == 0 || *s != 0) return -1;
	if (neg) x = -x;
	if (x > 32767L) return -1;
	*v = (int) x;
	return 0;
}

/* Exactly n digits from s: the value, or -1. */
static int digits(const char *s, int n)
{
	int v = 0;

	while (n-- > 0) {
		if (*s < '0' || *s > '9') return -1;
		v = v * 10 + *s++ - '0';
	}
	return v;
}

int wr_days_in(int y, int m)
{
	static const unsigned char dm[12] = { 31, 28, 31, 30, 31, 30, 31,
					      31, 30, 31, 30, 31 };

	if (m < 1 || m > 12) return 31;
	if (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) return 29;
	return dm[m - 1];
}

#define D_Y(v)	(((unsigned short) (v) >> 9) + 2000)
#define D_M(v)	(((unsigned short) (v) >> 5) & 15)
#define D_D(v)	((unsigned short) (v) & 31)
#define D_PACK(y, m, d)	((short) (unsigned short) (((y) - 2000) << 9 | (m) << 5 | (d)))

/* A string into the pool: its offset (0 and w->bad on overflow). */
static unsigned short put(struct wr_win *w, const char *s)
{
	unsigned short at = w->used;
	size_t n = strlen(s) + 1;

	if (w->used + n > WR_POOL) {
		w->bad = 1;
		return 0;
	}
	memcpy(w->pool + at, s, n);
	w->used = (unsigned short) (w->used + n);
	return at;
}

void wr_start(struct wr_win *w, char **f, int n, int len)
{
	int i, v;

	w->used = 0;
	w->bytes = (unsigned short) len;
	w->nrows = w->sel = w->top = 0;
	w->bad = 0;
	w->st = WS_BUILD;
	w->total = w->first = 0;
	w->live = 0;
	w->rep = 0;
	w->kind = WK_LIST;
	if (n < 4 || n > 8 || f[1][0] == 0) w->bad = 1;
	for (i = 0; n >= 3 && i < NKINDS; i++)
		if (strcmp(f[2], kinds + 8 * i) == 0) break;
	if (n < 3 || i == NKINDS) w->bad = 1;
	else w->kind = (unsigned char) i;
	w->id = put(w, n > 1 ? f[1] : "");
	w->title = put(w, n > 3 ? f[3] : "");
	for (i = 4; i < n; i++) {
		if (strncmp(f[i], "ROWS=", 5) == 0 && num(f[i] + 5, &v) == 0
		    && v >= 0)
			w->total = (unsigned short) v;
		else if (strncmp(f[i], "FIRST=", 6) == 0 && num(f[i] + 6, &v) == 0
			 && v >= 0)
			w->first = (unsigned short) v;
		else if (strcmp(f[i], "LIVE") == 0)
			w->live = 1;
		else if (strncmp(f[i], "REPEAT=", 7) == 0
			 && num(f[i] + 7, &v) == 0 && v >= 10 && v <= 1000
			 && w->kind == WK_PAD)
			w->rep = (unsigned char) (v / 10);
		else
			w->bad = 1;
	}
	if (w->first > 0 && w->first >= w->total) w->bad = 1;
}

/* key, label and third string of a new row; the row's offset, or -1. */
static int strings(struct wr_win *w, const char *k, const char *l,
		   const char *t)
{
	unsigned short o = w->used;

	put(w, k);
	put(w, l);
	put(w, t);
	return w->bad ? -1 : o;
}

/* One row (f[0] its verb, n fields).  0, or -1 if malformed. */
static int add_row(struct wr_win *w, int kind, char **f, int n)
{
	struct wr_row *r = &w->row[w->nrows];
	int o, v, a, b, c, i;
	short s[3];
	const char *t = "";

	r->kind = (unsigned char) kind;
	r->flags = 0;
	r->val = 0;
	switch (kind) {
	case RK_ITEM:
		if (n < 3 || n > 4) return -1;
		if (n == 4) t = f[3];
		break;
	case RK_INT:
		if (n < 7 || n > 8) return -1;
		if (num(f[3], &a) < 0 || num(f[4], &b) < 0 || num(f[5], &c) < 0
		    || num(f[6], &v) < 0 || a > b || c < 1) return -1;
		if (v < a) v = a;
		if (v > b) v = b;
		r->val = (short) v;
		if (n == 8) t = f[7];
		s[0] = (short) a; s[1] = (short) b; s[2] = (short) c;
		break;
	case RK_ENUM:
		if (n != 5 || num(f[3], &v) < 0 || f[4][0] == 0) return -1;
		for (a = 1, i = 0; f[4][i] != 0; i++) if (f[4][i] == '|') a++;
		if (a > 255 || v < 0 || v >= a) return -1;
		r->flags = (unsigned char) a;
		r->val = (short) v;
		t = f[4];
		break;
	case RK_BOOL:
		if (n != 4 || (strcmp(f[3], "0") != 0 && strcmp(f[3], "1") != 0))
			return -1;
		r->val = (short) (f[3][0] - '0');
		break;
	case RK_TIME:
		if (n != 4 || strlen(f[3]) != 5 || f[3][2] != ':') return -1;
		a = digits(f[3], 2);
		b = digits(f[3] + 3, 2);
		if (a < 0 || a > 23 || b < 0 || b > 59) return -1;
		r->val = (short) (a * 60 + b);
		break;
	case RK_DATE:
		if (n != 4 || strlen(f[3]) != 10 || f[3][4] != '-'
		    || f[3][7] != '-') return -1;
		a = digits(f[3], 4);
		b = digits(f[3] + 5, 2);
		c = digits(f[3] + 8, 2);
		if (a < 2000 || a > 2127 || b < 1 || b > 12 || c < 1
		    || c > wr_days_in(a, b)) return -1;
		r->val = D_PACK(a, b, c);
		break;
	case RK_DAYS:
		if (n != 4 || num(f[3], &v) < 0 || v < 0 || v > 127) return -1;
		r->val = (short) v;
		break;
	case RK_INPUT:
		if (n != 5 || num(f[4], &v) < 0 || v < 1 || v > WR_INMAX
		    || (int) strlen(f[3]) > v) return -1;
		r->val = (short) v;
		t = f[3];
		break;
	case RK_LINE:
		if (n != 2) return -1;
		break;
	case RK_ACTION:
		if (n != 3) return -1;
		break;
	}
	if (kind != RK_LINE && f[1][0] == 0) return -1;	/* no key */
	o = kind == RK_LINE ? strings(w, "", f[1], "")
			    : strings(w, f[1], f[2], t);
	if (o < 0) return -1;
	if (kind == RK_INT) {		/* MIN MAX STEP at an even offset */
		if ((w->used & 1) && w->used < WR_POOL) w->used++;
		if (w->used + sizeof s > WR_POOL) {
			w->bad = 1;
			return -1;
		}
		memcpy(w->pool + w->used, s, sizeof s);
		w->used = (unsigned short) (w->used + sizeof s);
	}
	r->off = (unsigned short) o;
	r->orig = r->val;
	w->nrows++;
	return 0;
}

/* One record, split by wr_split (f[0] the verb, n fields; len its bytes
 * with the '\n').  w: the window being built, or NULL if none.  "win"
 * only returns WR_START: the caller chooses the window and calls
 * wr_start. */
int wr_record(struct wr_win *w, char **f, int n, int len)
{
	int v = n > 0 ? verb(f[0]) : -1;

	if (v < 0) return WR_BAD;
	switch (v) {
	case V_WIN: return WR_START;
	case V_BUSY: return n == 1 ? WR_BUSY : WR_BAD;
	case V_IDLE: return n == 1 ? WR_IDLE : WR_BAD;
	case V_CLOSE: return n == 1 ? WR_CLOSE : WR_BAD;
	}
	if (w == NULL || w->st != WS_BUILD) return WR_BAD;	/* no "win" */
	w->bytes = (unsigned short) (w->bytes + len > 0xFFFF ? 0xFFFF
						: w->bytes + len);
	if (w->bytes > WR_BYTES) w->bad = 1;
	if (v == V_SHOW) {
		if (n != 1 || w->bad) {
			w->bad = 1;
			w->st = WS_EMPTY;
			return WR_BAD;
		}
		w->st = WS_SHOWN;
		return WR_SHOW;
	}
	if (w->bad) return WR_BAD;
	if (w->nrows == WR_ROWS) {
		w->bad = 1;
		return WR_BAD;
	}
	if (w->kind == WK_PAD && v - V_ROW + 1 != RK_LINE)
		return WR_BAD;			/* a pad has lines only */
	return add_row(w, v - V_ROW + 1, f, n) < 0 ? WR_BAD : WR_NONE;
}

const char *wr_id(const struct wr_win *w)
{
	return w->pool + w->id;
}

const char *wr_title(const struct wr_win *w)
{
	return w->pool + w->title;
}

const char *wr_key(const struct wr_win *w, int i)
{
	return w->pool + w->row[i].off;
}

const char *wr_label(const struct wr_win *w, int i)
{
	const char *k = wr_key(w, i);

	return k + strlen(k) + 1;
}

const char *wr_third(const struct wr_win *w, int i)
{
	const char *l = wr_label(w, i);

	return l + strlen(l) + 1;
}

/* An int row's MIN MAX STEP */
static void limits(const struct wr_win *w, int i, short *s)
{
	const char *t = wr_third(w, i);
	unsigned o = (unsigned) (t - w->pool) + (unsigned) strlen(t) + 1;

	memcpy(s, w->pool + ((o + 1) & ~1u), 3 * sizeof *s);
}

int wr_changed(const struct wr_win *w, int i)
{
	const struct wr_row *r = &w->row[i];

	switch (r->kind) {
	case RK_INPUT: return (r->flags & RF_CHANGED) != 0;
	case RK_INT: case RK_ENUM: case RK_BOOL: case RK_TIME: case RK_DATE:
	case RK_DAYS:
		return r->val != r->orig;
	}
	return 0;
}

int wr_selectable(const struct wr_win *w, int i)
{
	return w->row[i].kind != RK_LINE;
}

/* Text helpers, no sprintf (it would bring stdio's data along). */
static char *dec(char *p, char *end, int v, int width)
{
	char t[8];
	int n = 0;
	unsigned u = (unsigned) (v < 0 ? -v : v);

	do t[n++] = (char) ('0' + u % 10); while ((u /= 10) > 0 && n < 7);
	while (n < width && n < 7) t[n++] = '0';
	if (v < 0 && p < end) *p++ = '-';
	while (n > 0 && p < end) *p++ = t[--n];
	return p;
}

static char *str(char *p, char *end, const char *s)
{
	while (*s != 0 && p < end) *p++ = *s++;
	return p;
}

/* Row i's value as text into buf (NUL-terminated); wire: as sent to the
 * app in "submit" (enum: the index, days: the mask), else as shown
 * (enum: the option, bool on/off, days MTWTFSS).  Its length. */
int wr_value(const struct wr_win *w, int i, char *buf, int size, int wire)
{
	const struct wr_row *r = &w->row[i];
	char *p = buf, *end = buf + size - 1;
	const char *t;
	int k, v = r->val;

	if (size <= 0) return 0;
	switch (r->kind) {
	case RK_ITEM:
		p = str(p, end, wr_third(w, i));
		break;
	case RK_INT:
		p = dec(p, end, v, 1);
		t = wr_third(w, i);
		if (!wire && *t != 0) {
			if (p < end) *p++ = ' ';
			p = str(p, end, t);
		}
		break;
	case RK_ENUM:
		if (wire) {
			p = dec(p, end, v, 1);
			break;
		}
		for (t = wr_third(w, i), k = 0; k < v; t++)
			if (*t == '|') k++;
		while (*t != 0 && *t != '|' && p < end) *p++ = *t++;
		break;
	case RK_BOOL:
		p = str(p, end, wire ? (v ? "1" : "0") : (v ? "on" : "off"));
		break;
	case RK_TIME:
		p = dec(p, end, v / 60, 2);
		if (p < end) *p++ = ':';
		p = dec(p, end, v % 60, 2);
		break;
	case RK_DATE:
		p = dec(p, end, D_Y(v), 4);
		if (p < end) *p++ = '-';
		p = dec(p, end, D_M(v), 2);
		if (p < end) *p++ = '-';
		p = dec(p, end, D_D(v), 2);
		break;
	case RK_DAYS:
		if (wire) {
			p = dec(p, end, v, 1);
			break;
		}
		for (k = 0; k < 7 && p < end; k++)
			*p++ = v & (1 << k) ? "MTWTFSS"[k] : '-';
		break;
	case RK_INPUT:
		p = str(p, end, wr_third(w, i));
		break;
	}
	*p = 0;
	return (int) (p - buf);
}

/* Left/Right (dir -1, +1) on an int, enum or bool row: 1 if the value
 * changed. */
int wr_change(struct wr_win *w, int i, int dir)
{
	struct wr_row *r = &w->row[i];
	short s[3];
	long v;

	switch (r->kind) {
	case RK_INT:
		limits(w, i, s);
		v = (long) r->val + (long) dir * s[2];
		if (v < s[0]) v = s[0];
		if (v > s[1]) v = s[1];
		if (v == r->val) return 0;
		r->val = (short) v;
		return 1;
	case RK_ENUM:
		if (r->flags < 2) return 0;
		r->val = (short) ((r->val + dir + r->flags) % r->flags);
		return 1;
	case RK_BOOL:
		r->val = (short) !r->val;
		return 1;
	}
	return 0;
}

/* Fields edited one by one (OK on the row, Left/Right choose, Up/Down
 * change): time HH MM, date YYYY MM DD, days Mo..Su. */
int wr_fields(int kind)
{
	return kind == RK_TIME ? 2 : kind == RK_DATE ? 3 : kind == RK_DAYS ? 7 : 0;
}

static int wrap(int v, int lo, int hi)
{
	return v < lo ? hi : v > hi ? lo : v;
}

/* Up/Down (dir +1, -1) on field `field` of a time, date or days row: 1
 * if the value changed.  A day that no longer fits the month becomes its
 * last. */
int wr_field(struct wr_win *w, int i, int field, int dir)
{
	struct wr_row *r = &w->row[i];
	int v = r->val, y, m, d;

	switch (r->kind) {
	case RK_TIME:
		if (field == 0) v = wrap(v / 60 + dir, 0, 23) * 60 + v % 60;
		else if (field == 1) v = v / 60 * 60 + wrap(v % 60 + dir, 0, 59);
		break;
	case RK_DATE:
		y = D_Y(v); m = D_M(v); d = D_D(v);
		if (field == 0) y = wrap(y + dir, 2000, 2127);
		else if (field == 1) m = wrap(m + dir, 1, 12);
		else if (field == 2) d = wrap(d + dir, 1, wr_days_in(y, m));
		if (d > wr_days_in(y, m)) d = wr_days_in(y, m);
		v = D_PACK(y, m, d);
		break;
	case RK_DAYS:
		if (field >= 0 && field < 7) v ^= 1 << field;
		break;
	default:
		return 0;
	}
	if ((short) v == r->val) return 0;
	r->val = (short) v;
	return 1;
}

/* The keyboard's text into input row i.  0, or -1 if too long or the
 * pool is full. */
int wr_set_text(struct wr_win *w, int i, const char *s)
{
	struct wr_row *r = &w->row[i];
	char *t;
	size_t n = strlen(s), lk, ll;
	unsigned short o;

	if (r->kind != RK_INPUT || (int) n > r->val) return -1;
	t = (char *) wr_third(w, i);
	if (strcmp(t, s) == 0) return 0;
	if (n > strlen(t)) {		/* longer: the row's strings to the end */
		lk = strlen(wr_key(w, i)) + 1;
		ll = strlen(wr_label(w, i)) + 1;
		if (w->used + lk + ll + n + 1 > WR_POOL) return -1;
		o = w->used;
		memmove(w->pool + o, w->pool + r->off, lk + ll);
		r->off = o;
		w->used = (unsigned short) (o + lk + ll);
		t = w->pool + w->used;
		w->used = (unsigned short) (w->used + n + 1);
	}
	memcpy(t, s, n + 1);
	r->flags |= RF_CHANGED;
	return 0;
}

/* The app has the values now: nothing is changed any more. */
void wr_commit(struct wr_win *w)
{
	int i;

	for (i = 0; i < w->nrows; i++) {
		w->row[i].orig = w->row[i].val;
		if (w->row[i].kind == RK_INPUT) w->row[i].flags = 0;
	}
}

/* s with \t \n \\ escaped into dst: its length, or -1 if it does not fit
 * in size (with the NUL). */
int wr_esc(char *dst, int size, const char *s)
{
	int n = 0;

	for (; *s != 0; s++) {
		int e = *s == '\t' || *s == '\n' || *s == '\\';

		if (n + e + 1 >= size) return -1;
		if (e) {
			dst[n++] = '\\';
			dst[n++] = *s == '\t' ? 't' : *s == '\n' ? 'n' : '\\';
		} else
			dst[n++] = *s;
	}
	if (n >= size) return -1;
	dst[n] = 0;
	return n;
}

/* "submit ID KEY=VALUE ...\n" with the changed rows from *from on, as
 * many as fit in size; *from is the next row to send.  Its length, 0 if
 * no changed row is left.  A pair that never fits is skipped. */
int wr_submit(const struct wr_win *w, char *buf, int size, int *from)
{
	char v[WR_INMAX + 2];
	int n, k, i, pairs = 0;

	if (size < 16) return 0;
	memcpy(buf, "submit\t", 7);
	n = 7;
	if ((k = wr_esc(buf + n, size - n, wr_id(w))) < 0) return 0;
	n += k;
	for (i = *from; i < w->nrows; i++) {
		int m = n;

		if (!wr_changed(w, i)) continue;
		wr_value(w, i, v, sizeof v, 1);
		if (m + 2 < size) buf[m++] = '\t';
		if (m + 2 >= size || (k = wr_esc(buf + m, size - m - 1,
						 wr_key(w, i))) < 0) goto full;
		m += k;
		buf[m++] = '=';
		if ((k = wr_esc(buf + m, size - m - 1, v)) < 0) goto full;
		n = m + k;
		pairs++;
		continue;
	full:
		if (pairs == 0) continue;	/* never fits: skipped */
		break;
	}
	*from = i;
	if (pairs == 0) return 0;
	buf[n++] = '\n';
	buf[n] = 0;
	return n;
}
