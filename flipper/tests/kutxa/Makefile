# make kutxatest (spec section R, ACR.1-ACR.10): libkutxa (src/lib/kutxa)
# and the `kutxa` command (src/commands/kutxa) on the PC, under
# AddressSanitizer and UBSan.
#
#	make test	build kutxatest and the host kutxa, run both tests
#
# The tests were written before the library (test first, 2026-11-18); until
# T34a adds src/lib/kutxa/*.c and src/commands/kutxa/kutxa.c this stops with
# "libkutxa is not written yet".  T34a adds it to tests/host (CI) and to
# `make kutxatest` in flipper/Makefile.
MROOT	:= ../../..
L	:= $(MROOT)/src/lib/kutxa
C	:= $(MROOT)/src/commands/kutxa
CC	?= cc
SAN	:= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS	:= -std=gnu11 -O2 -g -Wall -Wextra $(SAN)
SRC	:= $(wildcard $(L)/*.c)
B	:= build

test: have $(B)/kutxatest $(B)/kutxa
	./$(B)/kutxatest
	sh ./clitest.sh ./$(B)/kutxa

have:
	@test -n "$(SRC)" -a -f $(C)/kutxa.c || \
	  { echo "libkutxa is not written yet (spec section R, task T34a)"; exit 1; }

$(B)/kutxatest: kutxatest.c $(SRC) $(L)/kutxa.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -I$(L) -o $@ kutxatest.c $(SRC)

$(B)/kutxa: $(C)/kutxa.c $(SRC) $(L)/kutxa.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -I$(L) -o $@ $(C)/kutxa.c $(SRC)

# The test's own logic can be checked without the library: compile only.
syntax:
	$(CC) -std=gnu11 -Wall -Wextra -fsyntax-only -I$(L) kutxatest.c

clean:
	rm -rf $(B)

.PHONY: test have syntax clean
