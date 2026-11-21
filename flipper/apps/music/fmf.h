/* SPDX-License-Identifier: GPL-3.0-or-later
 * fmf - the stock Music Player's song files, parsed one byte at a time
 * (issue #420).  Two formats, as the stock (and Unleashed) music_player:
 *
 *  Flipper Music Format (.fmf):	Filetype: Flipper Music Format
 *					Version: 0
 *					BPM: 120
 *					Duration: 4
 *					Octave: 5
 *					Notes: 8P, 8G5, 4A#5., C6
 *  RTTTL (.txt, .rtttl):		name:d=4,o=5,b=120:8p,8g,4a#.,c6
 *
 * A note is [duration] letter [#] [dots] [octave] [dots]: duration 1..128
 * (a whole note is 4 beats), letter A-G or P (a rest), octave 0..8.  The
 * header's values stand for what a note leaves out.  Each dot adds half
 * of what the previous one added (a dotted note is 1.5 times as long).
 *
 * No stdio, no malloc: the caller owns struct fmf (about 64 B), feeds the
 * file's bytes, then -1 at the end, and gets the notes one by one.  The
 * whole song is never in memory.
 *
 * Source of the format: the stock firmware's music_player_worker.c
 * (applications/external/music_player in Unleashed), GPLv3; rewritten as
 * a streaming parser here.
 */
#ifndef FMF_H
#define FMF_H

#define FMF_LINE	40		/* a header line, longer ones cut */
#define FMF_TOK		12		/* one note, longer ones are errors */

struct fmf_note {
	unsigned short hz;		/* 0: a rest */
	unsigned short ms;		/* at least 1, at most 65535 */
};

struct fmf {
	short bpm, dur, oct;		/* the header's values */
	unsigned char st;		/* FS_* below */
	unsigned char ll, tl;		/* bytes in line, tok */
	unsigned char bad;		/* an error was seen: FE_* */
	unsigned short line;		/* the line the parser is on, from 1 */
	unsigned short notes;		/* notes given so far */
	char lbuf[FMF_LINE];
	char tok[FMF_TOK];
};

/* fmf_feed() results */
#define FMF_MORE	0		/* nothing yet: feed the next byte */
#define FMF_NOTE	1		/* *n holds a note */
#define FMF_END		2		/* the song is over (after -1) */
#define FMF_ERR		(-1)		/* a bad file: see bad and line */

/* bad */
#define FE_NONE		0
#define FE_TYPE		1		/* "Filetype:" is not the music one */
#define FE_HEADER	2		/* BPM, Duration or Octave out of range */
#define FE_NOTE		3		/* a note that does not parse */
#define FE_NONOTES	4		/* the file ends before any note */

void fmf_init(struct fmf *m);
int fmf_feed(struct fmf *m, int c, struct fmf_note *n);
/* A note text alone ("8A#5."), with the header values of m: 0 and *n, or
 * -1.  fmf_feed() uses it; the tests too. */
int fmf_note(const struct fmf *m, const char *s, struct fmf_note *n);
const char *fmf_error(int bad);

#endif /* FMF_H */
