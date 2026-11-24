/* wintest - the window records of spec K.8 (flipper/gui/winrec.c) and
 * libgm (src/lib/gm), on the PC: every row type, malformed and overlong
 * records, the 640 B limit, values changed as the desktop does, submit
 * lines, and a round trip libgm -> pipe -> parser, events -> libgm. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "../../gui/winrec.h"
#include "../../../src/lib/gm/gm.h"

static int fails, checks;

#define T(c, what) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); } } while (0)

/* Feed records (lines) to w as the launcher does; the last result. */
static int feed(struct wr_win *w, const char *text)
{
	char line[WR_LINE + 8], *f[WR_FIELDS];
	const char *p = text, *nl;
	int r = WR_NONE, n, len;

	while (*p != 0) {
		nl = strchr(p, '\n');
		len = nl != NULL ? (int) (nl - p) : (int) strlen(p);
		if (len >= WR_LINE) {		/* the launcher drops these */
			r = WR_BAD;
			if (w != NULL) w->bad = 1;
		} else {
			memcpy(line, p, (size_t) len);
			line[len] = 0;
			n = wr_split(line, f, WR_FIELDS);
			r = n < 0 ? WR_BAD : wr_record(w, f, n, len + 1);
			if (r == WR_START) wr_start(w, f, n, len + 1);
		}
		p += len + (nl != NULL);
	}
	return r;
}

static const char *val(struct wr_win *w, int i, int wire)
{
	static char b[80];

	wr_value(w, i, b, sizeof b, wire);
	return b;
}

