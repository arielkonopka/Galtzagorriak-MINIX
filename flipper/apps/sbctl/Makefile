# sbctl protocol core: host unit tests (spec task T6).
#   make test     build and run them with the host compiler
CC	?= cc
CFLAGS	?= -O2 -Wall -Wextra -std=c99

test: test_sbproto
	./test_sbproto

test_sbproto: test_sbproto.c sbproto.c sbproto.h
	$(CC) $(CFLAGS) -o $@ test_sbproto.c sbproto.c

clean:
	-rm -f test_sbproto

.PHONY: test clean
