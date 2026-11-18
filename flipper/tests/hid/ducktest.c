/* ducktest - the Bad USB script engine (spec R11.3.3, flipper/apps/badusb
 * ducky.c) on the PC, under AddressSanitizer and UBSan.
 *
 * - golden reports: STRING/STRINGLN in US and DE, key combinations,
 *   HOLD/RELEASE, ALTCHAR/ALTSTRING, SYSRQ, MEDIA, GLOBE, the mouse,
 *   DELAY/DEFAULT_DELAY/STRING_DELAY waits, REPEAT (re-read from the file),
 *   CRLF, WAIT_FOR_BUTTON_PRESS;
 * - check mode (badusb -n): the first bad line with its number and the
 *   word it names; long lines, NULs, bounds;
 * - a stop from the caller (a signal in badusb) ends the script at once;
 * - fuzz (spec 11.3 Security): random scripts made of the commands, key
 *   names, numbers and random bytes; check and run agree, every report is
 *   well formed (at most 6 keys, mouse values in -127..127), a script
 *   that ends lets every key go (the last report all zeros), nothing
 *   reads or writes out of bounds (the sanitizers).
 *
 * Usage: ducktest LAYOUTDIR [ROUNDS].  Exit status: failures.
 *	  ducktest -r LAYOUT.kl|us SCRIPT: the reports SCRIPT makes, one
 *	  per line as QEMU prints them without "hid< " (tools/nattest.py
 *	  compares badusb in QEMU with them; waits are left out).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "libhid.h"
#include "ducky.h"

static int fails;

static void check(int ok, const char *what)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) fails++;
}

/* ---- a recording dk_io ---- */

static char rec[1 << 16];		/* what the script did, as text */
static size_t recn;
static int stop_after = -1;		/* reports until a stop; -1 never */
static int nrep, badrep, buttons;
static unsigned char lastkbd[8], lastmouse[4];
static int sawkbd, sawmouse;

static void put(const char *s)
{
	size_t n = strlen(s);

	if (recn + n + 1 < sizeof rec) {
		memcpy(rec + recn, s, n + 1);
		recn += n;
	}
}

static int report(const char *what, const unsigned char *r, int n)
{
	char b[64];
	int i, o;

	o = snprintf(b, sizeof b, "%s", what);
	for (i = 0; i < n; i++) o += snprintf(b + o, sizeof b - o, " %02x", r[i]);
	snprintf(b + o, sizeof b - o, "\n");
	put(b);
	nrep++;
	if (stop_after >= 0 && nrep >= stop_after) return -1;
	return 0;
}

static int t_kbd(void *c, const unsigned char r[8])
{
	int i, n = 0;

	(void) c;
	if (r[1] != 0) badrep++;
	for (i = 2; i < 8; i++) {
		if (r[i] == 0) n = 1;
		else if (n) badrep++;	/* a key after a gap */
	}
	memcpy(lastkbd, r, 8);
	sawkbd = 1;
	return report("kbd", r, 8);
}

static int t_mouse(void *c, const unsigned char r[4])
{
	int i;

	(void) c;
	for (i = 1; i < 4; i++)
		if (r[i] == 0x80) badrep++;	/* -128: out of range */
	memcpy(lastmouse, r, 4);
	sawmouse = 1;
	return report("mouse", r, 4);
}

static int t_cons(void *c, const unsigned char r[2])
{
	(void) c;
	return report("cons", r, 2);
}

static int t_wait(void *c, long ms)
{
	char b[32];

	(void) c;
	if (ms > 0) {
		snprintf(b, sizeof b, "wait %ld\n", ms);
		put(b);
	}
	if (stop_after >= 0 && nrep >= stop_after) return -1;
	return 0;
}

static int t_button(void *c)
{
	(void) c;
	buttons++;
	put("button\n");
	return 0;
}

static const struct dk_io io = { t_kbd, t_mouse, t_cons, t_wait, t_button,
				 NULL, NULL };

/* ---- scripts ---- */

static int script_fd(const char *s, size_t n)
{
	FILE *f = tmpfile();
	int fd;

	if (f == NULL) {
		perror("tmpfile");
		exit(99);
	}
	fd = dup(fileno(f));
	fclose(f);
	if (write(fd, s, n) != (ssize_t) n) exit(99);
	lseek(fd, 0, SEEK_SET);
	return fd;
}

