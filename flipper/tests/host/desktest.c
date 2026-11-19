/* desktest - the desktop's module records (spec K.3, K.6, T23a;
 * flipper/gui/desktop.c) on the PC: every verb, malformed lines, overlong
 * fields, escapes, ownership (uid + name), a full table, exited modules,
 * and dk_record -> dk_parse round trips. */
#include <stdio.h>
#include <string.h>
#include "../../gui/desktop.h"

static int fails, checks;

#define T(c, what) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); } } while (0)

/* Parse and apply one line as the launcher does: DC_* bits, or -1 for a
 * malformed record. */
static int feed(struct desk *d, const char *text)
{
	char line[DK_LINE + 8];
	struct dk_rec r;
	size_t n = strlen(text);

	if (n > DK_LINE) return -1;	/* the launcher drops these */
	memcpy(line, text, n + 1);
	if (n > 0 && line[n - 1] == '\n') line[n - 1] = 0;
	if (dk_parse(line, &r) < 0) return -1;
	return dk_apply(d, &r);
}

static struct dk_note note_buf[DK_NNOTES];

static void init(struct desk *d)
{
	memset(d, 0, sizeof *d);
	memset(note_buf, 0x55, sizeof note_buf);
	d->n = note_buf;
	d->st = -1;
}

static void malformed(void)
{
	static const char *bad[] = {
		"",
		"tile",
		"tile\tbat\t100\t7",			/* no text */
		"tile\tbat\t100\t7\tx\ty",		/* one field too many */
		"close\tbat\t100\t7\tx",
		"window\tbat\t100\t7\tx",		/* not a desktop verb */
		"TILE\tbat\t100\t7\tx",
		"tile\t\t100\t7\tx",			/* empty name */
		"tile\tabcdefghijk\t100\t7\tx",		/* name of 11 */
		"tile\tbat\t-1\t7\tx",
		"tile\tbat\t65536\t7\tx",
		"tile\tbat\t1x\t7\tx",
		"tile\tbat\t\t7\tx",
		"tile\tbat\t100\t0\tx",			/* pid 0 */
		"tile\tbat\t100\t32768\tx",
		"tile\tbat\t100\t99999999999999999999\tx",
		"notify\tm\t1\t2",			/* no text */
		"notify\tm\t1\t2\ta\tb\tc",		/* 7 fields */
		"tile\tbat\t100\t7\tx\tbody",		/* a body: notify only */
		(const char *) 0
	};
	struct desk d;
	int i;

	init(&d);
	for (i = 0; bad[i] != NULL; i++) {
		char what[80];

		snprintf(what, sizeof what, "malformed %d dropped", i);
		T(feed(&d, bad[i]) == -1, what);
	}
	T(dk_tiles(&d) == 0 && dk_unread(&d) == 0 && d.notes == 0, "malformed: nothing stored");
	T(feed(&d, "tile\tabcdefghij\t65535\t32767\tok\n") == DC_TILES,
		"the longest name, uid and pid");
}

static void verbs(void)
{
	struct desk d;
	const struct dk_tile *t;

	init(&d);
	T(feed(&d, "tile\tbat\t100\t7\tBattery 80%\n") == DC_TILES, "tile: new");
	T(dk_tiles(&d) == 1, "tile: one");
	t = dk_tile(&d, 0);
	T(t != NULL && strcmp(t->text, "Battery 80%") == 0 && t->pid == 7
		&& t->uid == 100, "tile: stored");
	T(feed(&d, "tile\tbat\t100\t7\tBattery 80%") == 0,
		"tile: the same text again changes nothing (no redraw)");
	T(feed(&d, "tile\tbat\t100\t8\tBattery 79%") == DC_TILES,
		"tile: changed");
	t = dk_tile(&d, 0);
	T(dk_tiles(&d) == 1 && strcmp(t->text, "Battery 79%") == 0
		&& t->pid == 8, "tile: replaced in place, newest pid");
	T(feed(&d, "tile\tempty\t100\t7\t") == DC_TILES, "tile: an empty text is new");
	T(feed(&d, "status\tbat\t100\t7\tREC") == DC_STATUS, "status: new");
	T(d.st >= 0 && strcmp(d.t[(int) d.st].text, "REC") == 0, "status: shown");
	T(dk_tiles(&d) == 2, "status: not a tile");
	T(feed(&d, "status\tbat\t100\t7\tREC") == 0, "status: same, no change");
	T(feed(&d, "notify\tmail\t100\t7\tNew mail") == (DC_NOTE | DC_STATUS),
		"notify: counted");
	T(dk_unread(&d) == 1 && d.notes == 1
		&& strcmp(d.n[0].title, "New mail") == 0
		&& strcmp(d.n[0].name, "mail") == 0 && d.n[0].uid == 100
		&& d.n[0].body[0] == 0, "notify: count and text");
	T(feed(&d, "close\tbat\t100\t7") == (DC_TILES | DC_STATUS),
		"close: the tile and the status go");
	T(dk_tiles(&d) == 1 && d.st == -1, "close: only the other tile left");
	T(feed(&d, "close\tbat\t100\t7") == 0, "close: of an unknown module: nothing");
	{
		int i;

		for (i = 0; i < 120; i++) feed(&d, "notify\tm\t1\t2\tx");
		T(dk_unread(&d) == DK_NNOTES && d.notes == DK_NNOTES,
			"notify: only the last 8 are kept");
	}
}

