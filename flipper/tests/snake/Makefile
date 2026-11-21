# Snake game loop: host unit tests (issue #424 item 4).
#   make test    build and run them with the host compiler
CC	?= cc
SNK	= ../../../src/lib/snake
CFLAGS	?= -O2 -Wall -Wextra -std=c99 -fsanitize=address,undefined -g -I$(SNK)

test: test_snake
	./test_snake

test_snake: test_snake.c $(SNK)/snakecore.c $(SNK)/snakecore.h
	$(CC) $(CFLAGS) -o $@ test_snake.c $(SNK)/snakecore.c

clean:
	-rm -f test_snake

.PHONY: test clean