static void reset(void)
{
	recn = 0;
	rec[0] = 0;
	nrep = badrep = buttons = 0;
	sawkbd = sawmouse = 0;
	memset(lastkbd, 0, sizeof lastkbd);
	memset(lastmouse, 0, sizeof lastmouse);
}

/* Run (or check, with run 0) script s; the result, *d after it. */
static int go(struct dk *d, const unsigned char *kl, const char *s,
	      size_t n, int run)
{
	int fd = script_fd(s, n), r;

	reset();
	memset(d, 0, sizeof *d);
	d->kl = kl;
	d->io = run ? &io : NULL;
	r = dk_run(d, fd);
	close(fd);
	return r;
}

static void golden(const unsigned char *kl, const char *name,
		   const char *script, const char *want)
{
	struct dk d;
	char what[160];
	int r = go(&d, kl, script, strlen(script), 1);

	snprintf(what, sizeof what, "%s: %s", name, script);
	for (char *p = what; *p; p++) if (*p == '\n') *p = '|';
	check(r == DK_DONE && strcmp(rec, want) == 0 && badrep == 0, what);
	if (r != DK_DONE || strcmp(rec, want) != 0) {
		printf("     result %d (%s %s, line %d)\n     got:\n%s     want:\n%s",
		       r, d.err ? d.err : "", d.errarg, d.line, rec, want);
	}
}

static void bad(const unsigned char *kl, const char *script, int line,
		const char *err, const char *arg)
{
	struct dk d;
	char what[200];
	int r = go(&d, kl, script, strlen(script), 0);

	snprintf(what, sizeof what, "-n: line %d \"%s\" \"%s\": ", line, err, arg);
	strncat(what, script, 60);
	for (char *p = what; *p; p++) if (*p == '\n') *p = '|';
	check(r == DK_ERROR && d.line == line && strcmp(d.err, err) == 0
	      && strcmp(d.errarg, arg) == 0, what);
	if (r != DK_ERROR || d.line != line || strcmp(d.err, err) != 0
	    || strcmp(d.errarg, arg) != 0)
		printf("     got %d line %d \"%s\" \"%s\"\n", r, d.line,
		       d.err ? d.err : "", d.errarg);
}

#define UP	"kbd 00 00 00 00 00 00 00 00\n"