static void every_row(void)
{
	static struct wr_win w;
	int r;

	r = feed(&w, "win\tmain\tlist\tAlarm\n"
		"item\tnew\tNew alarm\t3\n"
		"int\tvol\tVolume\t0\t100\t5\t40\t%\n"
		"enum\ttone\tTone\t1\tclassic|rise|pips\n"
		"bool\tvib\tVibro\t1\n"
		"time\tat\tTime\t07:30\n"
		"date\tday\tDate\t2026-11-17\n"
		"days\tdays\tDays\t31\n"
		"input\tname\tName\tWake up\t20\n"
		"line\tA static line\n"
		"action\tsave\tSave\n"
		"show\n");
	T(r == WR_SHOW && w.st == WS_SHOWN && !w.bad, "every row type: shown");
	T(w.nrows == 10 && w.kind == WK_LIST, "10 rows, list");
	T(strcmp(wr_id(&w), "main") == 0 && strcmp(wr_title(&w), "Alarm") == 0,
		"id and title");
	T(w.row[0].kind == RK_ITEM && strcmp(wr_key(&w, 0), "new") == 0
		&& strcmp(wr_label(&w, 0), "New alarm") == 0
		&& strcmp(val(&w, 0, 0), "3") == 0, "item with RIGHT");
	T(w.row[1].kind == RK_INT && w.row[1].val == 40
		&& strcmp(val(&w, 1, 0), "40 %") == 0
		&& strcmp(val(&w, 1, 1), "40") == 0, "int with unit");
	T(strcmp(val(&w, 2, 0), "rise") == 0 && strcmp(val(&w, 2, 1), "1") == 0
		&& w.row[2].flags == 3, "enum: option and index");
	T(strcmp(val(&w, 3, 0), "on") == 0 && strcmp(val(&w, 3, 1), "1") == 0,
		"bool");
	T(w.row[4].val == 7 * 60 + 30 && strcmp(val(&w, 4, 0), "07:30") == 0,
		"time");
	T(strcmp(val(&w, 5, 0), "2026-11-17") == 0, "date");
	T(strcmp(val(&w, 6, 0), "MTWTF--") == 0 && strcmp(val(&w, 6, 1), "31") == 0,
		"days: Monday to Friday");
	T(strcmp(val(&w, 7, 0), "Wake up") == 0 && w.row[7].val == 20, "input");
	T(w.row[8].kind == RK_LINE && !wr_selectable(&w, 8)
		&& strcmp(wr_label(&w, 8), "A static line") == 0, "line");
	T(w.row[9].kind == RK_ACTION && strcmp(wr_label(&w, 9), "Save") == 0,
		"action");
	T(w.bytes <= WR_BYTES && w.used <= WR_POOL, "sizes counted");

	/* Left/Right in the desktop */
	T(wr_change(&w, 1, 1) && w.row[1].val == 45, "int +step");
	w.row[1].val = 98;
	T(wr_change(&w, 1, 1) && w.row[1].val == 100, "int clamps at MAX");
	T(!wr_change(&w, 1, 1), "int at MAX: no change");
	T(wr_change(&w, 2, -1) && w.row[2].val == 0
		&& wr_change(&w, 2, -1) && w.row[2].val == 2, "enum wraps");
	T(wr_change(&w, 3, 1) && w.row[3].val == 0, "bool toggles");
	T(!wr_change(&w, 0, 1), "item: no value");
	/* fields */
	T(wr_fields(RK_TIME) == 2 && wr_fields(RK_DATE) == 3
		&& wr_fields(RK_DAYS) == 7 && wr_fields(RK_INT) == 0, "fields");
	T(wr_field(&w, 4, 0, -1) && strcmp(val(&w, 4, 0), "06:30") == 0, "hour -1");
	w.row[4].val = 23 * 60 + 59;
	T(wr_field(&w, 4, 1, 1) && strcmp(val(&w, 4, 0), "23:00") == 0,
		"minute wraps in its hour");
	T(wr_field(&w, 4, 0, 1) && strcmp(val(&w, 4, 0), "00:00") == 0,
		"hour wraps");
	feed(&w, "");
	{
		static struct wr_win d;

		feed(&d, "win\td\tlist\tD\ndate\tx\tX\t2024-01-31\nshow\n");
		T(wr_field(&d, 0, 1, 1) && strcmp(val(&d, 0, 0), "2024-02-29") == 0,
			"date: Jan 31 + 1 month = Feb 29 (leap)");
		T(wr_field(&d, 0, 0, 1) && strcmp(val(&d, 0, 0), "2025-02-28") == 0,
			"date: + 1 year = Feb 28");
		T(wr_field(&d, 0, 2, 1) && strcmp(val(&d, 0, 0), "2025-02-01") == 0,
			"date: day wraps in the month");
		feed(&d, "win\td\tlist\tD\ndate\tx\tX\t2000-01-15\nshow\n");
		T(wr_field(&d, 0, 0, -1) && strncmp(val(&d, 0, 0), "2127", 4) == 0,
			"date: year wraps 2000 -> 2127");
		T(wr_field(&d, 0, 0, 1) && strncmp(val(&d, 0, 0), "2000", 4) == 0,
			"date: and back");
	}
	T(wr_field(&w, 6, 6, 1) && strcmp(val(&w, 6, 0), "MTWTF-S") == 0,
		"days: Sunday on");
	/* input */
	T(wr_set_text(&w, 7, "Wake") == 0 && strcmp(val(&w, 7, 0), "Wake") == 0
		&& wr_changed(&w, 7), "input: shorter in place");
	T(wr_set_text(&w, 7, "Wake up now, please") == 0
		&& strcmp(val(&w, 7, 0), "Wake up now, please") == 0
		&& strcmp(wr_key(&w, 7), "name") == 0
		&& strcmp(wr_label(&w, 7), "Name") == 0, "input: longer moves");
	T(wr_set_text(&w, 7, "123456789012345678901") < 0, "input: over MAX");
	T(strcmp(wr_label(&w, 8), "A static line") == 0
		&& strcmp(wr_key(&w, 9), "save") == 0, "other rows intact");
	{
		char b[256];
		int from = 0, n;

		n = wr_submit(&w, b, sizeof b, &from);
		T(n > 0 && strcmp(b, "submit\tmain\tvol=100\ttone=2\tvib=0\t"
			"at=00:00\tdays=95\tname=Wake up now, please\n") == 0,
			"submit: the changed rows");
		if (n > 0 && strcmp(b, "submit\tmain\tvol=100\ttone=2\tvib=0\t"
			"at=00:00\tdays=95\tname=Wake up now, please\n") != 0)
			printf("  got %s", b);
		T(from == w.nrows && wr_submit(&w, b, sizeof b, &from) == 0,
			"submit: all sent");
		from = 0;
		n = wr_submit(&w, b, 40, &from);
		T(n > 0 && n < 40 && from < w.nrows, "submit: split when full");
		n = 0;
		while (from < w.nrows && wr_submit(&w, b, 40, &from) > 0) n++;
		T(n >= 1, "submit: the rest in more lines");
		wr_commit(&w);
		from = 0;
		T(wr_submit(&w, b, sizeof b, &from) == 0 && !wr_changed(&w, 7),
			"commit: nothing changed any more");
	}
}

