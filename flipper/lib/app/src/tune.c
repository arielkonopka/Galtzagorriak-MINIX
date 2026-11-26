/* tune.c - libapp: tunes for /dev/speaker (spec F.2, 7b.2): the built-in
 * tones and the user's RTTTL or Flipper Music Format (.fmf) files, turned
 * into the speaker's notes (4 bytes each: Hz, ms, 16-bit little endian;
 * 0 Hz is a rest).  No I/O: the host tests run this file as it is.
 *
 * RTTTL:	name:d=4,o=5,b=100:8e6,8p,c#.6,...
 * FMF:		Filetype: Flipper Music Format / Version: 0 / BPM: 120 /
 *		Duration: 8 / Octave: 5 / Notes: 8A#5., P, C6, ...
 * A note in either: [duration]letter[#][.][octave][.]; letters a-g (h
 * is b), p is a rest.  The tunes are short: the alert writes one
 * /dev/speaker block (TUNE_MAX notes) per ring, longer tunes are cut.
 */
#include <string.h>
#include "app.h"

/* The built-in tones, as RTTTL: the default of an alarm is "beep", of
 * the others "chime".  const: in flash. */
const char *const tune_names[] = {
	"beep", "chime", "rise", "classic", "gentle", "pips",
	"watch", "hourly", "jingle", "catch", "eggs", "game", "duel",
	"marsel", "saints", "ode", "elise", "troll", "korob", "spy", "chirp", NULL
};
static const char *const tune_text[] = {
	"beep:d=16,o=7,b=150:b6,p,b6,p,b6",		/* ~2 kHz x3 */
	"chime:d=8,o=6,b=180:g,4c7",
	"rise:d=16,o=5,b=160:c,e,g,c6,e6,g6,4c7",
	"classic:d=16,o=7,b=200:c,p,c,p,c,p,c,8p,c,p,c,p,c,p,c",
	"gentle:d=8,o=5,b=100:e,g,a,4b,p,a,4g",
	"pips:d=16,o=6,b=120:a,4p,a,4p,a,4p,4a6",
	/* 1980s digital watch, on the piezo's square wave (issue "radyjko z
	 * lat 80"): original little tunes in that style */
	"watch:d=16,o=6,b=170:c,e,g,c7,g,e,c,8p,c,e,g,c7,4p",
	"hourly:d=16,o=6,b=190:e,e,p,e,e,4p,g,g,p,g,g,4p,8c7",
	"jingle:d=8,o=6,b=150:e,g,4c7,g,e,4c,e,g,a,4g,p,f,a,4c7,a,f,4c,16e,16g,2c7",
	/* game blips in the style of the pocket LCD games of the 1980s and
	 * their eastern clones (catch the eggs, game over, the alarm race):
	 * original, square-wave, short */
	"catch:d=16,o=6,b=200:c,e,g,e,c,e,g,c7,4p",
	"eggs:d=16,o=6,b=210:g,p,g,p,e,p,e,p,c,p,c,4p,8g",
	"game:d=8,o=6,b=140:g,e,c,4p,e,c,a5,4p,2c5",
	"duel:d=16,o=6,b=240:a,e7,a,e7,a,e7,a,e7,4p,a,e7,a,e7,a,e7,a,e7",
	/* our own square-wave arrangements of public-domain tunes (first
	 * phrases, as alarm tones); "spy" is an original riff in a surf/spy
	 * style, not a copy of any film or TV theme */
	"marsel:d=8,o=5,b=110:g4,4c,c,c,4e,4e,8d,8c,4d,4g,8g,4a,a,4b4,4c6,4c6.",
	"saints:d=8,o=5,b=140:4c,4e,4f,2g,4c,4e,4f,2g,4c,4e,4f,g,e,4c,4e,2d",
	"ode:d=4,o=5,b=130:e,e,f,g,g,f,e,d,c,c,d,e,4e.,8d,2d",
	"elise:d=16,o=6,b=120:e,d#,e,d#,e,b5,d,c,4a5,p,c5,e5,a5,4b5,p,e5,g#5,b5,4c,p,e5,e,d#,e,d#,e,b5,d,c,4a5",
	"troll:d=8,o=5,b=140:e,f#,g,a,b,g,4b,a#,f#,4a#,a,e,4a,e,f#,g,a,b,g,b,e6,d6,b,g,b,2d6",
	"korob:d=4,o=6,b=140:e,8b5,8c,d,8c,8b5,a5,8a5,8c,e,d,8c,8b5,4c.,8d,e,c,a5,4a5,2p",
	"spy:d=8,o=5,b=150:a,c6,a,e,4a,p,g,b,g,d,4g,p,a,c6,e6,c6,a,4e6,2a",
	/* a synthetic alarm: fast chirps up and down the piezo's loudest
	 * range (2 to 4 kHz), each a sweep c7..b7..c7 */
	"chirp:d=32,o=7,b=200:c7,d7,e7,f7,g7,a7,b7,a7,g7,f7,e7,d7,c7,8p,c7,d7,e7,f7,g7,a7,b7,a7,g7,f7,e7,d7,c7,8p,c7,d7,e7,f7,g7,a7,b7,a7,g7,f7,e7,d7,c7,8p,c7,d7,e7,f7,g7,a7,b7,a7,g7,f7,e7,d7,c7,8p",
};

