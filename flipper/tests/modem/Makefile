# make modemtest (spec section O, ACO.1, ACO.2): libmodem (src/lib/modem)
# on the PC: frames, the ARQ stream through a lossy channel, the carousel,
# the RFID line code P4, the CC1101 presets, the legal band check, the
# duty-cycle counter and a fuzz run, under AddressSanitizer and UBSan.
#
#	make test	build modemtest with the host compiler and run it
#
# Needs a host C compiler only.  Run by `make modemtest` (flipper/Makefile)
# and by tests/host, which the `build` CI job runs.
MROOT	:= ../../..
L	:= $(MROOT)/src/lib/modem
CC	?= cc
SAN	:= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS	:= -std=gnu11 -O2 -g -Wall -Wextra $(SAN)
SRC	:= $(wildcard $(L)/*.c)
B	:= build

test: $(B)/modemtest
	./$(B)/modemtest

$(B)/modemtest: modemtest.c $(SRC) $(L)/modem.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -I$(L) -o $@ modemtest.c $(SRC)

clean:
	rm -rf $(B)

.PHONY: test clean