static void kinds_paging(void)
{
	static struct wr_win w;

	T(feed(&w, "win\tc\tclock\tDesk\nline\tNext 07:30\nshow\n") == WR_SHOW
		&& w.kind == WK_CLOCK, "clock window");
	T(feed(&w, "win\tq\tconfirm\tDelete?\nline\tSure?\naction\tyes\tDelete\nshow\n")
		== WR_SHOW && w.kind == WK_CONFIRM, "confirm window");
	T(feed(&w, "win\tt\ttext\tAbout\nline\ta\nline\tb\nshow\n") == WR_SHOW
		&& w.kind == WK_TEXT && w.nrows == 2, "text window");
	T(feed(&w, "win\tl\tlist\tLong\tROWS=40\tFIRST=16\nitem\ta\tA\nshow\n")
		== WR_SHOW && w.total == 40 && w.first == 16, "ROWS= FIRST=");
	T(feed(&w, "win\tl\tlist\tLong\tROWS=40\tFIRST=40\nshow\n") == WR_BAD,
		"FIRST past ROWS: dropped");
	T(feed(&w, "win\tl\tgrid\tX\nshow\n") == WR_BAD, "unknown kind: dropped");
	T(feed(&w, "win\t\tlist\tX\nshow\n") == WR_BAD, "empty id: dropped");
	T(feed(&w, "win\tl\tlist\nshow\n") == WR_BAD, "no title: dropped");
	T(feed(&w, "win\tl\tlist\tX\tFOO=1\nshow\n") == WR_BAD, "unknown option");
	T(feed(&w, "win\ts\tlist\tDisplay\tLIVE\nint\ta\tB\t0\t9\t1\t5\nshow\n")
		== WR_SHOW && w.live, "LIVE (settings, K.7)");
	T(feed(&w, "win\tl\tlist\tLong\tROWS=40\tFIRST=16\tLIVE\nshow\n")
		== WR_SHOW && w.live && w.total == 40, "ROWS= FIRST= LIVE");
	T(feed(&w, "win\tl\tlist\tX\nshow\n") == WR_SHOW && !w.live,
		"not LIVE unless asked");
	/* pad (K.8, T28d): raw keys to the app, lines only, REPEAT=ms */
	T(feed(&w, "win\tm\tpad\tMouse\nline\tOK click\nshow\n") == WR_SHOW
		&& w.kind == WK_PAD && w.nrows == 1 && w.rep == 0, "pad window");
	T(feed(&w, "win\tm\tpad\tMouse\tREPEAT=10\nshow\n") == WR_SHOW
		&& w.rep == 1, "pad REPEAT=10");
	T(feed(&w, "win\tm\tpad\tMouse\tREPEAT=1000\nshow\n") == WR_SHOW
		&& w.rep == 100, "pad REPEAT=1000");
	T(feed(&w, "win\tm\tpad\tMouse\tREPEAT=5\nshow\n") == WR_BAD,
		"pad REPEAT below 10 ms: dropped");
	T(feed(&w, "win\tm\tpad\tMouse\tREPEAT=1001\nshow\n") == WR_BAD,
		"pad REPEAT above 1 s: dropped");
	T(feed(&w, "win\tl\tlist\tX\tREPEAT=100\nshow\n") == WR_BAD,
		"REPEAT only on a pad");
	T(feed(&w, "win\tm\tpad\tMouse\nitem\ta\tA\naction\tb\tB\n"
		"line\tx\nshow\n") == WR_SHOW && w.nrows == 1
		&& w.row[0].kind == RK_LINE, "pad: rows other than line dropped");
	T(feed(&w, "win\tm\tpad\tMouse\tREPEAT=20\tLIVE\nshow\n") == WR_SHOW
		&& w.rep == 2 && w.live, "pad with every option");
}

