/* winrec - the window records of spec K.8 (window apps), see winrec.c */
#ifndef GUI_WINREC_H
#define GUI_WINREC_H

#define WR_BYTES	640		/* the records of one window, win..show */
#define WR_LINE		128		/* one record, its '\n' included */
#define WR_ROWS		16		/* rows of one window (one page) */
#define WR_POOL		512		/* the strings of one window */
#define WR_FIELDS	10		/* fields of one record */
#define WR_INMAX	63		/* longest input text (the keyboard) */

/* Window kinds */
#define WK_LIST		0
#define WK_TEXT		1
#define WK_CLOCK	2
#define WK_CONFIRM	3
#define WK_PAD		4		/* raw keys to the app, lines only */

/* Row kinds */
#define RK_ITEM		1		/* KEY LABEL [RIGHT] */
#define RK_INT		2		/* KEY LABEL MIN MAX STEP VALUE [UNIT] */
#define RK_ENUM		3		/* KEY LABEL INDEX OPT|OPT|... */
#define RK_BOOL		4		/* KEY LABEL 0|1 */
#define RK_TIME		5		/* KEY LABEL HH:MM */
#define RK_DATE		6		/* KEY LABEL YYYY-MM-DD */
#define RK_DAYS		7		/* KEY LABEL MASK (bit 0 Monday) */
#define RK_INPUT	8		/* KEY LABEL TEXT MAX */
#define RK_LINE		9		/* TEXT */
#define RK_ACTION	10		/* KEY LABEL */

/* What a record did (wr_record) */
#define WR_NONE		0		/* a row was added, or nothing to do */
#define WR_START	1		/* "win": a window begins (wr_start next) */
#define WR_SHOW		2		/* "show": the window is complete */
#define WR_BUSY		3
#define WR_IDLE		4
#define WR_CLOSE	5
#define WR_BAD		6		/* malformed: dropped */
#define WR_RUN		7		/* "run LABEL FILE": open FILE in the
					 * app of /etc/apps called LABEL, this
					 * window ends (issue #413) */

#define WS_EMPTY	0		/* struct wr_win st */
#define WS_BUILD	1		/* win seen, rows coming */
#define WS_SHOWN	2		/* show seen: complete */

#define RF_CHANGED	1		/* input: the text was edited */

/* One row: 8 bytes.  Its strings sit one after the other in the pool at
 * off: key, label, then the third string (item RIGHT, int UNIT, enum
 * OPTS, input TEXT; "line" has only its text, in the label place); an int
 * row's MIN MAX STEP follow as three shorts at the next even offset. */
struct wr_row {
	unsigned char kind;
	unsigned char flags;		/* RF_*; enum: number of options */
	unsigned short off;
	short val;			/* int, enum index, bool, minutes of
					 * the day, days mask, packed date,
					 * input MAX */
	short orig;			/* the value the app sent */
};

/* One window (about 640 B; the desktop keeps two). */
struct wr_win {
	unsigned short used;		/* pool bytes used */
	unsigned short bytes;		/* record bytes win..show */
	unsigned short id, title;	/* pool offsets */
	unsigned short total, first;	/* ROWS=N paging: all rows, this page */
	unsigned char kind, nrows;
	unsigned char sel, top;		/* the desktop's cursor */
	unsigned char bad : 1;		/* overflow: dropped at show */
	unsigned char live : 1;		/* LIVE: submit after every change */
	unsigned char st : 2;		/* WS_* (with bad, live: one byte) */
	unsigned char rep;		/* pad: REPEAT=ms / 10, 0: default */
	struct wr_row row[WR_ROWS];
	char pool[WR_POOL];
};

int wr_split(char *line, char **f, int max);
int wr_record(struct wr_win *w, char **f, int n, int len);
void wr_start(struct wr_win *w, char **f, int n, int len);
const char *wr_id(const struct wr_win *w);
const char *wr_title(const struct wr_win *w);
const char *wr_key(const struct wr_win *w, int i);
const char *wr_label(const struct wr_win *w, int i);
const char *wr_third(const struct wr_win *w, int i);
int wr_changed(const struct wr_win *w, int i);
int wr_selectable(const struct wr_win *w, int i);
int wr_value(const struct wr_win *w, int i, char *buf, int size, int wire);
int wr_change(struct wr_win *w, int i, int dir);
int wr_fields(int kind);
int wr_field(struct wr_win *w, int i, int field, int dir);
int wr_set_text(struct wr_win *w, int i, const char *s);
void wr_commit(struct wr_win *w);
int wr_submit(const struct wr_win *w, char *buf, int size, int *from);
int wr_esc(char *dst, int size, const char *s);
int wr_days_in(int y, int m);

#endif /* GUI_WINREC_H */
