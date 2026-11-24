# The Music Player's song parser (issue #420): host unit tests.
#   make test     build and run them with the host compiler
CC	?= cc
CFLAGS	?= -O1 -g -Wall -Wextra -std=c99 -fsanitize=address,undefined \
	   -fno-sanitize-recover=undefined

test: test_fmf
	./test_fmf

test_fmf: test_fmf.c fmf.c fmf.h
	$(CC) $(CFLAGS) -o $@ test_fmf.c fmf.c

clean:
	-rm -f test_fmf

.PHONY: test clean
