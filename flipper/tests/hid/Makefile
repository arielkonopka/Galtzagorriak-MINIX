# make hidtest (spec R11.3.2): libhid (src/lib/hid) on the PC: the
# built-in US layout against the stock en-US.kl, the layout size check,
# all printable ASCII typed in US and DE and read back through a reverse
# table, UTF-8 decoding and the report builders, under AddressSanitizer
# and UBSan.
#
# ducktest (spec R11.3.3): the Bad USB script engine
# (flipper/apps/badusb/ducky.c): golden reports, badusb -n's errors, a stop,
# and a fuzz test of random scripts (spec 11.3 Security).
#
#	make test	build hidtest and ducktest with the host compiler, run them
#
# Needs a host C compiler only.  Run by `make hidtest` (flipper/Makefile)
# and by tests/host, which the `build` CI job runs.
MROOT	:= ../../..
L	:= $(MROOT)/src/lib/hid
BU	:= $(MROOT)/flipper/apps/badusb
CC	?= cc
SAN	:= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS	:= -std=gnu11 -O2 -g -Wall -Wextra $(SAN)
B	:= build

test: $(B)/hidtest $(B)/ducktest
	./$(B)/hidtest $(L)/layouts
	./$(B)/ducktest $(L)/layouts

$(B)/hidtest: hidtest.c $(L)/hid.c $(L)/libhid.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -I$(L) -o $@ hidtest.c $(L)/hid.c

# <minix/hid.h> from the tree, after the host's own headers
$(B)/ducktest: ducktest.c $(BU)/ducky.c $(BU)/ducky.h $(L)/hid.c $(L)/libhid.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -I$(L) -I$(BU) -idirafter $(MROOT)/include -o $@ \
	    ducktest.c $(BU)/ducky.c $(L)/hid.c

clean:
	rm -rf $(B)

.PHONY: test clean