const char *tune_builtin(const char *name)
{
	int i;

	for (i = 0; tune_names[i] != NULL; i++)
		if (strcmp(name, tune_names[i]) == 0) return tune_text[i];
	return NULL;
}

/* C4 .. B4 in Hz. */
static const unsigned short c4[12] = {
	262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494
};
static const signed char semi[7] = { 9, 11, 0, 2, 4, 5, 7 };	/* a..g */

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

static int isdig(int c)
{
	return c >= '0' && c <= '9';
}

/* A decimal number at *p (advanced); -1 if there is none. */
static int number(const char **p)
{
	int v = 0, any = 0;

	while (isdig(**p) && v < 10000) {
		v = v * 10 + *(*p)++ - '0';
		any = 1;
	}
	return any ? v : -1;
}

/* The notes from p up to the end of the line or string. */
static int notes(const char *p, int defd, int defo, int bpm,
					unsigned char *out, int max)
{
	int n = 0, d, o, l, s, dot;
	unsigned long hz, ms;

	if (bpm < 1 || defd < 1 || defo < 0) return -1;
	while (n < max) {
		while (*p == ' ' || *p == '\t' || *p == ',') p++;
		if (*p == 0 || *p == '\n' || *p == '\r') break;
		if ((d = number(&p)) < 1) d = defd;
		l = lower(*p++);
		if (l == 'h') l = 'b';
		if (l != 'p' && (l < 'a' || l > 'g')) return -1;
		s = l == 'p' ? -1 : semi[l - 'a'];
		dot = 0;
		if (*p == '#') {
			p++;
			if (s >= 0) s++;
		}
		if (*p == '.') { p++; dot = 1; }
		if ((o = number(&p)) < 0) o = defo;
		if (*p == '.') { p++; dot = 1; }
		while (*p != 0 && *p != ',' && *p != '\n') p++;
		if (s == 12) { s = 0; o++; }		/* b# */
		if (o > 8) o = 8;
		hz = s < 0 ? 0 : o >= 4 ? (unsigned long) c4[s] << (o - 4)
					: (unsigned long) c4[s] >> (4 - o);
		ms = 240000UL / ((unsigned long) bpm * d);
		if (dot) ms += ms / 2;
		if (ms == 0) ms = 1;
		if (ms > 65535) ms = 65535;
		out[4*n] = hz & 0xFF; out[4*n+1] = hz >> 8;
		out[4*n+2] = ms & 0xFF; out[4*n+3] = ms >> 8;
		n++;
	}
	return n;
}

/* The value of "Key:" at the start of a line of an FMF text, or NULL. */
static const char *fmf_key(const char *src, const char *key)
{
	int l = strlen(key);
	const char *p = src;

	while (p != NULL && *p != 0) {
		if (strncmp(p, key, l) == 0 && p[l] == ':') {
			p += l + 1;
			while (*p == ' ' || *p == '\t') p++;
			return p;
		}
		if ((p = strchr(p, '\n')) != NULL) p++;
	}
	return NULL;
}

int tune_parse(const char *src, unsigned char *out, int max)
{
	const char *p, *q;
	int d = 4, o = 6, b = 63, v;

	if (fmf_key(src, "Notes") != NULL) {
		if ((p = fmf_key(src, "BPM")) != NULL) b = number(&p);
		if ((p = fmf_key(src, "Duration")) != NULL) d = number(&p);
		if ((p = fmf_key(src, "Octave")) != NULL) o = number(&p);
		return notes(fmf_key(src, "Notes"), d, o, b, out, max);
	}
	/* RTTTL: name, then d=,o=,b= settings, then the notes. */
	if ((p = strchr(src, ':')) == NULL || (q = strchr(p + 1, ':')) == NULL)
		return -1;
	for (p++; p < q; ) {
		int k = lower(*p);

		if (k == ' ' || k == ',') { p++; continue; }
		if (p[1] != '=') return -1;
		p += 2;
		v = number(&p);
		if (k == 'd') d = v;
		else if (k == 'o') o = v;
		else if (k == 'b') b = v;
		else return -1;
	}
	return notes(q + 1, d, o, b, out, max);
}