static void malformed(void)
{
	static struct wr_win w;
	static const char *bad[] = {
		"item\tk\n", "item\tk\tL\tR\tX\n", "item\t\tL\n",
		"int\tk\tL\t0\t10\t1\n", "int\tk\tL\t10\t0\t1\t5\n",
		"int\tk\tL\t0\t10\t0\t5\n", "int\tk\tL\t0\t10\t1\tx\n",
		"int\tk\tL\t0\t99999\t1\t5\n", "int\tk\tL\t0\t10\t1\t5\tu\tz\n",
		"enum\tk\tL\t3\ta|b|c\n", "enum\tk\tL\t-1\ta|b\n",
		"enum\tk\tL\t0\t\n", "bool\tk\tL\t2\n", "bool\tk\tL\n",
		"time\tk\tL\t24:00\n", "time\tk\tL\t7:30\n", "time\tk\tL\t07:60\n",
		"date\tk\tL\t2026-02-30\n", "date\tk\tL\t1999-01-01\n",
		"date\tk\tL\t2026-13-01\n", "date\tk\tL\t2026/01/01\n",
		"days\tk\tL\t128\n", "days\tk\tL\t-1\n",
		"input\tk\tL\ttoo long\t3\n", "input\tk\tL\tx\t64\n",
		"input\tk\tL\tx\t0\n", "line\n", "line\ta\tb\n",
		"action\tk\n", "frob\tk\n", NULL
	};
	int i, r;

	T(feed(NULL, "item\tk\tL\n") == WR_BAD, "a row with no window");
	T(feed(&w, "show\n") == WR_BAD, "show with no window");
	T(feed(&w, "busy\n") == WR_BUSY && feed(&w, "idle\n") == WR_IDLE
		&& feed(&w, "close\n") == WR_CLOSE, "busy, idle, close");
	T(feed(&w, "run\tSub-GHz\t/home/flipper/subghz/a.sub\n") == WR_RUN
		&& feed(&w, "run\tSub-GHz\tsubghz/a.sub\n") == WR_BAD
		&& feed(&w, "run\t\t/a.sub\n") == WR_BAD
		&& feed(&w, "run\tSub-GHz\n") == WR_BAD,
		"run LABEL FILE: an absolute path, a label (#413)");
	T(feed(&w, "busy\tx\n") == WR_BAD, "busy with a field");
	for (i = 0; bad[i] != NULL; i++) {
		char t[200];

		snprintf(t, sizeof t, "win\tm\tlist\tT\n%sitem\tok\tOK\nshow\n",
			bad[i]);
		feed(&w, "win\tm\tlist\tT\n");
		r = feed(&w, bad[i]);
		checks++;
		if (r != WR_BAD) {
			fails++;
			printf("FAIL malformed row accepted: %s", bad[i]);
		}
		r = feed(&w, t);
		checks++;
		if (r != WR_SHOW || w.nrows != 1) {
			fails++;
			printf("FAIL malformed row breaks its window: %s", bad[i]);
		}
	}
	/* the escapes */
	T(feed(&w, "win\tm\tlist\tA\\tB\nline\tx\\\\y\\nz\nshow\n") == WR_SHOW
		&& strcmp(wr_title(&w), "A\tB") == 0
		&& strcmp(wr_label(&w, 0), "x\\y\nz") == 0, "escapes");
	{
		char t[32], *f[WR_FIELDS];

		strcpy(t, "a\tb\tc\r");
		T(wr_split(t, f, WR_FIELDS) == 3 && strcmp(f[2], "c") == 0,
			"CR LF line end");
		strcpy(t, "a\tb\tc\td");
		T(wr_split(t, f, 3) < 0, "too many fields");
	}
	{
		char e[8];

		T(wr_esc(e, sizeof e, "a\tb") == 4 && strcmp(e, "a\\tb") == 0,
			"wr_esc");
		T(wr_esc(e, 4, "a\tb") < 0, "wr_esc: does not fit");
	}
}

