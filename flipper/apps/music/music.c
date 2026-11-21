/* SPDX-License-Identifier: GPL-3.0-or-later
 * music - the stock Music Player on Galtzagorriak MINIX (issue #420), a
 * window app (spec K.8): the launcher draws, the app only describes.
 *
 *	list	the songs in /ext/music_player (/home/flipper/music_player):
 *		.fmf (Flipper Music Format) and .txt/.rtttl (RTTTL), paged
 *	play	the song: its state (Playing, Done, Stopped, the error and
 *		its line), Volume (0-100, live), Play again or Stop
 *
 * The song is never in memory: fmf.c parses the file a byte at a time,
 * and the notes go to /dev/speaker in writes of at most CHUNK ms, so
 * between two writes the app looks at its events (Stop, Volume, Back)
 * without a second process.  Long notes are cut into CHUNK ms pieces of
 * the same tone.  The window changes only when the state does.
 *
 * Volume is kept for the session only (no file, no flash).  The songs are
 * the user's own: none ship with the package but a scale to try it.
 *
 *	Music Player:nobody:@music:=/usr/bin/music	(/etc/apps)
 *
 * Source: the stock / Unleashed music_player (GPLv3), its formats and its
 * screens (file list, then the playing view with volume); no code copied.
 * RAM: no stdio, no malloc, no opendir() (the directory is read in raw
 * entries), no signal handler: no MM gap (K.8).
 */
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "gm.h"
#include "fmf.h"

#define SDIR	"/home/flipper/music_player"	/* /ext/music_player */
#define NAMEMAX	60				/* V3 entries (spec O.4) */
#define PAGE	16				/* rows of a list window */
#define CHUNK	100				/* ms of sound per write */
#define BATCH	16				/* notes per write */

enum { W_LIST, W_PLAY };
enum { S_PLAY, S_DONE, S_STOP, S_ERROR, S_NODEV };

static struct {
	char song[NAMEMAX + 1];
	short first;			/* the list's page */
	short vol;			/* 0..100 */
	unsigned char cur, st, bad;
	unsigned short line;		/* of an error */
	int fd, sp;			/* the song, /dev/speaker; -1 */
	struct fmf m;
	struct fmf_note left;		/* what is left of the last note */
	short bn, bi;			/* bytes in rb, the next one */
	unsigned char rb[64];		/* the song file, read ahead */
} a;

static char *num(char *t, long v)		/* t[12] */
{
	char *p = t + 11;
	unsigned long u = (unsigned long) (v < 0 ? -v : v);

	*p = 0;
	do *--p = (char) ('0' + u % 10); while ((u /= 10) > 0);
	if (v < 0) *--p = '-';
	return p;
}

static int ends(const char *s, const char *e)
{
	size_t l = strlen(s), k = strlen(e);

	return l > k && strcmp(s + l - k, e) == 0;
}

static int song(const char *nm)
{
	return nm[0] != '.' && (ends(nm, ".fmf") || ends(nm, ".txt")
				|| ends(nm, ".rtttl"));
}

/* The n-th song (from 0) into name, or the count when n < 0: the count,
 * 1 found, 0 not found, -1 no directory.  Raw entries as badusb-gui:
 * 16 B (V1/V2) or 64 B (V3, spec O.4), told apart by the first ".". */
static int dir_scan(int n, char *name)
{
	char b[256], nm[NAMEMAX + 1];
	int fd, i, k, count = 0, esz = 0, nl;

	if ((fd = open(SDIR, O_RDONLY)) < 0) return -1;
	while ((k = (int) read(fd, b, sizeof b)) > 0) {
		if (esz == 0) esz = k >= 6 && b[4] == '.' && b[5] == 0 ? 64 : 16;
		nl = esz - (esz == 64 ? 4 : 2);
		for (i = 0; i + esz <= k; i += esz) {
			if (b[i] == 0 && b[i + 1] == 0 && (esz == 16
					|| (b[i + 2] == 0 && b[i + 3] == 0)))
				continue;		/* inode 0: free */
			memcpy(nm, b + i + esz - nl, nl);
			nm[nl] = 0;
			if (!song(nm)) continue;
			if (count++ == n) {
				close(fd);
				strcpy(name, nm);
				return 1;
			}
		}
	}
	close(fd);
	return n < 0 ? count : 0;
}

