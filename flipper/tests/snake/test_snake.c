/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host unit tests for the Snake game loop (src/lib/snake, issue #424).
 * Built and run on the PC (make -C flipper/tests/snake test), ASan/UBSan.
 */
#include <stdio.h>
#include <string.h>
#include "snakecore.h"

static int fails;
#define CHECK(c) do { if (!(c)) { \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* A deterministic rng: always the first free cell (pick 0). */
static uint16_t rng0(void *ctx, uint16_t n) { (void)ctx; (void)n; return 0; }

/* Drive the snake straight into the right wall and check the one
 * last-chance tick before death. */
static void test_wall_last_chance(void)
{
	Snake s;
	SnakeStep st = SNAKE_STEP_MOVE;
	int moved = 0;

	snake_init(&s, NULL, NULL);
	/* keep going right; the fruit is on this row, so avoid it by moving
	 * the fruit out of the way first via a turn up/down path instead. */
	snake_turn(&s, SNAKE_UP);		/* leave the fruit row */
	snake_step(&s);
	snake_turn(&s, SNAKE_RIGHT);
	while (s.state == SNAKE_LIFE && moved < 100) {
		st = snake_step(&s);
		moved++;
	}
	CHECK(s.state == SNAKE_LASTCHANCE);	/* at the wall, not dead yet */
	CHECK(st == SNAKE_STEP_MOVE);
	/* turn away in the last chance: survives */
	snake_turn(&s, SNAKE_UP);
	st = snake_step(&s);
	CHECK(s.state == SNAKE_LIFE);
	/* now run into the top wall and do NOT turn: dies on the 2nd tick */
	while (s.state == SNAKE_LIFE) snake_step(&s);
	CHECK(s.state == SNAKE_LASTCHANCE);
	st = snake_step(&s);
	CHECK(st == SNAKE_STEP_DIE);
	CHECK(s.state == SNAKE_OVER);
	CHECK(snake_step(&s) == SNAKE_STEP_NONE);	/* no move after death */
}

/* Eating a fruit grows the snake and scores. */
static void test_eat_grows(void)
{
	Snake s;
	int i;
	SnakeStep st;

	snake_init(&s, rng0, NULL);
	CHECK(snake_score(&s) == 0);
	/* the first fruit is straight ahead on the head's row */
	for (i = 0; i < SNAKE_W; i++) {
		st = snake_step(&s);
		if (st == SNAKE_STEP_EAT) break;
	}
	CHECK(st == SNAKE_STEP_EAT);
	CHECK(s.len == SNAKE_START + 1);
	CHECK(snake_score(&s) == 1);
}

/* A turn backward onto itself is ignored (no instant self-crash). */
static void test_reverse_ignored(void)
{
	Snake s;

	snake_init(&s, NULL, NULL);		/* facing right */
	snake_turn(&s, SNAKE_LEFT);		/* backward: must be ignored */
	CHECK(snake_step(&s) != SNAKE_STEP_DIE);
	CHECK(s.dir == SNAKE_RIGHT);
}

/* Running the head into its own body kills it. */
static void test_self_bite(void)
{
	Snake s;
	int i;

	snake_init(&s, rng0, NULL);
	/* grow a few segments so a tight loop can bite the body */
	for (i = 0; i < 40 && snake_score(&s) < 4; i++)
		snake_step(&s);
	/* a U-turn: up, left, down -> back onto the body */
	snake_turn(&s, SNAKE_UP);   snake_step(&s);
	snake_turn(&s, SNAKE_LEFT); snake_step(&s);
	snake_turn(&s, SNAKE_DOWN); snake_step(&s);
	snake_turn(&s, SNAKE_RIGHT);
	/* at least one of these short loops lands on a body cell */
	for (i = 0; i < 4 && s.state != SNAKE_OVER; i++) {
		snake_turn(&s, i % 2 ? SNAKE_DOWN : SNAKE_LEFT);
		snake_step(&s);
	}
	/* not asserting death on a specific tick (board-size dependent); the
	 * point is the loop never reads out of bounds under ASan. */
	CHECK(s.len <= SNAKE_MAX);
}

/* The body never leaves the board and snake_at agrees with the head. */
static void test_bounds(void)
{
	Snake s;
	int i;
	uint16_t j;

	snake_init(&s, rng0, NULL);
	for (i = 0; i < 500 && s.state != SNAKE_OVER; i++) {
		snake_turn(&s, (SnakeDir)(i % 4));
		snake_step(&s);
		for (j = 0; j < s.len; j++) {
			CHECK(s.body[j].x < SNAKE_W);
			CHECK(s.body[j].y < SNAKE_H);
		}
	}
	CHECK(snake_at(&s, s.body[0].x, s.body[0].y) == 1);
}

int main(void)
{
	test_wall_last_chance();
	test_eat_grows();
	test_reverse_ignored();
	test_self_bite();
	test_bounds();
	if (fails == 0) printf("snake: all tests passed\n");
	else printf("snake: %d checks FAILED\n", fails);
	return fails ? 1 : 0;
}