static void limits(void)
{
	static struct wr_win w;
	char t[2048], *p;
	int i, r;

	/* 17 rows */
	p = t + sprintf(t, "win\tm\tlist\tT\n");
	for (i = 0; i < 17; i++) p += sprintf(p, "item\tk%d\tL\n", i);
	strcpy(p, "show\n");
	T(feed(&w, t) == WR_BAD && w.st != WS_SHOWN, "17 rows: the window is dropped");
	/* 16 rows: fine */
	p = t + sprintf(t, "win\tm\tlist\tT\n");
	for (i = 0; i < 16; i++) p += sprintf(p, "item\tk%d\tL\n", i);
	strcpy(p, "show\n");
	T(feed(&w, t) == WR_SHOW && w.nrows == 16, "16 rows");
	/* over 640 B of records */
	p = t + sprintf(t, "win\tm\tlist\tT\n");
	for (i = 0; i < 12; i++)
		p += sprintf(p, "line\t%050d\n", i);	/* 56 B each: 672 */
	strcpy(p, "show\n");
	T(feed(&w, t) == WR_BAD && w.st != WS_SHOWN, "over 640 B: dropped");
	/* exactly 640 B */
	p = t + sprintf(t, "win\tm\tlist\tABCDEFGHIJKLMNO\n");	/* 27 */
	for (i = 0; i < 16; i++)		/* 38 each: 608 */
		p += sprintf(p, "int\tk%02d\tLabel\t-10000\t10000\t1000\t-5000\n", i);
	strcpy(p, "show\n");			/* 5: 640 */
	r = feed(&w, t);
	T(r == WR_SHOW && w.bytes == 640 && w.used <= WR_POOL, "640 B: shown");
	T(wr_change(&w, 15, -1) && w.row[15].val == -6000
		&& wr_change(&w, 15, 1) && wr_change(&w, 15, 1)
		&& w.row[15].val == -4000, "int limits found after its strings");
	memmove(t + 12, t + 11, strlen(t + 11) + 1);	/* the title + 1: 641 */
	T(feed(&w, t) == WR_BAD && w.st != WS_SHOWN, "641 B: dropped");
	/* under 640 B, but more than WR_POOL of strings */
	p = t + sprintf(t, "win\tm\ttext\tT\n");
	for (i = 0; i < 11; i++)
		p += sprintf(p, "line\t%050d\n", i);	/* 616 B, strings 583 */
	strcpy(p, "show\n");
	T(feed(&w, t) == WR_BAD && w.st != WS_SHOWN,
		"more than WR_POOL bytes of strings: dropped");
	/* an overlong line */
	p = t + sprintf(t, "win\tm\tlist\tT\nline\t");
	memset(p, 'x', 200);
	strcpy(p + 200, "\nshow\n");
	T(feed(&w, t) == WR_BAD, "an overlong record drops the window");
	/* a second window replaces the first in the same buffer */
	T(feed(&w, "win\ta\tlist\tA\nitem\tx\tX\nshow\n") == WR_SHOW
		&& feed(&w, "win\tb\ttext\tB\nshow\n") == WR_SHOW
		&& w.nrows == 0 && strcmp(wr_id(&w), "b") == 0, "rebuilt");
	/* rows after show belong to no window */
	T(feed(&w, "item\tx\tX\n") == WR_BAD, "a row after show");
	/* input relocation when the pool is full */
	p = t + sprintf(t, "win\tm\tlist\tT\n");
	for (i = 0; i < 8; i++)
		p += sprintf(p, "input\tk%d\t%030d\t%020d\t63\n", i, i, i);
	strcpy(p, "show\n");
	T(feed(&w, t) == WR_SHOW, "8 inputs");
	{
		char big[64];

		memset(big, 'y', 63);
		big[63] = 0;
		for (i = 0; i < 8 && wr_set_text(&w, i, big) == 0; i++) ;
		T(i < 8, "input: no room in the pool -> -1, not an overflow");
		T(w.used <= WR_POOL, "pool within its size");
	}
}