static void goldens(const unsigned char *us, const unsigned char *de)
{
	golden(us, "US", "STRING Hi\n",
	       "kbd 02 00 0b 00 00 00 00 00\n" UP
	       "kbd 00 00 0c 00 00 00 00 00\n" UP);
	golden(de, "DE", "STRING zy@\n",
	       "kbd 00 00 1c 00 00 00 00 00\n" UP
	       "kbd 00 00 1d 00 00 00 00 00\n" UP
	       "kbd 40 00 14 00 00 00 00 00\n" UP);
	golden(us, "US", "STRINGLN a\n",
	       "kbd 00 00 04 00 00 00 00 00\n" UP
	       "kbd 00 00 28 00 00 00 00 00\n" UP);
	golden(us, "US, CRLF, blank lines, REM", "REM hi\r\n\r\n  ENTER\r\n",
	       "kbd 00 00 28 00 00 00 00 00\n" UP);
	golden(us, "US", "GUI r\n",
	       "kbd 08 00 15 00 00 00 00 00\n" UP);
	golden(us, "US, a character after a modifier: its key alone",
	       "CTRL SHIFT R\n",
	       "kbd 03 00 15 00 00 00 00 00\n" UP);
	golden(us, "US", "CTRL-ALT DELETE\n",
	       "kbd 05 00 4c 00 00 00 00 00\n" UP);
	golden(us, "US (DEFAULT_DELAY waits after itself too, as stock)",
	       "DELAY 500\nDEFAULT_DELAY 20\nTAB\nREPEAT 2\n",
	       "wait 500\n" "wait 20\n"
	       "kbd 00 00 2b 00 00 00 00 00\n" UP "wait 20\n"
	       "kbd 00 00 2b 00 00 00 00 00\n" UP "wait 20\n"
	       "kbd 00 00 2b 00 00 00 00 00\n" UP "wait 20\n");
	golden(us, "US", "STRING_DELAY 7\nSTRING ab\nSTRING c\n",
	       "kbd 00 00 04 00 00 00 00 00\n" UP "wait 7\n"
	       "kbd 00 00 05 00 00 00 00 00\n" UP
	       "kbd 00 00 06 00 00 00 00 00\n" UP);
	golden(us, "US", "HOLD SHIFT\nHOLD a\nRELEASE a\nRELEASE SHIFT\n",
	       "kbd 02 00 00 00 00 00 00 00\n"
	       "kbd 02 00 04 00 00 00 00 00\n"
	       "kbd 02 00 00 00 00 00 00 00\n" UP);
	golden(us, "US, HOLD released at the end", "HOLD CTRL\n",
	       "kbd 01 00 00 00 00 00 00 00\n" UP);
	golden(us, "US", "ALTCHAR 65\n",
	       "kbd 04 00 00 00 00 00 00 00\n"
	       "kbd 04 00 5e 00 00 00 00 00\n" "kbd 04 00 00 00 00 00 00 00\n"
	       "kbd 04 00 5d 00 00 00 00 00\n" "kbd 04 00 00 00 00 00 00 00\n"
	       UP);
	golden(us, "US, ALTSTRING (ñ: 0241)", "ALTSTRING \xc3\xb1\n",
	       "kbd 04 00 00 00 00 00 00 00\n"
	       "kbd 04 00 62 00 00 00 00 00\n" "kbd 04 00 00 00 00 00 00 00\n"
	       "kbd 04 00 5a 00 00 00 00 00\n" "kbd 04 00 00 00 00 00 00 00\n"
	       "kbd 04 00 5c 00 00 00 00 00\n" "kbd 04 00 00 00 00 00 00 00\n"
	       "kbd 04 00 59 00 00 00 00 00\n" "kbd 04 00 00 00 00 00 00 00\n"
	       UP);
	golden(us, "US", "SYSRQ b\n",
	       "kbd 04 00 46 05 00 00 00 00\n" UP);
	golden(us, "US", "MEDIA VOLUME_UP\n", "cons e9 00\ncons 00 00\n");
	golden(us, "US", "GLOBE e\n",
	       "cons 9d 02\nkbd 00 00 08 00 00 00 00 00\n" UP "cons 00 00\n");
	golden(us, "US", "MOUSEMOVE 200 -5\nMOUSE_SCROLL -1\n",
	       "mouse 00 7f fb 00\nmouse 00 49 00 00\nmouse 00 00 00 ff\n");
	golden(us, "US", "LEFTCLICK\nCTRL RIGHT_CLICK\n",
	       "kbd 00 00 00 00 00 00 00 00\n"
	       "mouse 01 00 00 00\nmouse 00 00 00 00\n" UP
	       "kbd 01 00 00 00 00 00 00 00\n"
	       "mouse 02 00 00 00\nmouse 00 00 00 00\n" UP);
	golden(us, "US", "HOLD LEFTCLICK\nMOUSEMOVE 1 1\nRELEASE LEFTCLICK\n",
	       "mouse 01 00 00 00\nmouse 01 01 01 00\nmouse 00 00 00 00\n");
	golden(us, "US", "WAIT_FOR_BUTTON_PRESS\nID 1234:abcd Me:Kbd\n",
	       "button\n");
	golden(us, "US, no newline at the end", "STRING x",
	       "kbd 00 00 1b 00 00 00 00 00\n" UP);
	golden(us, "US, STRING leaves out characters it has no key for",
	       "STRING \xe2\x82\xac" "a\n",
	       "kbd 00 00 04 00 00 00 00 00\n" UP);
}

