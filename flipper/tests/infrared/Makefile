# make irtest (spec Phase 7, AC7.1; issue #389): libir (src/lib/infrared,
# the Unleashed infrared encoders and decoders, and irfile.c) on the PC:
# every stock .irtest vector (decoder, encoder, encoder -> decoder), .ir
# files read and written back byte for byte, the TV universal database
# streamed and round-tripped, bad input skipped; under AddressSanitizer
# and UBSan.
#
#	make test	build irtest with the host compiler and run it
#
# Needs a host C compiler only.  Run by `make irtest` (flipper/Makefile)
# and by tests/host, which the `build` CI job runs.
MROOT	:= ../../..
L	:= $(MROOT)/src/lib/infrared
CC	?= cc
# -fno-sanitize=alignment: Unleashed reads timings through casts at any
# byte; the Cortex-M4 does unaligned LDR (as in tests/subghz).
# -fno-sanitize=shift-base: the NEC encoder shifts a byte into the sign bit
# of an int (0xFF << 24), which GCC defines on the Flipper as stock expects.
SAN	:= -fsanitize=address,undefined -fno-sanitize=alignment,shift-base \
	   -fno-sanitize-recover=undefined
CFLAGS	:= -std=gnu11 -O1 -g -Wall -Wno-format $(SAN)
INC	:= -I$(L) -I$(L)/shim -I$(L)/encoder_decoder -include irshim.h
SRC	:= $(shell find $(L) -name '*.c')
HDR	:= $(shell find $(L) -name '*.h')
B	:= build

test: $(B)/irtest
	./$(B)/irtest vectors

$(B)/irtest: irtest.c $(SRC) $(HDR) Makefile
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(INC) -o $@ irtest.c $(SRC)

clean:
	rm -rf $(B)

.PHONY: test clean
