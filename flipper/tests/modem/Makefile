# make modemtest (spec section P, ACP.1, ACP.2): libmodem (src/lib/modem)
# on the PC: frames, the ARQ stream through a lossy channel, the carousel,
# the RFID line code P4, the CC1101 presets, the legal band check, the
# radio region files (flipper/rootfs/usr/share/region, spec Q), the
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
CFLAGS	:= -std=gnu11 -O2 -g -Wall -Wextra $(SAN) \
	   -DREGION_DIR=\"$(abspath $(MROOT)/flipper/rootfs/usr/share/region)\" \
	   -DREG_DOCS=\"$(abspath $(MROOT)/docs/regulation)\"
SRC	:= $(wildcard $(L)/*.c $(L)/heatshrink/*.c)
B	:= build

test: $(B)/modemtest
	./$(B)/modemtest

REGS	:= $(wildcard $(MROOT)/flipper/rootfs/usr/share/region/*.reg)

# RQ.5: the shipped tables' SHA-256, made as for the Flipper (minix.mk)
$(B)/reghash.c: $(MROOT)/flipper/tools/mkreghash.py $(REGS)
	@mkdir -p $(B)
	python3 -I $< $@ $(REGS)

$(B)/modemtest: modemtest.c $(SRC) $(L)/modem.h $(L)/mdmz.h $(B)/reghash.c
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -I$(L) -o $@ modemtest.c $(SRC) $(B)/reghash.c

clean:
	rm -rf $(B)

.PHONY: test clean
