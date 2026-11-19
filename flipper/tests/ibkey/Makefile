# make ibkeytest (spec R6.7 part 3b): libibkey (src/lib/ibkey, the
# Unleashed Cyfral and Metakom iButton protocols) on the PC: every key's
# pulse list -> the line as a reader sees it (any start, jitter) -> the
# decoders, and garbage that must not decode.
#
#	make test	build ibkeytest with the host compiler and run it
#
# Needs a host C compiler only.  Run by `make ibkeytest` (flipper/Makefile)
# and by tests/host, which the `build` CI job runs.
MROOT	:= ../../..
L	:= $(MROOT)/src/lib/ibkey
CC	?= cc
SAN	:= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS	:= -std=gnu89 -O2 -g -Wall -Wextra -Wno-old-style-definition $(SAN)
B	:= build

test: $(B)/ibkeytest
	./$(B)/ibkeytest

$(B)/ibkeytest: ibkeytest.c $(L)/ibkey.c $(L)/ibkey.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -I$(L) -o $@ ibkeytest.c $(L)/ibkey.c

clean:
	rm -rf $(B)

.PHONY: test clean
