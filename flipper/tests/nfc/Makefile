# make nfctest (spec Phase 14, AC14.1/AC14.2; issue #387): libnfc
# (src/lib/libnfc) on the PC - Crypto-1, the streaming key dictionary, the
# MIFARE Classic reader authentication and the dictionary attack against a
# software card model, all under AddressSanitizer and UBSan.
#
#	make test	build nfctest with the host compiler and run it
#
# Needs a host C compiler only.  Run by tests/host, which the `build` CI
# job runs.
MROOT	:= ../../..
L	:= $(MROOT)/src/lib/libnfc
CC	?= cc
SAN	:= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS	:= -std=gnu11 -O1 -g -Wall $(SAN)
INC	:= -I$(L)
SRC	:= $(L)/crypto1.c $(L)/mf_classic.c $(L)/mf_dict.c $(L)/mf_attack.c \
	   $(L)/nfc_file.c
HDR	:= $(wildcard $(L)/*.h)
B	:= build

test: $(B)/nfctest
	cd . && ./$(B)/nfctest

$(B)/nfctest: nfctest.c $(SRC) $(HDR)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(INC) -o $@ nfctest.c $(SRC)

clean:
	rm -rf $(B)

.PHONY: test clean
