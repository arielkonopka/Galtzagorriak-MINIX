/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host tests of fmf.c (issue #420): make -C flipper/apps/music test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fmf.h"

static int fails;

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, \
	__LINE__, #c); fails++; } } while (0)

/* Feed a whole text; the notes into out (up to max): their count, or
 * -1 - bad on an error. */
static int run(const char *s, struct fmf_note *out, int max, struct fmf *m)
{
	struct fmf_note n;
	int k = 0, r, c;

	fmf_init(m);
	for (;;) {
		c = *s ? (unsigned char) *s++ : -1;
		do {
			r = fmf_feed(m, c, &n);
			if (r == FMF_NOTE && k < max) out[k++] = n;
		} while (r == FMF_NOTE && c < 0);
		if (r == FMF_ERR) return -1 - m->bad;
		if (r == FMF_END) return k;
	}
}

static void notes(void)
{
	struct fmf m;
	struct fmf_note n;

	fmf_init(&m);			/* 120 BPM, 4, octave 5 */
	CHECK(fmf_note(&m, "A4", &n) == 0 && n.hz == 440 && n.ms == 500);
	CHECK(fmf_note(&m, "A", &n) == 0 && n.hz == 880);
	CHECK(fmf_note(&m, "8C4", &n) == 0 && n.hz == 262 && n.ms == 250);
	CHECK(fmf_note(&m, "C#4", &n) == 0 && n.hz == 277);
	CHECK(fmf_note(&m, "4A4.", &n) == 0 && n.ms == 750);
	CHECK(fmf_note(&m, "4A.4", &n) == 0 && n.ms == 750);
	CHECK(fmf_note(&m, "4A4..", &n) == 0 && n.ms == 875);
	CHECK(fmf_note(&m, "1P", &n) == 0 && n.hz == 0 && n.ms == 2000);
	CHECK(fmf_note(&m, "p", &n) == 0 && n.hz == 0);
	CHECK(fmf_note(&m, "C0", &n) == 0 && n.hz == 16);
	CHECK(fmf_note(&m, "B8", &n) == 0 && n.hz == 7902);
	CHECK(fmf_note(&m, "B#8", &n) == 0 && n.hz == 8372);
	CHECK(fmf_note(&m, "128a4", &n) == 0 && n.ms == 15);
	CHECK(fmf_note(&m, "H4", &n) < 0);
	CHECK(fmf_note(&m, "A9", &n) < 0);
	CHECK(fmf_note(&m, "0A", &n) < 0);
	CHECK(fmf_note(&m, "256A", &n) < 0);
	CHECK(fmf_note(&m, "P#", &n) < 0);
	CHECK(fmf_note(&m, "A4x", &n) < 0);
	CHECK(fmf_note(&m, "", &n) < 0);
	CHECK(fmf_note(&m, "A4....", &n) < 0);
	m.bpm = 1;			/* a whole note at 1 BPM: 240 s */
	CHECK(fmf_note(&m, "1A", &n) == 0 && n.ms == 65535);
}

static void files(void)
{
	struct fmf m;
	struct fmf_note o[16];

	CHECK(run("Filetype: Flipper Music Format\nVersion: 0\nBPM: 60\n"
		  "Duration: 8\nOctave: 4\nNotes: 8P, A, 4A#5., C6\n",
		  o, 16, &m) == 4);
	CHECK(o[0].hz == 0 && o[0].ms == 500);
	CHECK(o[1].hz == 440 && o[1].ms == 500);
	CHECK(o[2].hz == 932 && o[2].ms == 1500);
	CHECK(o[3].hz == 1047 && o[3].ms == 500);
	/* CRLF, no newline at the end, extra spaces */
	CHECK(run("Filetype: Flipper Music Format\r\nBPM: 120 \r\n"
		  "Notes:  C4 ,D4,,E4", o, 16, &m) == 3);
	CHECK(o[2].hz == 330);
	/* lines after the Notes line do not count */
	CHECK(run("Filetype: Flipper Music Format\nNotes: C4\nFoo: x\n",
		  o, 16, &m) == 1);
	CHECK(run("Filetype: Flipper SubGhz Key File\nNotes: C\n", o, 16, &m)
	      == -1 - FE_TYPE);
	CHECK(run("hello world\n", o, 16, &m) == -1 - FE_TYPE);
	CHECK(run("Filetype: Flipper Music Format\nBPM: 0\nNotes: C\n",
		  o, 16, &m) == -1 - FE_HEADER);
	CHECK(run("Filetype: Flipper Music Format\nOctave: 9\nNotes: C\n",
		  o, 16, &m) == -1 - FE_HEADER);
	CHECK(run("Filetype: Flipper Music Format\nNotes: C, X\n", o, 16, &m)
	      == -1 - FE_NOTE);
	CHECK(m.line == 2);
	CHECK(run("Filetype: Flipper Music Format\nBPM: 60\n", o, 16, &m)
	      == -1 - FE_NONOTES);
	CHECK(run("", o, 16, &m) == -1 - FE_NONOTES);
	CHECK(run("Filetype: Flipper Music Format\nNotes: AAAAAAAAAAAAAAAA\n",
		  o, 16, &m) == -1 - FE_NOTE);
	/* RTTTL */
	CHECK(run("Test:d=8,o=4,b=60:a,4c5,p,g#.\n", o, 16, &m) == 4);
	CHECK(o[0].hz == 440 && o[0].ms == 500);
	CHECK(o[1].hz == 523 && o[1].ms == 1000);
	CHECK(o[2].hz == 0);
	CHECK(o[3].hz == 415 && o[3].ms == 750);
	CHECK(run("x: d = 4 , o = 5 , b = 120 :\n c,\n d\n", o, 16, &m) == 2);
	CHECK(o[0].hz == 523 && o[0].ms == 500 && o[1].hz == 587);
	CHECK(run("x::c", o, 16, &m) == 1 && o[0].hz == 1047 && o[0].ms == 952);
	CHECK(run("x:d=4,q=5:c", o, 16, &m) == -1 - FE_HEADER);
	CHECK(run("x:d=4,b=2000:c", o, 16, &m) == -1 - FE_HEADER);
}

/* Random bytes never make it read or write out of bounds (ASan) and
 * every note it gives is in range. */
static void fuzz(void)
{
	static const char al[] = "Filetype:FlipperMusicFormatNotesBPMDuration"
				 "Octave:d=o=b=,.#0123456789ABCDEFGPabcdefgp \n\r:";
	char buf[200];
	struct fmf m;
	struct fmf_note o[64];
	int i, j, k, r;

	srand(420);
	for (i = 0; i < 200000; i++) {
		int len = rand() % (int) sizeof buf;

		for (j = 0; j < len - 1; j++)
			buf[j] = rand() % 8 ? al[rand() % (sizeof al - 1)]
					    : (char) (rand() % 255 + 1);
		buf[len > 0 ? len - 1 : 0] = 0;
		if (i % 3 == 0 && len > 40)
			memcpy(buf, "Filetype: Flipper Music Format\nNotes:", 37);
		r = run(buf, o, 64, &m);
		for (k = 0; k < r && k < 64; k++)
			CHECK(o[k].ms >= 1 && o[k].hz <= 8372);
		if (fails) return;
	}
}

int main(void)
{
	notes();
	files();
	fuzz();
	if (fails) { printf("fmf: %d failed\n", fails); return 1; }
	printf("fmf: all tests passed\n");
	return 0;
}