static void owners(void)
{
	struct desk d;

	init(&d);
	feed(&d, "tile\tbat\t100\t7\tmine");
	T(feed(&d, "tile\tbat\t200\t9\ttheirs") == DC_TILES,
		"same name, other uid: its own tile");
	T(dk_tiles(&d) == 2 && strcmp(dk_tile(&d, 0)->text, "mine") == 0,
		"another uid cannot change my tile");
	T(feed(&d, "close\tbat\t200\t9") == DC_TILES && dk_tiles(&d) == 1
		&& strcmp(dk_tile(&d, 0)->text, "mine") == 0,
		"another uid cannot close my tile");
	feed(&d, "status\tbat\t100\t7\tA");
	feed(&d, "status\tclk\t100\t7\tB");
	T(strcmp(d.t[(int) d.st].text, "B") == 0, "status: the newest is shown");
	feed(&d, "close\tclk\t100\t7");
	T(d.st >= 0 && strcmp(d.t[(int) d.st].text, "A") == 0,
		"status: the other comes back when the newest closes");
}

static void escapes(void)
{
	struct desk d;
	const struct dk_tile *t;
	char b[DK_LINE + 1];
	int n;

	init(&d);
	feed(&d, "tile\tx\t1\t2\ta\\tb\\nc\\\\d\\q\r");
	t = dk_tile(&d, 0);
	T(t != NULL && strcmp(t->text, "a b c\\dq") == 0,
		"escapes: \\t \\n as spaces, \\\\, an unknown one, CR LF");
	feed(&d, "tile\ty\t1\t2\tbell\007here");
	T(strcmp(dk_tile(&d, 1)->text, "bell here") == 0,
		"control characters become spaces");
	feed(&d, "tile\tz\t1\t2\t0123456789012345678901234567890123456789");
	T(strlen(dk_tile(&d, 2)->text) == DK_TEXT, "an overlong text is cut to 31");
	/* dk_record -> dk_parse */
	n = dk_record(b, DK_LINE, "tile", "bat", 1000, 42, "a\tb\\c\nd");
	if (n > 0) b[n] = 0;
	T(n > 0 && b[n - 1] == '\n' && memcmp(b, "tile\tbat\t1000\t42\ta\\tb\\\\c\\nd\n",
		(size_t) n) == 0, "dk_record: fields and escapes");
	T(feed(&d, b) == DC_TILES && strcmp(dk_tile(&d, 3)->text, "a b\\c d") == 0,
		"dk_record -> dk_parse round trip");
	n = dk_record(b, DK_LINE, "close", "bat", 0, 1, NULL);
	T(n == 14 && memcmp(b, "close\tbat\t0\t1\n", 14) == 0, "dk_record: close");
	{
		char big[200];

		memset(big, 'x', sizeof big - 1);
		big[sizeof big - 1] = 0;
		T(dk_record(b, DK_LINE, "tile", "bat", 1, 2, big) == -1,
			"dk_record: a record over 128 B is refused");
		big[DK_LINE - 14] = 0;	/* "tile\tbat\t1\t2\t" is 13 */
		n = dk_record(b, DK_LINE, "tile", "bat", 1, 2, big);
		if (n > 0) b[n] = 0;
		T(n == DK_LINE, "dk_record: 128 B with the newline fits");
		T(feed(&d, b) == DC_TILES, "a record of 128 B is taken");
		big[DK_LINE - 14] = 'x';
		big[DK_LINE - 13] = 0;
		T(dk_record(b, DK_LINE, "tile", "bat", 1, 2, big) == -1,
			"dk_record: 129 B with the newline does not");
	}
}

