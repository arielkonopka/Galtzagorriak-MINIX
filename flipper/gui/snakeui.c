/* SPDX-License-Identifier: GPL-3.0-or-later
 * snakeui - Snake, a small window app (spec Phase 7, K.8; issue #424).
 *
 * The game loop is src/lib/snake (host-tested, pure logic); this file is
 * only the window: a pad window (K.8) whose keys arrive as GM_KEY, the
 * board drawn as text lines, one character a cell.  A tick every PERIOD
 * ms advances the snake; the arrows turn it, OK restarts after a game
 * over, Back leaves.  No files, no flash writes; the state is .bss.
 */
#include <sys/types.h>
#include <sys/times.h>
#include <string.h>
#include <unistd.h>
#include <gm.h>
#include "snakecore.h"

#define PERIOD 220		/* ms between ticks (stock snake is ~250) */

static Snake game;
static unsigned long seed;	/* xorshift state for the fruit */

/* A cheap pseudo-random source seeded from the boot clock, in [0, n). */
static uint16_t rng(void *ctx, uint16_t n)
{
	(void) ctx;
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return (uint16_t)(seed % (n ? n : 1));
}

static void seed_init(void)
{
	struct tms t;

	seed = (unsigned long) times(&t);
	seed ^= (unsigned long) getpid() << 16;
	if (seed == 0) seed = 0x1234567u;
}

/* Append the decimal of n (0..999) to p; return the new end. */
static char *putnum(char *p, int n)
{
	if (n >= 100) *p++ = (char)('0' + n / 100);
	if (n >= 10)  *p++ = (char)('0' + (n / 10) % 10);
	*p++ = (char)('0' + n % 10);
	return p;
}

/* Draw the board and the score as the pad window's lines. */
static void draw(void)
{
	char line[SNAKE_W + 1], sc[32], *p;
	int x, y;

	gm_win_pad("snake", "Snake", 0);
	for (y = 0; y < SNAKE_H; y++) {
		for (x = 0; x < SNAKE_W; x++) {
			char c = ' ';

			if (x == game.body[0].x && y == game.body[0].y)
				c = '@';		/* head */
			else if (snake_at(&game, x, y))
				c = 'o';		/* body */
			else if (x == game.fruit.x && y == game.fruit.y)
				c = '*';		/* fruit */
			line[x] = c;
		}
		line[SNAKE_W] = 0;
		gm_line(line);
	}
	p = sc;
	if (game.state == SNAKE_OVER) { memcpy(p, "Game over  ", 11); p += 11; }
	memcpy(p, "Score ", 6); p += 6;
	p = putnum(p, snake_score(&game));
	if (game.state == SNAKE_OVER) { memcpy(p, "  OK", 4); p += 4; }
	*p = 0;
	gm_line(sc);
	if (gm_show() < 0) _exit(0);		/* the desktop is gone */
}

/* An arrow press turns the snake; OK restarts when it is over. */
static void key(const char *k, const char *what)
{
	if (strcmp(what, "press") != 0 && strcmp(what, "repeat") != 0)
		return;
	if (game.state == SNAKE_OVER) {
		if (strcmp(k, "ok") == 0) {
			snake_init(&game, rng, 0);
			draw();
		}
		return;
	}
	if (strcmp(k, "up") == 0)	snake_turn(&game, SNAKE_UP);
	else if (strcmp(k, "down") == 0)  snake_turn(&game, SNAKE_DOWN);
	else if (strcmp(k, "left") == 0)  snake_turn(&game, SNAKE_LEFT);
	else if (strcmp(k, "right") == 0) snake_turn(&game, SNAKE_RIGHT);
}

int main(int argc, char **argv)
{
	char buf[160], ev[160], *f[GM_FIELDS];

	(void) argc; (void) argv;
	if (gm_init(buf, sizeof buf) < 0) {
		(void) write(2, "snakeui: start Snake from the launcher\n", 39);
		return 1;
	}
	seed_init();
	snake_init(&game, rng, 0);
	draw();
	for (;;) {
		if (gm_wait(ev, PERIOD) == 0) {	/* timeout: advance a tick */
			if (game.state != SNAKE_OVER) {
				snake_step(&game);
				draw();
			}
			continue;
		}
		switch (gm_event(ev, sizeof ev, f, GM_FIELDS)) {
		case GM_KEY:
			if (f[1] != 0 && f[2] != 0) key(f[1], f[2]);
			break;
		case GM_REDRAW:
		case GM_PAGE:
			draw();
			break;
		case GM_BACK:
		case GM_CLOSE:
			return 0;
		}
	}
}
