# make nfctest / mfkeytest (spec Phase 14, AC14.1/AC14.2; issue #387):
# libnfc (src/lib/libnfc) on the PC - Crypto-1, the streaming key dictionary,
# the MIFARE Classic reader authentication and the dictionary attack against a
# software card model (nfctest); and the vendored MIFARE Classic key-recovery
# engine (src/lib/nfc/mfkey) driven through src/lib/libnfc/mfkey_host,
# recovering a planted key from a stock-format .mfkey32.log and reporting the
# time per key (mfkeytest).  All under AddressSanitizer and UBSan.
#
#	make test	build both and run them
#
# Needs a host C compiler only.  Run by tests/host, which the `build` CI
# job runs.
MROOT	:= ../../..
L	:= $(MROOT)/src/lib/libnfc
V	:= $(MROOT)/src/lib/nfc
CC	?= cc
SAN	:= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS	:= -std=gnu11 -O1 -g -Wall $(SAN)
INC	:= -I$(L)
B	:= build

# nfctest: the part-1 hand-written libnfc.
NFC_SRC	:= $(L)/crypto1.c $(L)/mf_classic.c $(L)/mf_dict.c $(L)/mf_attack.c \
	   $(L)/nfc_file.c $(L)/iso14443_3a.c $(L)/mf_ultralight.c \
	   $(L)/nfc_poller.c $(L)/nfc_sha1.c
NFC_HDR	:= $(wildcard $(L)/*.h)

# mfkeytest: the vendored recovery engine, the libnfc host driver, and the
# minimal furi/mf_classic shim (mfkey_shim) the vendored files compile
# against.  The shim dir comes first so the stock header names resolve there;
# the vendored mfkey dir supplies crypto1.h/mfkey.h via <> includes.
# The vendored Crypto-1 and the MSB-table attack rely on two things stock
# firmware ships as-is: shifts of signed values (e.g. filter() << 31) and
# deliberate unaligned 32-bit reads of the packed 24-bit state buckets (the
# struct Msb carries 4 bytes of tail padding exactly so these are safe on the
# Cortex-M).  Keep ASan and the rest of UBSan - it still catches real
# out-of-bounds in the untouched vendored files - and drop only those two
# checks.
MFKEY_CFLAGS := $(CFLAGS) -fno-sanitize=shift -fno-sanitize=alignment
MFKEY_INC := -I$(L)/mfkey_shim -I$(V)/mfkey -I$(L)
MFKEY_SRC := $(V)/mfkey/crypto1.c $(V)/mfkey/mfkey_attack.c \
	     $(V)/mfkey/mfkey_recovery.c $(V)/mfkey/mfkey_bs_verify.c \
	     $(V)/mfkey/mfkey_state_expansion.c $(V)/mfkey/mfkey_batch_prelude.c \
	     $(L)/mfkey_host.c
MFKEY_HDR := $(L)/mfkey_host.h $(wildcard $(L)/mfkey_shim/*.h)

test: $(B)/nfctest $(B)/mfkeytest prototest cardtest p5test cryptotest
	cd . && ./$(B)/nfctest
	cd . && ./$(B)/mfkeytest

$(B)/nfctest: nfctest.c $(NFC_SRC) $(NFC_HDR)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(INC) -o $@ nfctest.c $(NFC_SRC)

$(B)/mfkeytest: $(V)/mfkeytest.c $(MFKEY_SRC) $(MFKEY_HDR)
	@mkdir -p $(B)
	$(CC) $(MFKEY_CFLAGS) $(MFKEY_INC) -o $@ $(V)/mfkeytest.c $(MFKEY_SRC)

include proto.mk

clean:
	rm -rf $(B)

.PHONY: test clean
