# libu2f (issue #420): the U2F messages and U2FHID framing on the PC, with
# a fake P-256 and with the real one (micro-ecc, ../../../src/lib/u2f/uecc),
# under ASan and UBSan.
#	make test
MROOT	:= ../../..
U2F	:= $(MROOT)/src/lib/u2f
CC	?= cc
CFLAGS	:= -O1 -g -Wall -Wextra -std=gnu99 -fsanitize=address,undefined \
	   -fno-sanitize-recover=undefined -I$(U2F)

test: u2ftest
	./u2ftest

u2ftest: u2ftest.c $(U2F)/u2f.c $(U2F)/sha256.c $(U2F)/p256.c $(U2F)/u2fstore.c $(U2F)/u2f.h \
		$(U2F)/sha256.h $(U2F)/p256.h $(U2F)/u2fstore.h $(wildcard $(U2F)/uecc/*)
	$(CC) $(CFLAGS) -o $@ u2ftest.c $(U2F)/u2f.c $(U2F)/sha256.c $(U2F)/p256.c \
	   $(U2F)/u2fstore.c

clean:
	-rm -f u2ftest

.PHONY: test clean