static void checks(const unsigned char *us)
{
	char big[600];

	bad(us, "STRING ok\nFOO\n", 2, "unknown command or key", "FOO");
	bad(us, "a\n", 1, "unknown command or key", "a");
	bad(us, "REM\nDELAY x\n", 2, "bad number", "x");
	bad(us, "DELAY\n", 1, "number missing", "");
	bad(us, "DELAY 3600001\n", 1, "bad number", "3600001");
	bad(us, "DELAY -1\n", 1, "bad number", "-1");
	bad(us, "DELAY 99999999999999999999\n", 1, "bad number",
	    "99999999999999999999");
	bad(us, "REPEAT 2\n", 1, "REPEAT with no line before it", "");
	bad(us, "ENTER\nREPEAT 10001\n", 2, "bad number", "10001");
	bad(us, "ENTER\nREPEAT 0\n", 2, "bad number", "0");
	bad(us, "HOLD FOO\n", 1, "unknown key", "FOO");
	bad(us, "HOLD a b\n", 1, "one key only", "a");
	bad(us, "MEDIA LOUDER\n", 1, "unknown media key", "LOUDER");
	bad(us, "MOUSEMOVE 1\n", 1, "bad number", "");
	bad(us, "MOUSEMOVE 1 40000\n", 1, "bad number", "40000");
	bad(us, "ALTCHAR 123456\n", 1, "bad number", "123456");
	bad(us, "ID 12:34\n", 1, "ID wants VID:PID", "12:34");
	bad(us, "WAIT_FOR_BUTTON_PRESS now\n", 1, "no argument wanted", "now");
	bad(us, "CTRL a b c d e f g\n", 1, "more than 6 keys", "g");
	{
		static const char nul[] = "STRING a\nSTRING b\0c\n";
		struct dk d;
		int r = go(&d, us, nul, sizeof nul - 1, 0);

		check(r == DK_ERROR && d.line == 2
		      && strcmp(d.err, "NUL in the line") == 0,
		      "-n: a NUL in a line is an error, with its line");
	}
	memset(big, 'x', sizeof big);
	memcpy(big, "REM\nSTRING ", 11);
	big[sizeof big - 1] = '\n';
	{
		struct dk d;
		int r = go(&d, us, big, sizeof big, 0);

		check(r == DK_ERROR && d.line == 2
		      && strcmp(d.err, "line too long") == 0,
		      "-n: a line longer than 256 bytes is an error, with its line");
		r = go(&d, us, big, sizeof big, 1);
		check(r == DK_ERROR && nrep == 0,
		      "run: the long line is not typed (nothing cut and sent)");
	}
	{
		struct dk d;
		const char *s = "STRING ab\nENTER\nREPEAT 3\nSTRING z\n";
		int r = go(&d, us, s, strlen(s), 0);

		check(r == DK_DONE && recn == 0,
		      "-n: a good script passes, no reports");
	}
}

static void stops(const unsigned char *us)
{
	struct dk d;
	const char *s = "STRING hello\nDELAY 10000\nSTRING never\n";
	int r;

	stop_after = 3;
	r = go(&d, us, s, strlen(s), 1);
	stop_after = -1;
	check(r == DK_STOP && d.line == 1 && nrep == 3,
	      "a stop from the caller ends the script at once");
	stop_after = 2;
	r = go(&d, us, s, strlen(s), 1);
	stop_after = -1;
	check(r == DK_STOP && strstr(rec, "wait 10000") == NULL,
	      "a stop before a DELAY: the DELAY does not start");
}

/* ---- fuzz ---- */

static const char *words[] = {
	"REM", "ID", "DELAY", "STRING", "STRINGLN", "DEFAULT_DELAY",
	"DEFAULTDELAY", "STRING_DELAY", "STRINGDELAY", "DEFAULT_STRING_DELAY",
	"REPEAT", "SYSRQ", "ALTCHAR", "ALTSTRING", "ALTCODE", "HOLD",
	"RELEASE", "WAIT_FOR_BUTTON_PRESS", "MEDIA", "GLOBE", "MOUSEMOVE",
	"MOUSE_SCROLL", "CTRL", "SHIFT", "ALT", "GUI", "CTRL-ALT", "ENTER",
	"F12", "LEFTCLICK", "RIGHT_CLICK", "VOLUME_UP", "MUTE", "a", "Z",
	"0", "1", "-1", "127", "128", "999", "10000", "3600000", "65535",
	"-40000", "1234:abcd", " ", "\t", "\r", "\xc3\xb1", "\xff", "\xe2\x82",
};
#define NWORDS	(int) (sizeof words / sizeof *words)

/* Whole good lines, so that many scripts run far (and hold keys). */
static const char *goodl[] = {
	"STRING Kaixo, mundua! 123", "STRINGLN \xc3\xb1 ok", "DELAY 5",
	"REPEAT 3", "CTRL a", "CTRL-ALT DELETE", "GUI r", "HOLD SHIFT",
	"RELEASE SHIFT", "HOLD a", "RELEASE a", "HOLD LEFTCLICK",
	"RELEASE LEFTCLICK", "MOUSEMOVE 300 -200", "MOUSESCROLL -300",
	"MEDIA MUTE", "ALTSTRING a\xc3\xb1", "ALTCHAR 0169", "SYSRQ b",
	"GLOBE e", "ENTER", "REM x", "DEFAULT_DELAY 1", "STRING_DELAY 2",
	"WAIT_FOR_BUTTON_PRESS", "ID 1234:5678 A:B", "LEFTCLICK",
};
#define NGOOD	(int) (sizeof goodl / sizeof *goodl)

