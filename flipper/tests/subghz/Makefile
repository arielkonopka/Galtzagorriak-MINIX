# make subghztest (spec Phase 9, AC9.1; issue #302): libsubghz
# (src/lib/subghz, the Unleashed Sub-GHz protocols) on the PC: every
# RAW capture of the Unleashed unit tests (vectors/) decoded against
# decode.txt, the stock random-capture count, encoder -> decoder round
# trips for every protocol with an encoder, and a decoder fuzz, all under
# AddressSanitizer and UBSan.
#
#	make test	build subghztest with the host compiler and run it
#	make golden	rewrite decode.txt from the library; look at the diff
#			before keeping it
#
# Needs a host C compiler only.  Run by `make subghztest` (flipper/Makefile)
# and by tests/host, which the `build` CI job runs.
MROOT	:= ../../..
L	:= $(MROOT)/src/lib/subghz
CC	?= cc
# -fno-sanitize=alignment: Unleashed reads keys through casts at any byte;
# the Cortex-M4 does unaligned LDR (CCR.UNALIGN_TRP clear, as stock).
# -fno-sanitize=shift-base,shift-exponent: it shifts into the sign bit and
# by the bit count (data << 64 when a key is 64 bits), which GCC and the
# ARM compiler define on the Flipper (an LSL by 64 gives 0, as the code
# expects); the result is masked afterwards.
SAN	:= -fsanitize=address,undefined \
	   -fno-sanitize=alignment,shift-base,shift-exponent \
	   -fno-sanitize-recover=undefined
# -Wno-format: Unleashed passes uint32_t to %lX (right on the Flipper,
# where uint32_t is unsigned long); the shim's printf reads %l as 32 bits.
CFLAGS	:= -std=gnu11 -O1 -g -Wall -Wno-format $(SAN)
INC	:= -I$(L) -I$(L)/shim -I$(L)/subghz
SRC	:= $(shell find $(L) -name '*.c')
HDR	:= $(shell find $(L) -name '*.h')
B	:= build

test: $(B)/subghztest
	./$(B)/subghztest vectors decode.txt

$(B)/subghztest: subghztest.c $(SRC) $(HDR)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(INC) -o $@ subghztest.c $(SRC) -lm

golden: $(B)/subghztest
	./$(B)/subghztest -g vectors > decode.txt

clean:
	rm -rf $(B)

.PHONY: test golden clean