/* libgm -> pipe -> parser, and events -> libgm */
static void roundtrip(void)
{
	static struct wr_win w;
	char buf[128], ev[64], got[2048], *f[GM_FIELDS];
	int pe[2], pr[2], n, len = 0;

	/* 3 and 4 taken first, so that the pipes are not on them */
	dup2(open("/dev/null", O_RDONLY), GM_EVFD);
	dup2(open("/dev/null", O_RDONLY), GM_RECFD);
	if (pipe(pe) < 0 || pipe(pr) < 0) { T(0, "pipes"); return; }
	dup2(pe[0], GM_EVFD);
	dup2(pr[1], GM_RECFD);
	T(gm_init(buf, sizeof buf) == 0, "gm_init");
	gm_win("main", "list", "Demo", 0, 0);
	gm_item("about", "About", "");
	gm_item("n", "Notes", "3");
	gm_int("vol", "Volume", 0, 100, 5, 40, "%");
	gm_enum("tone", "Tone", 2, "a|b|c");
	gm_bool("vib", "Vibro", 1);
	gm_time("at", "Time", 7, 5);
	gm_date("day", "Date", 2026, 1, 9);
	gm_days("d", "Days", 0x7F);
	gm_input("name", "Name", "a\tb\\c", 20);
	gm_line("Line\nwith newline");
	gm_action("save", "Save");
	T(gm_show() == 0, "gm_show");
	dup2(open("/dev/null", O_RDONLY), GM_RECFD);	/* the pipe's last writer */
	close(pr[1]);
	while ((n = (int) read(pr[0], got + len, sizeof got - 1 - len)) > 0)
		len += n;
	got[len] = 0;
	T(feed(&w, got) == WR_SHOW && w.nrows == 11, "libgm window parsed");
	T(strcmp(val(&w, 0, 0), "") == 0 && strcmp(val(&w, 1, 0), "3") == 0,
		"item");
	T(strcmp(val(&w, 2, 0), "40 %") == 0, "int");
	T(strcmp(val(&w, 3, 0), "c") == 0, "enum");
	T(strcmp(val(&w, 5, 0), "07:05") == 0, "time");
	T(strcmp(val(&w, 6, 0), "2026-01-09") == 0, "date");
	T(strcmp(val(&w, 7, 0), "MTWTFSS") == 0, "days");
	T(strcmp(val(&w, 8, 0), "a\tb\\c") == 0, "input escapes");
	T(strcmp(wr_label(&w, 9), "Line\nwith newline") == 0, "line escapes");
	/* a long record never fits a small buffer: dropped, the rest go */
	{
		char small[24];
		int p2[2];

		if (pipe(p2) == 0) {
			dup2(p2[1], GM_RECFD);
			gm_init(small, sizeof small);
			gm_win("w", "list", "T", 0, 0);
			gm_line("this line is far too long for 24 bytes");
			gm_line("ok");
			gm_show();
			close(GM_RECFD);
			close(p2[1]);
			len = 0;
			while ((n = (int) read(p2[0], got + len,
					sizeof got - 1 - len)) > 0) len += n;
			got[len] = 0;
			if (strcmp(got, "win\tw\tlist\tT\nline\tok\nshow\n") != 0)
				printf("  got [%s]\n", got);
			T(strcmp(got, "win\tw\tlist\tT\nline\tok\nshow\n") == 0,
				"libgm: a record over the buffer is dropped");
			close(p2[0]);
		}
	}
	/* a pad window through libgm */
	{
		char pb[128];
		int p3[2];

		dup2(open("/dev/null", O_RDONLY), GM_RECFD);	/* not on 4 */
		if (pipe(p3) == 0) {
			dup2(p3[1], GM_RECFD);
			gm_init(pb, sizeof pb);
			gm_win_pad("mouse", "Mouse", 10);
			gm_line("OK click");
			gm_show();
			close(GM_RECFD);
			close(p3[1]);
			len = 0;
			while ((n = (int) read(p3[0], got + len,
					sizeof got - 1 - len)) > 0) len += n;
			got[len] = 0;
			T(strcmp(got, "win\tmouse\tpad\tMouse\tREPEAT=10\n"
				"line\tOK click\nshow\n") == 0
				&& feed(&w, got) == WR_SHOW && w.kind == WK_PAD
				&& w.rep == 1, "libgm: gm_win_pad");
			close(p3[0]);
		}
	}
	/* events */
	{
		const char *e = "ok\tmain\tabout\nsubmit\tmain\tvol=45\tname=x\\ty\n"
				"page\tlong\t16\n";
		char lng[100];

		T(write(pe[1], e, strlen(e)) > 0, "write");
		T(gm_event(ev, sizeof ev, f, GM_FIELDS) == GM_OK
			&& strcmp(f[1], "main") == 0 && strcmp(f[2], "about") == 0,
			"event ok");
		T(gm_event(ev, sizeof ev, f, GM_FIELDS) == GM_SUBMIT, "event submit");
		{
			char *v, *k = gm_kv(f[3], &v);

			T(strcmp(k, "name") == 0 && strcmp(v, "x\ty") == 0, "gm_kv");
		}
		T(gm_event(ev, sizeof ev, f, GM_FIELDS) == GM_PAGE
			&& strcmp(f[2], "16") == 0, "event page");
		memset(lng, 'z', sizeof lng);
		T(write(pe[1], lng, sizeof lng) > 0, "write");
		T(write(pe[1], "\nback\tmain\nredraw\nfoo\n", 22) > 0, "write");
		T(gm_event(ev, sizeof ev, f, GM_FIELDS) == GM_BACK,
			"an overlong event is dropped");
		T(gm_event(ev, sizeof ev, f, GM_FIELDS) == GM_REDRAW, "redraw");
		T(gm_event(ev, sizeof ev, f, GM_FIELDS) == GM_NONE, "unknown");
		T(gm_wait(ev, 0) == 0, "gm_wait: nothing there, time up");
		T(write(pe[1], "key\tup\tpress\nkey\tup\trepeat\n", 27) == 27,
			"write");
		T(gm_wait(ev, 0) == 1, "gm_wait: an event in the pipe");
		T(gm_event(ev, sizeof ev, f, GM_FIELDS) == GM_KEY
			&& strcmp(f[1], "up") == 0 && strcmp(f[2], "press") == 0,
			"event key (pad)");
		T(gm_wait(ev, 0) == 1, "gm_wait: an event already read");
		T(gm_event(ev, sizeof ev, f, GM_FIELDS) == GM_KEY
			&& strcmp(f[2], "repeat") == 0, "event key repeat");
		T(gm_wait(ev, 0) == 0, "gm_wait: none left");
		close(pe[1]);
		T(gm_event(ev, sizeof ev, f, GM_FIELDS) == GM_CLOSE,
			"the desktop gone: GM_CLOSE");
	}
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	every_row();
	kinds_paging();
	malformed();
	limits();
	roundtrip();
	printf("wintest: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
