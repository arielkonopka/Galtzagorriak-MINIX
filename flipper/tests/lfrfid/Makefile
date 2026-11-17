# make lfrfidtest (spec R9.0.2, AC9.0.1): liblfrfid (src/lib/lfrfid, the
# Unleashed LF RFID protocols) on the PC: every protocol's encoder ->
# synthetic reader edges -> all decoders, and render_data against render.txt.
#
#	make test	build lfrfidtest with the host compiler and run it
#	make golden	rewrite render.txt with the C library's printf (as the
#			stock firmware's newlib); look at the diff before keeping it
#
# Needs a host C compiler only.  Run by `make lfrfidtest` (flipper/Makefile)
# and by tests/host, which the `build` CI job runs.
MROOT	:= ../../..
L	:= $(MROOT)/src/lib/lfrfid
CC	?= cc
# -fno-sanitize=alignment: Unleashed reads preambles as *(uint32_t*) at
# any byte (Indala26, Nexwatch, ...); the Cortex-M4 does unaligned LDR and
# MINIX leaves CCR.UNALIGN_TRP clear, as the stock firmware does.
# -fno-sanitize=shift-base: it shifts uint8_t (int) into the sign bit
# (EM4100 DEZ 10: data[1] << 24), which GCC defines (its manual: signed
# << is not treated as undefined).
SAN	:= -fsanitize=address,undefined -fno-sanitize=alignment,shift-base \
	   -fno-sanitize-recover=undefined
# -Wno-format: Unleashed passes uint32_t to %lu (right on the Flipper,
# where uint32_t is unsigned long); the shim's printf reads %l as 32 bits.
CFLAGS	:= -std=gnu11 -O2 -g -Wall -Wno-format $(SAN)
INC	:= -I$(L) -I$(L)/shim
SRC	:= $(shell find $(L) -name '*.c')
HDR	:= $(shell find $(L) -name '*.h')
B	:= build

test: $(B)/lfrfidtest
	./$(B)/lfrfidtest render.txt

$(B)/lfrfidtest: lfrfidtest.c $(SRC) $(HDR)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(INC) -o $@ lfrfidtest.c $(SRC) -lm

$(B)/lfrfidgolden: lfrfidtest.c $(SRC) $(HDR)
	@mkdir -p $(B)
	$(CC) -std=gnu11 -O2 -w -DLFRFID_LIBC_PRINTF $(INC) -o $@ lfrfidtest.c $(SRC) -lm

golden: $(B)/lfrfidgolden
	./$(B)/lfrfidgolden -g > render.txt

clean:
	rm -rf $(B)

.PHONY: test golden clean
