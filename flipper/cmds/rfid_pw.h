/* rfid_pw.h - the copier's T5577 password and the dictionary it lives in
 * (issue #448).  Included by rfid.c and by flipper/tests/lfrfid/rfidpwtest.c
 * (host test), so nothing here touches the device.
 *
 * The dictionary is the user's file of T5577 passwords, one 8-digit hex
 * word per line, `#` for a comment (t5577.h, T5577_DICT_USER_PATH's
 * format).  A password the copier made is appended as
 *
 *	1A2B3C4D # copy
 *
 * BEFORE it is written to any tag (the rule: every key or password written
 * to a card is in the dictionary first).  The file is only ever streamed a
 * line at a time and appended to: no table in RAM (one 48-byte line buffer
 * on the stack), one small SD-card append per copy and nothing in internal
 * flash.  "The password of the previous copier" is the last `# copy` line,
 * so it survives a reboot with no second file.
 *
 * Passwords the copier makes have bit 31 clear: a password frame that
 * reaches a tag with no password is read off shifted bit positions, where
 * bit 31 lands on the lock bit (t5577.h) - a word with it clear can never
 * lock a tag by mistake.  31 random bits are plenty for a door fob.
 *
 * The choice (Copier: legacy|own|previous|dict) is a two-line file:
 *	Copier: own
 *	Password: 1A2B3C4D		(only for dict)
 */
#ifndef RFID_PW_H
#define RFID_PW_H

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PW_DICT	"/home/flipper/lfrfid/assets/t5577_dict_user.lfrfid"
#define PW_CONF	"/home/flipper/lfrfid/copier.conf"
#define PW_RAND	"/dev/random"

#define PW_LEGACY	0	/* no password, as before */
#define PW_OWN		1	/* a new random one for every copy (default) */
#define PW_PREVIOUS	2	/* the one the last copy used */
#define PW_DICT_PICK	3	/* one chosen from the dictionary */
#define PW_MODES	4

/* chars, not pointers: a const pointer table is relocated into RAM in a PIC
 * program, this stays in flash */
static const char pw_mode_name[PW_MODES][9] =
	{ "legacy", "own", "previous", "dict" };

/* One dictionary line -> its word: 8 hex digits, then space, # or the end. */
static int pw_line(const char *s, unsigned long *w)
{
	unsigned long v = 0;
	int i;

	while (*s == ' ' || *s == '\t') s++;
	for (i = 0; i < 8; i++, s++) {
		if (!isxdigit((unsigned char) *s)) return 0;
		v = v << 4 | (unsigned long) (isdigit((unsigned char) *s)
			? *s - '0' : (tolower((unsigned char) *s) - 'a' + 10));
	}
	if (*s != 0 && *s != ' ' && *s != '\t' && *s != '#'
					&& *s != '\n' && *s != '\r')
		return 0;
	*w = v;
	return 1;
}

/* The next word of the open dictionary: 1 (copy = made by the copier), or 0
 * at the end.  Over-long lines are skipped whole. */
static int pw_next(FILE *f, unsigned long *w, int *copy)
{
	char l[48];

	while (fgets(l, sizeof l, f) != NULL) {
		if (strchr(l, '\n') == NULL && !feof(f)) {	/* too long */
			int c;

			while ((c = getc(f)) != EOF && c != '\n') ;
			continue;
		}
		if (pw_line(l, w)) {
			if (copy) *copy = strstr(l, "# copy") != NULL;
			return 1;
		}
	}
	return 0;
}

/* 1 if the word is in the dictionary (a missing file: not). */
static int pw_has(const char *path, unsigned long w)
{
	FILE *f;
	unsigned long x;
	int r = 0;

	if ((f = fopen(path, "r")) == NULL) return 0;
	while (!r && pw_next(f, &x, NULL)) r = x == w;
	fclose(f);
	return r;
}

/* The last word the copier made; 0 if there is none. */
static int pw_previous(const char *path, unsigned long *w)
{
	FILE *f;
	unsigned long x;
	int c, r = 0;

	if ((f = fopen(path, "r")) == NULL) return 0;
	while (pw_next(f, &x, &c)) if (c) { *w = x; r = 1; }
	fclose(f);
	return r;
}

/* Entry number `n` (from 0) of the dictionary: its word, or 0 past the end.
 * For the lists of the app and the sweep: one pass each, no table. */
static int pw_nth(const char *path, int n, unsigned long *w)
{
	FILE *f;
	int r = 0;

	if ((f = fopen(path, "r")) == NULL) return 0;
	while (pw_next(f, w, NULL)) if (n-- == 0) { r = 1; break; }
	fclose(f);
	return r;
}