static void fuzz(const unsigned char *kl, int rounds)
{
	static char s[2048];
	struct dk d;
	int i, n, w, rc, rr, bad_agree = 0, bad_end = 0, bad_rep = 0, done = 0;
	long reps = 0;
	unsigned seed = 1;

	srand(seed);
	for (i = 0; i < rounds; i++) {
		n = 0;
		while (n < (int) sizeof s - 300 && rand() % 40 != 0) {
			int k = rand() % 8;

			if (k == 0) {		/* random bytes */
				int j, m = rand() % 20;

				for (j = 0; j < m; j++) s[n++] = (char) (rand() % 256);
			} else if (k == 1 && rand() % 50 == 0) {	/* long */
				int j, m = 250 + rand() % 20;

				for (j = 0; j < m; j++) s[n++] = 'x';
			} else if (k >= 5) {	/* a good line */
				const char *x = goodl[rand() % NGOOD];

				w = (int) strlen(x);
				memcpy(s + n, x, (size_t) w);
				n += w;
				s[n++] = '\n';
			} else {
				const char *x = words[rand() % NWORDS];

				w = (int) strlen(x);
				memcpy(s + n, x, (size_t) w);
				n += w;
				if (rand() % 3) s[n++] = ' ';
			}
			if (rand() % 3 == 0) s[n++] = '\n';
		}
		rc = go(&d, kl, s, (size_t) n, 0);
		rr = go(&d, kl, s, (size_t) n, 1);
		if (rc != rr) {
			if (bad_agree++ < 3) {
				printf("     check %d, run %d (%s, line %d):\n", rc,
				       rr, d.err ? d.err : "", d.line);
				fwrite(s, 1, (size_t) n, stdout);
				printf("\n");
			}
		}
		if (rr == DK_DONE) done++;
		reps += nrep;
		if (badrep) bad_rep++;
		if (rr == DK_DONE && ((sawkbd && memcmp(lastkbd,
		    "\0\0\0\0\0\0\0\0", 8) != 0) || (sawmouse
		    && lastmouse[0] != 0)))
			bad_end++;
	}
	printf("  fuzz: %d scripts, %d ran to the end, %ld reports\n", rounds,
	       done, reps);
	check(bad_agree == 0, "fuzz: check (-n) and run agree on every script");
	check(bad_rep == 0, "fuzz: every report well formed");
	check(bad_end == 0, "fuzz: a script that ends has let every key and "
	      "button go");
}

/* -r: the reports of one script file. */
static int reports(const char *layout, const char *file)
{
	unsigned char kl[HID_KL_SIZE];
	struct dk d;
	char *l, *nl;
	int fd, r;

	if (strcmp(layout, "us") == 0) memcpy(kl, hid_us, sizeof kl);
	else if (hid_kl_read(kl, layout) < 0) {
		perror(layout);
		return 2;
	}
	if ((fd = open(file, O_RDONLY)) < 0) {
		perror(file);
		return 2;
	}
	reset();
	memset(&d, 0, sizeof d);
	d.kl = kl;
	d.io = &io;
	r = dk_run(&d, fd);
	close(fd);
	for (l = rec; *l != 0; l = nl + 1) {
		if ((nl = strchr(l, '\n')) == NULL) break;
		if (strncmp(l, "wait", 4) != 0 && strncmp(l, "button", 6) != 0)
			fwrite(l, 1, (size_t) (nl + 1 - l), stdout);
	}
	if (r != DK_DONE) {
		fprintf(stderr, "ducktest: %s: line %d: %s %s\n", file, d.line,
			d.err ? d.err : "stopped", d.errarg);
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	unsigned char de[HID_KL_SIZE];
	char p[512];

	if (argc == 4 && strcmp(argv[1], "-r") == 0)
		return reports(argv[2], argv[3]);

	if (argc < 2) {
		fprintf(stderr, "usage: ducktest LAYOUTDIR [ROUNDS]\n");
		return 2;
	}
	snprintf(p, sizeof p, "%s/de-DE.kl", argv[1]);
	if (hid_kl_read(de, p) < 0) {
		perror(p);
		return 2;
	}
	printf("ducktest: the Bad USB script engine (R11.3.3)\n");
	goldens(hid_us, de);
	checks(hid_us);
	stops(hid_us);
	fuzz(hid_us, argc > 2 ? atoi(argv[2]) : 20000);
	fuzz(de, 2000);
	printf("%d failed\n", fails);
	return fails;
}