static void shown(char *nm)		/* the name without its type */
{
	char *d = strrchr(nm, '.');

	if (d != NULL && d != nm) *d = 0;
}

static void w_list(int first)
{
	char key[8], t[12], nm[NAMEMAX + 1];
	int n = dir_scan(-1, nm), row;

	if (n < 0) n = 0;
	if (first < 0 || first >= n) first = 0;
	a.first = (short) first;
	gm_win("list", "list", "Music Player", n > PAGE ? n : 0, first);
	for (row = first; row < first + PAGE && row < n; row++) {
		if (dir_scan(row, nm) != 1) break;
		shown(nm);
		key[0] = 's';
		strcpy(key + 1, num(t, row));
		gm_item(key, nm, "");
	}
	if (n == 0) {
		gm_line("No songs: put .fmf or");
		gm_line("RTTTL .txt files in");
		gm_line("/ext/music_player");
	}
	gm_show();
	a.cur = W_LIST;
}

static void w_play(void)
{
	static const char state[][12] = { "Playing", "Done", "Stopped",
					  "Error", "No speaker" };
	char l[48], t[12], nm[NAMEMAX + 1];

	strcpy(nm, a.song);
	shown(nm);
	gm_win_live("play", "list", "Music Player", 1);
	gm_line(nm);
	strcpy(l, state[a.st]);
	if (a.st == S_ERROR) {
		strcat(l, ": ");
		strcat(l, fmf_error(a.bad));
		if (a.bad != FE_NONOTES) {
			strcat(l, ", line ");
			strcat(l, num(t, a.line));
		}
	} else if (a.st == S_PLAY) {
		strcat(l, ", ");
		strcat(l, num(t, a.m.bpm));
		strcat(l, " BPM");
	}
	gm_line(l);
	gm_int("vol", "Volume", 0, 100, 10, a.vol, "%");
	if (a.st == S_PLAY) gm_action("stop", "Stop");
	else gm_action("play", "Play again");
	gm_show();
	a.cur = W_PLAY;
}

static void put(unsigned char *p, unsigned hz, unsigned ms)
{
	p[0] = (unsigned char) (hz & 0xFF);
	p[1] = (unsigned char) (hz >> 8);
	p[2] = (unsigned char) (ms & 0xFF);
	p[3] = (unsigned char) (ms >> 8);
}

static void volume(void)
{
	unsigned char b[4];

	if (a.sp < 0) return;
	put(b, 0xFFFF, (unsigned) a.vol);
	(void) write(a.sp, b, 4);
}

static void stop(int st)
{
	if (a.fd >= 0) close(a.fd);
	if (a.sp >= 0) close(a.sp);	/* closing ends the sound */
	a.fd = a.sp = -1;
	a.left.ms = 0;
	a.bn = a.bi = 0;
	a.st = (unsigned char) st;
}

static void start(void)
{
	char path[sizeof SDIR + NAMEMAX + 1];

	stop(S_STOP);
	strcpy(path, SDIR "/");
	strcat(path, a.song);
	fmf_init(&a.m);
	if ((a.fd = open(path, O_RDONLY)) < 0) {
		a.st = S_ERROR, a.bad = FE_NONOTES;
		return;
	}
	if ((a.sp = open("/dev/speaker", O_WRONLY)) < 0) {
		stop(S_NODEV);
		return;
	}
	volume();
	a.st = S_PLAY;
}

