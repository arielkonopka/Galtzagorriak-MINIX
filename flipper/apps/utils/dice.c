/* SPDX-License-Identifier: GPL-3.0-or-later
 * dice - the Dice app (Unleashed's "Dice") on Galtzagorriak MINIX (issue
 * #420), a window app (spec K.8).  Pick the die (coin, d4 .. d100) and how
 * many (1-9), OK on Roll; the result line shows every die and the sum.
 * The numbers come from /dev/random (the hardware generator on the
 * Flipper), without modulo bias: a byte that would favour the low faces
 * is thrown away and another one read.
 *
 *	Dice:nobody:@game:=/usr/bin/dice	(/etc/apps)
 *
 * RAM: no stdio, no malloc, no signal handler: no MM gap (K.8).
 */
#include <sys/types.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "gm.h"

#define NDIE	9
static const unsigned char sides[] = { 2, 4, 6, 8, 10, 12, 20, 100 };
#define OPTS	"Coin|d4|d6|d8|d10|d12|d20|d100"

static short die = 2, count = 1, rolls;
static unsigned char got[NDIE];

static char *num(char *t, long v)		/* t[12] */
{
	char *p = t + 11;

	*p = 0;
	do *--p = (char) ('0' + v % 10); while ((v /= 10) > 0);
	return p;
}

/* 1..n from the random device, -1 when it cannot be read. */
static int roll(int fd, int n)
{
	unsigned char c;
	int lim = 256 - 256 % n;

	do {
		if (read(fd, &c, 1) != 1) return -1;
	} while (c >= lim);
	return c % n + 1;
}

static void w_main(void)
{
	char l[48], t[12];
	int i, sum = 0, coin = sides[die] == 2;

	gm_win_live("dice", "list", "Dice", 1);
	gm_enum("die", "Die", die, OPTS);
	gm_int("n", "Count", 1, NDIE, 1, count, "");
	gm_action("roll", "Roll");
	if (rolls > 0) {
		l[0] = 0;
		for (i = 0; i < count; i++) {
			if (coin) strcat(l, got[i] == 1 ? "H " : "T ");
			else {
				strcat(l, num(t, got[i]));
				strcat(l, " ");
			}
			sum += got[i];
			if (strlen(l) > 30) break;
		}
		gm_line(l);
		if (!coin && count > 1) {
			strcpy(l, "Sum ");
			strcat(l, num(t, sum));
			gm_line(l);
		}
	}
	gm_show();
}

int main(int argc, char **argv)
{
	char buf[128], ev[128], *f[GM_FIELDS], *k, *v;
	static const char use[] = "dice: start Dice from the launcher\n";
	int i, r, fd;

	(void) argv;
	if (argc != 1 || gm_init(buf, sizeof buf) < 0) {
		(void) write(2, use, sizeof use - 1);
		return 1;
	}
	w_main();
	for (;;) switch (gm_event(ev, sizeof ev, f, GM_FIELDS)) {
	case GM_SUBMIT:
		for (i = 2; i < GM_FIELDS && f[i] != NULL; i++) {
			k = gm_kv(f[i], &v);
			r = atoi(v);
			if (strcmp(k, "die") == 0 && r >= 0
			    && r < (int) sizeof sides)
				die = (short) r;
			else if (strcmp(k, "n") == 0 && r >= 1 && r <= NDIE)
				count = (short) r;
		}
		break;
	case GM_OK:
		if (f[2] == NULL || strcmp(f[2], "roll") != 0) {
			gm_busy(0);
			break;
		}
		if ((fd = open("/dev/random", O_RDONLY)) < 0) {
			gm_busy(0);
			break;
		}
		for (i = 0; i < count; i++)
			if ((r = roll(fd, sides[die])) > 0)
				got[i] = (unsigned char) r;
		close(fd);
		rolls++;
		w_main();
		break;
	case GM_REDRAW:
		w_main();
		break;
	case GM_CLOSE:
	case GM_BACK:
		return 0;
	}
}