static int pw_count(const char *path)
{
	FILE *f;
	unsigned long w;
	int n = 0;

	if ((f = fopen(path, "r")) == NULL) return 0;
	while (pw_next(f, &w, NULL)) n++;
	fclose(f);
	return n;
}

/* Append a word the copier is about to write (copy: marked `# copy`, so it
 * counts as "previous"; a word the user adds is not).  0, or -1 if it could
 * not be saved (then nothing may be written to a tag with it). */
static int pw_record(const char *path, unsigned long w, int copy)
{
	FILE *f;
	char dir[96], *s;
	int r;

	if (strlen(path) < sizeof dir) {	/* the folders, if new */
		strcpy(dir, path);
		for (s = dir + 1; (s = strchr(s, '/')) != NULL; s++) {
			*s = 0;
			mkdir(dir, 0755);
			*s = '/';
		}
	}
	if ((f = fopen(path, "a")) == NULL) return -1;
	r = fprintf(f, copy ? "%08lX # copy\n" : "%08lX\n",
			w & 0xFFFFFFFFUL) > 0 ? 0 : -1;
	if (fclose(f) != 0) r = -1;
	return r;
}

/* A fresh random word (bit 31 clear, not 0, not in the dictionary) from
 * `rnd` (/dev/random).  0, or -1. */
static int pw_random(const char *path, const char *rnd, unsigned long *w)
{
	FILE *f;
	unsigned char b[4];
	unsigned long v;
	int t;

	if ((f = fopen(rnd, "r")) == NULL) return -1;
	for (t = 0; t < 16; t++) {
		if (fread(b, 1, 4, f) != 4) break;
		v = ((unsigned long) b[0] << 24 | (unsigned long) b[1] << 16
			| (unsigned long) b[2] << 8 | b[3]) & 0x7FFFFFFFUL;
		if (v != 0 && !pw_has(path, v)) {
			fclose(f);
			*w = v;
			return 0;
		}
	}
	fclose(f);
	return -1;
}

/* The saved choice (default own); a missing file is the default. */
static void pw_conf_load(const char *path, int *mode, unsigned long *w)
{
	FILE *f;
	char l[48];
	int i;

	*mode = PW_OWN;
	*w = 0;
	if ((f = fopen(path, "r")) == NULL) return;
	while (fgets(l, sizeof l, f) != NULL) {
		if (strncmp(l, "Copier:", 7) == 0) {
			char *s = l + 7;

			while (*s == ' ') s++;
			for (i = 0; i < PW_MODES; i++)
				if (strncmp(s, pw_mode_name[i],
						strlen(pw_mode_name[i])) == 0)
					*mode = i;
		} else if (strncmp(l, "Password:", 9) == 0)
			pw_line(l + 9, w);
	}
	fclose(f);
}

static int pw_conf_save(const char *path, int mode, unsigned long w)
{
	FILE *f;
	char dir[96], *s;
	int r;

	if (strlen(path) < sizeof dir) {
		strcpy(dir, path);
		for (s = dir + 1; (s = strchr(s, '/')) != NULL; s++) {
			*s = 0;
			mkdir(dir, 0755);
			*s = '/';
		}
	}
	if ((f = fopen(path, "w")) == NULL) return -1;
	r = fprintf(f, "Copier: %s\n", pw_mode_name[mode]) > 0 ? 0 : -1;
	if (mode == PW_DICT_PICK)
		fprintf(f, "Password: %08lX\n", w & 0xFFFFFFFFUL);
	if (fclose(f) != 0) r = -1;
	return r;
}

/* The password this copy gets, and recorded in the dictionary if new.
 * 0: use `*w` (0 = none: legacy); -1: it could not be saved or made
 * (nothing may be written).  Previous with none yet, and a dict choice no
 * longer in the file, make a new one (own), as the manual says. */
static int pw_pick(int mode, unsigned long chosen, const char *dict,
	const char *rnd, unsigned long *w, int *made)
{
	*w = 0;
	*made = 0;
	if (mode == PW_LEGACY) return 0;
	if (mode == PW_PREVIOUS && pw_previous(dict, w)) return 0;
	if (mode == PW_DICT_PICK && chosen != 0 && pw_has(dict, chosen)) {
		*w = chosen;
		return 0;
	}
	if (pw_random(dict, rnd, w) < 0) return -1;
	if (pw_record(dict, *w, 1) < 0) return -1;
	*made = 1;
	return 0;
}

#endif
