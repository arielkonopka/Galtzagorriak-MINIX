/* SPDX-License-Identifier: GPL-3.0-or-later
 * snakecore - the Snake game loop, pure logic (Galtzagorriak MINIX, issue
 * #424 item 4, spec Phase 7 app port).
 *
 * No I/O, no malloc, no globals: the caller owns the state and renders it.
 * The mechanics follow the Unleashed firmware's snake_game.c (GPLv3): the
 * snake turns only when the new direction is orthogonal to the current
 * one, running into a wall gives one "last chance" tick to turn away, and
 * eating a fruit grows the snake by one.  The board here is a small cell
 * grid (not the stock app's pixel field), so it draws as text in a window
 * app; one cell is one character.
 *
 * RAM: the state is one struct (points array sized for the board), owned
 * by the caller; on the target it is in the app's .bss.  No flash writes.
 */
#ifndef SNAKE_SNAKECORE_H
#define SNAKE_SNAKECORE_H

#include <stdint.h>

#define SNAKE_W 16		/* board width in cells */
#define SNAKE_H 7		/* board height in cells (8th window row: score) */
#define SNAKE_MAX (SNAKE_W * SNAKE_H)	/* longest the snake can get */
#define SNAKE_START 4		/* body length at the start */

typedef struct {
	uint8_t x, y;
} SnakePoint;

typedef enum {
	SNAKE_UP = 0,		/* values used by the orthogonality test */
	SNAKE_RIGHT = 1,
	SNAKE_DOWN = 2,
	SNAKE_LEFT = 3
} SnakeDir;

typedef enum {
	SNAKE_LIFE = 0,		/* running */
	SNAKE_LASTCHANCE,	/* one tick against a wall, still turnable */
	SNAKE_OVER		/* dead; the board no longer changes */
} SnakeState;

/* What a step did, so the caller can beep or redraw. */
typedef enum {
	SNAKE_STEP_NONE = 0,	/* the game is already over */
	SNAKE_STEP_MOVE,	/* moved into an empty cell */
	SNAKE_STEP_EAT,		/* ate a fruit and grew */
	SNAKE_STEP_DIE		/* this step ended the game */
} SnakeStep;

/* A source of randomness for fruit placement, so tests are deterministic.
 * Returns a number in [0, n); n is the count of free cells (>= 1). */
typedef uint16_t (*snake_rng)(void *rngctx, uint16_t n);

typedef struct {
	SnakePoint body[SNAKE_MAX];	/* [0] is the head */
	uint16_t len;
	SnakeDir dir;			/* the heading taken this step */
	SnakeDir next;			/* the pending turn (may be ignored) */
	SnakePoint fruit;
	SnakeState state;
	snake_rng rng;
	void *rngctx;
} Snake;

/* Start a new game.  rng/rngctx place the fruit (rng may be NULL: the
 * fruit then sits at a fixed cell, useful for tests). */
void snake_init(Snake *s, snake_rng rng, void *rngctx);

/* Ask to turn: ignored if backward of the current heading, applied at the
 * next step if orthogonal (as the stock game does). */
void snake_turn(Snake *s, SnakeDir d);

/* Advance one tick; returns what happened. */
SnakeStep snake_step(Snake *s);

/* The score (fruits eaten so far). */
int snake_score(const Snake *s);

/* 1 if (x, y) holds a body segment (for rendering). */
int snake_at(const Snake *s, int x, int y);

#endif /* SNAKE_SNAKECORE_H */