static int dead;

static int alive(int pid)
{
	return pid != dead;
}

static void full_and_purge(void)
{
	struct desk d;
	char l[64];
	int i;

	init(&d);
	for (i = 0; i < DK_NTILES; i++) {
		snprintf(l, sizeof l, "tile\tm%d\t1\t%d\tt", i, 100 + i);
		T(feed(&d, l) == DC_TILES, "fill the table");
	}
	T(feed(&d, "tile\tmore\t1\t5\tt") == DC_FULL, "a full table: dropped");
	T(feed(&d, "status\tmore\t1\t5\tt") == DC_FULL, "a full table: status dropped");
	T(feed(&d, "tile\tm3\t1\t103\tnew") == DC_TILES,
		"a full table: a module's own tile still changes");
	T(sizeof(struct dk_tile) == 48, "one entry is 48 B");
	dead = 105;
	T(dk_purge(&d, alive) == DC_TILES && dk_tiles(&d) == DK_NTILES - 1,
		"an exited module's tile goes");
	T(dk_purge(&d, alive) == 0, "purge again: nothing");
	T(feed(&d, "tile\tmore\t1\t5\tt") == DC_TILES, "its place is free again");
	dead = 106;
	dk_purge(&d, alive);
	T(feed(&d, "status\tst\t1\t5\tR") == DC_STATUS, "a status in the freed place");
	dead = 5;
	T(dk_purge(&d, alive) == (DC_TILES | DC_STATUS) && d.st == -1,
		"an exited module's status goes too");
}

/* T23c: the Notifications page's list: the last 8, newest first, a body,
 * unread count, clear. */
static void notes(void)
{
	struct desk d;
	char line[DK_LINE];
	int i;

	init(&d);
	for (i = 0; i < 10; i++) {
		snprintf(line, sizeof line, "notify\treminder\t21\t5\tNote %d"
			 "\tBody %d", i, i);
		T(feed(&d, line) == (DC_NOTE | DC_STATUS), "notes: posted");
	}
	T(d.notes == 8 && dk_unread(&d) == 8, "notes: 10 posted, 8 kept, 8 unread");
	T(strcmp(d.n[0].title, "Note 9") == 0 && strcmp(d.n[7].title, "Note 2") == 0
		&& strcmp(d.n[0].body, "Body 9") == 0,
		"notes: the newest first, the two oldest gone");
	d.n[3].unread = 0;			/* opened */
	T(dk_unread(&d) == 7, "notes: an opened one is not unread");
	dk_clear(&d, 3);
	T(d.notes == 7 && dk_unread(&d) == 7 && strcmp(d.n[3].title, "Note 5") == 0
		&& strcmp(d.n[6].title, "Note 2") == 0, "notes: clear: the rest move up");
	dk_clear(&d, 6);
	dk_clear(&d, 7);			/* past the end: nothing */
	dk_clear(&d, -1);
	T(d.notes == 6 && strcmp(d.n[5].title, "Note 3") == 0, "notes: clear the oldest");
	while (d.notes > 0) dk_clear(&d, 0);
	T(dk_unread(&d) == 0, "notes: all cleared");
	/* long fields are cut; a long TEXT without BODY is the body too */
	feed(&d, "notify\tm\t1\t2\t0123456789012345678901234567890123456789");
	T(strlen(d.n[0].title) == DK_TITLE
		&& strcmp(d.n[0].body, "0123456789012345678901234567890123456789") == 0,
		"notes: a long text: the row cut, the whole text as the body");
	feed(&d, "notify\tm\t1\t2\tt\t0123456789012345678901234567890123456789"
		"0123456789");
	T(strlen(d.n[0].body) == DK_BODY, "notes: the body cut at 47");
	feed(&d, "notify\tm\t1\t2\ta\\tb\tc\\nd");
	T(strcmp(d.n[0].title, "a b") == 0 && strcmp(d.n[0].body, "c d") == 0,
		"notes: escapes undone");
	T(sizeof(struct dk_note) == 88, "notes: 88 B an entry");
	d.n = NULL;
	T(feed(&d, "notify\tm\t1\t2\tx") == 0, "notes: no table: dropped");
}

int main(void)
{
	malformed();
	verbs();
	owners();
	escapes();
	notes();
	full_and_purge();
	printf("desktest: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
