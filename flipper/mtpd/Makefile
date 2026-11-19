# mtpd on the PC (spec R10.4, T29a): the same sources as on the Flipper,
# built with the host compiler under AddressSanitizer and UBSan.
#
#	make -C flipper/mtpd test	tools/mtphost.py against mtpd on a pipe
#	make -C flipper/mtpd fuzz	the container parser under libFuzzer
#					(clang), FUZZTIME seconds (default 60;
#					AC10.4.6 asks for 3600 once per PR)
#
# The PC build takes names of up to 60 bytes, the V3 rule (spec O).
CC	?= cc
SAN	:= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS	:= -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter $(SAN) \
	   -DMTP_NAME_MAX=60
B	:= build
SRC	:= mtpcore.c mtpfs.c mtpfd.c
FUZZTIME ?= 60
CLANG	?= clang

all: test

$(B)/mtpd: mtpd.c $(SRC) mtp.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) mtpd.c $(SRC) -o $@

# The test makes its own storage root and removes it afterwards.
test: $(B)/mtpd
	rm -rf $(B)/root && mkdir -p $(B)/root
	python3 -I ../tools/mtphost.py test --spawn "$(B)/mtpd -r $(B)/root" \
	   --root $(B)/root --name-max 60 --record $(B)/seed.bin
	rm -rf $(B)/root

# read() and write() of the stream transport go to the fuzzer's input.
$(B)/fuzz: fuzz.c $(SRC) mtp.h
	@mkdir -p $(B)
	$(CLANG) -std=gnu11 -O1 -g -fsanitize=fuzzer,address,undefined \
	   -fno-sanitize-recover=undefined -DMTP_NAME_MAX=60 \
	   -Dread=fz_read -Dwrite=fz_write -c mtpfd.c -o $(B)/mtpfd-fz.o
	$(CLANG) -std=gnu11 -O1 -g -fsanitize=fuzzer,address,undefined \
	   -fno-sanitize-recover=undefined -DMTP_NAME_MAX=60 \
	   fuzz.c mtpcore.c mtpfs.c $(B)/mtpfd-fz.o -o $@

# The seed corpus: the stream of the mtphost test (make test), and a few
# hand-made containers (tools/mtphost.py seeds).
fuzz: $(B)/fuzz
	@mkdir -p $(B)/corpus
	@test -f $(B)/seed.bin || $(MAKE) test
	cp $(B)/seed.bin $(B)/corpus/
	python3 -I ../tools/mtphost.py seeds $(B)/corpus
	$(B)/fuzz -max_total_time=$(FUZZTIME) -timeout=10 -max_len=8192 \
	   -print_final_stats=1 $(B)/corpus

clean:
	rm -rf $(B)

.PHONY: all test fuzz clean
