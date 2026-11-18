# make hidtest (spec R11.3.2): libhid (src/lib/hid) on the PC: the
# built-in US layout against the stock en-US.kl, the layout size check,
# all printable ASCII typed in US and DE and read back through a reverse
# table, UTF-8 decoding and the report builders, under AddressSanitizer
# and UBSan.
#
#	make test	build hidtest with the host compiler and run it
#
# Needs a host C compiler only.  Run by `make hidtest` (flipper/Makefile)
# and by tests/host, which the `build` CI job runs.
MROOT	:= ../../..
L	:= $(MROOT)/src/lib/hid
CC	?= cc
SAN	:= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS	:= -std=gnu11 -O2 -g -Wall -Wextra $(SAN)
B	:= build

test: $(B)/hidtest
	./$(B)/hidtest $(L)/layouts

$(B)/hidtest: hidtest.c $(L)/hid.c $(L)/libhid.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -I$(L) -o $@ hidtest.c $(L)/hid.c

clean:
	rm -rf $(B)

.PHONY: test clean