/* The next note of the song into a.left: 1, or 0 at the end (a.st set). */
static int next(void)
{
	int r, c;

	if (a.left.ms > 0) return 1;
	for (;;) {
		if (a.fd < 0) c = -1;
		else {
			if (a.bi == a.bn) {
				a.bn = (short) read(a.fd, a.rb, sizeof a.rb);
				a.bi = 0;
				if (a.bn <= 0) {
					close(a.fd);
					a.fd = -1;
					a.bn = 0;
					continue;
				}
			}
			c = a.rb[a.bi++];
		}
		r = fmf_feed(&a.m, c, &a.left);
		if (r == FMF_NOTE) return 1;
		if (r == FMF_END) { stop(S_DONE); break; }
		if (r == FMF_ERR) {
			a.bad = a.m.bad;
			a.line = a.m.line;
			stop(S_ERROR);
			break;
		}
	}
	return 0;
}

/* At most CHUNK ms of the song to the speaker (the write returns when it
 * has played). */
static void play(void)
{
	unsigned char b[4 * BATCH];
	unsigned ms, total = 0;
	int k = 0;

	while (k < BATCH && total < CHUNK && next()) {
		ms = a.left.ms;
		if (ms > CHUNK - total) ms = CHUNK - total;
		put(b + 4 * k++, a.left.hz, ms);
		a.left.ms = (unsigned short) (a.left.ms - ms);
		total += ms;
	}
	if (k > 0 && a.sp >= 0 && write(a.sp, b, (size_t) (4 * k)) < 0
	    && errno != EINTR)
		stop(S_NODEV);
	if (a.st != S_PLAY && a.cur == W_PLAY) w_play();
}

static void values(char **f)
{
	char *k, *v;
	int i;

	for (i = 2; i < GM_FIELDS && f[i] != NULL; i++) {
		k = gm_kv(f[i], &v);
		if (strcmp(k, "vol") == 0) {
			a.vol = (short) atoi(v);
			if (a.vol < 0) a.vol = 0;
			if (a.vol > 100) a.vol = 100;
			volume();
		}
	}
}

static void ok(const char *win, const char *key)
{
	if (strcmp(win, "list") == 0 && key[0] == 's') {
		if (dir_scan(atoi(key + 1), a.song) != 1) {
			w_list(a.first);
			return;
		}
		start();
		w_play();
	} else if (strcmp(key, "stop") == 0) {
		stop(S_STOP);
		w_play();
	} else if (strcmp(key, "play") == 0) {
		start();
		w_play();
	} else
		gm_busy(0);
}

int main(int argc, char **argv)
{
	char buf[128], ev[128], *f[GM_FIELDS];
	static const char use[] = "music: start the Music Player from the "
		"launcher\n";

	(void) argv;
	if (argc != 1 || gm_init(buf, sizeof buf) < 0) {
		(void) write(2, use, sizeof use - 1);
		return 1;
	}
	a.fd = a.sp = -1;
	a.vol = 80;
	a.st = S_STOP;
	w_list(0);
	for (;;) {
		/* playing: a chunk, then only a look at the events */
		if (a.st == S_PLAY && !gm_wait(ev, 0)) {
			play();
			continue;
		}
		switch (gm_event(ev, sizeof ev, f, GM_FIELDS)) {
		case GM_OK:
			if (f[1] != NULL && f[2] != NULL) ok(f[1], f[2]);
			break;
		case GM_SUBMIT:
			if (f[1] != NULL && strcmp(f[1], "play") == 0) values(f);
			break;
		case GM_BACK:		/* the desktop shows the list */
			stop(S_STOP);
			w_list(a.first);
			break;
		case GM_PAGE:
			if (f[1] != NULL && f[2] != NULL
			    && strcmp(f[1], "list") == 0)
				w_list(atoi(f[2]));
			break;
		case GM_REDRAW:
			if (a.cur == W_PLAY) w_play();
			else w_list(a.first);
			break;
		case GM_CLOSE:
			stop(S_STOP);
			return 0;
		}
	}
}
