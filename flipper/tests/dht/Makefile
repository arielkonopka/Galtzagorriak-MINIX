# make dhttest (issue #358): libdht (src/lib/dht, the AM2301/DHT21/DHT22
# frame decoder) on the PC: every reading's frame -> the decoder, with
# jitter, any start of the capture and the host's own pulse in front, and
# garbage and bad checksums that must be refused.
#
#	make test	build dhttest with the host compiler and run it
#
# Needs a host C compiler only.  Run by `make dhttest` (flipper/Makefile)
# and by tests/host, which the `build` CI job runs.
MROOT	:= ../../..
L	:= $(MROOT)/src/lib/dht
CC	?= cc
SAN	:= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS	:= -std=gnu89 -O2 -g -Wall -Wextra $(SAN)
B	:= build

test: $(B)/dhttest
	./$(B)/dhttest

$(B)/dhttest: dhttest.c $(L)/dht.c $(L)/dht.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -I$(L) -o $@ dhttest.c $(L)/dht.c

clean:
	rm -rf $(B)

.PHONY: test clean
