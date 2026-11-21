# RAW .sub editor: host unit tests (issue #424 item 2).
#   make test    build and run them with the host compiler
CC	?= cc
L	= ../../../src/lib/subghz
CFLAGS	?= -O2 -Wall -Wextra -std=c99 -fsanitize=address,undefined -g -I$(L)

test: test_sgedit
	./test_sgedit

test_sgedit: test_sgedit.c $(L)/sgedit.c $(L)/sgedit.h
	$(CC) $(CFLAGS) -o $@ test_sgedit.c $(L)/sgedit.c

clean:
	-rm -f test_sgedit

.PHONY: test clean
