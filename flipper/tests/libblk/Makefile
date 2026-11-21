# libblk (spec 12h, R12.38): host unit tests against a fake block space
# (blk_* in test_libblk.c, with read and write counters).
#   make test    build and run them with the host compiler
CC	?= cc
MROOT	= ../../..
CFLAGS	?= -O1 -Wall -Wextra -std=c99 -fsanitize=address,undefined \
	   -fno-sanitize-recover=undefined -g
INC	= -idirafter $(MROOT)/include

test: test_libblk
	./test_libblk

test_libblk: test_libblk.c $(MROOT)/src/lib/blk/libblk.c \
		$(MROOT)/include/libblk.h $(MROOT)/include/blk.h \
		$(MROOT)/include/minix/blk.h
	$(CC) $(CFLAGS) $(INC) -o $@ test_libblk.c $(MROOT)/src/lib/blk/libblk.c

clean:
	-rm -f test_libblk

.PHONY: test clean
