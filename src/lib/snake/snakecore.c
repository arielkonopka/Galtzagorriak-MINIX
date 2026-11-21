/* SPDX-License-Identifier: GPL-3.0-or-later
 * snakecore - the Snake game loop (issue #424 item 4).  See snakecore.h.
 *
 * Adapted from the Unleashed firmware's applications/system/snake_game
 * (GPLv3): the turn, last-chance and fruit rules are the same; the board
 * is a text cell grid instead of the pixel field, and the fruit's
 * randomness is injected so the host test is deterministic.
 */
#include "snakecore.h"

void snake_init(Snake *s, snake_rng rng, void *rngctx)
{
	int i;

	s->len = SNAKE_START;
	/* lay the body along row H/2, head at the right end, facing right */
	for (i = 0; i < SNAKE_START; i++) {
		s->body[i].x = (uint8_t)(SNAKE_START - i);
		s->body[i].y = SNAKE_H / 2;
	}
	s->dir = SNAKE_RIGHT;
	s->next = SNAKE_RIGHT;
	s->state = SNAKE_LIFE;
	s->rng = rng;
	s->rngctx = rngctx;
	/* first fruit: a fixed cell well ahead of the head */
	s->fruit.x = SNAKE_W - 4;
	s->fruit.y = SNAKE_H / 2;
}

void snake_turn(Snake *s, SnakeDir d)
{
	s->next = d;
}

int snake_at(const Snake *s, int x, int y)
{
	uint16_t i;

	for (i = 0; i < s->len; i++)
		if (s->body[i].x == x && s->body[i].y == y)
			return 1;
	return 0;
}

int snake_score(const Snake *s)
{
	return (int)s->len - SNAKE_START;
}

/* The heading for this step: take the pending turn only when it is
 * orthogonal to the current one (sum odd), else keep going straight. */
static SnakeDir turned(const Snake *s)
{
	int ortho = ((int)s->dir + (int)s->next) % 2 == 1;

	return ortho ? s->next : s->dir;
}

/* Count free cells and, with the rng, drop the next fruit on one. */
static void new_fruit(Snake *s)
{
	uint16_t free, pick, x, y;

	free = SNAKE_W * SNAKE_H - s->len;
	if (free == 0)
		return;			/* board full: handled as a win */
	pick = s->rng ? s->rng(s->rngctx, free) : 0;
	for (y = 0; y < SNAKE_H; y++)
		for (x = 0; x < SNAKE_W; x++) {
			if (snake_at(s, x, y))
				continue;
			if (pick == 0) {
				s->fruit.x = (uint8_t)x;
				s->fruit.y = (uint8_t)y;
				return;
			}
			pick--;
		}
}

SnakeStep snake_step(Snake *s)
{
	SnakePoint head;
	int wall, eat;
	uint16_t i;

	if (s->state == SNAKE_OVER)
		return SNAKE_STEP_NONE;

	s->dir = turned(s);
	head = s->body[0];
	switch (s->dir) {
	case SNAKE_UP:	  head.y--; break;
	case SNAKE_DOWN:  head.y++; break;
	case SNAKE_LEFT:  head.x--; break;
	case SNAKE_RIGHT: head.x++; break;
	}

	/* x/y are unsigned: a step off the left/top wraps to a big value, so
	 * one check on the high side catches every wall. */
	wall = head.x >= SNAKE_W || head.y >= SNAKE_H;
	if (wall) {
		if (s->state == SNAKE_LIFE) {
			s->state = SNAKE_LASTCHANCE;	/* one tick to turn away */
			return SNAKE_STEP_MOVE;
		}
		s->state = SNAKE_OVER;
		return SNAKE_STEP_DIE;
	}
	if (s->state == SNAKE_LASTCHANCE)
		s->state = SNAKE_LIFE;

	/* the tail cell is about to be vacated (unless we grow), so hitting
	 * it is not a crash; check every other segment */
	for (i = 0; i + 1 < s->len; i++)
		if (s->body[i].x == head.x && s->body[i].y == head.y) {
			s->state = SNAKE_OVER;
			return SNAKE_STEP_DIE;
		}

	eat = head.x == s->fruit.x && head.y == s->fruit.y;
	if (eat) {
		if (s->len >= SNAKE_MAX) {	/* filled the board: a win */
			s->state = SNAKE_OVER;
			return SNAKE_STEP_DIE;
		}
		s->len++;
	}
	/* shift the body down, put the head at the front */
	for (i = s->len - 1; i > 0; i--)
		s->body[i] = s->body[i - 1];
	s->body[0] = head;

	if (eat) {
		new_fruit(s);
		return SNAKE_STEP_EAT;
	}
	return SNAKE_STEP_MOVE;
}
