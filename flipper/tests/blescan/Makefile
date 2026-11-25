# make -C tests/blescan test (issue #388, the BLE scanner): libblescan
# (src/lib/blescan) on the PC under AddressSanitizer and UBSan with golden
# HCI frames, and the `blescan` command on a recorded stream (clitest.sh).
MROOT	:= ../../..
L	:= $(MROOT)/src/lib/blescan
CC	?= cc
SAN	:= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS	:= -std=gnu11 -O1 -g -Wall -Wextra $(SAN)
B	:= build

test: $(B)/bstest $(B)/blescan
	./$(B)/bstest
	sh ./clitest.sh ./$(B)/blescan

$(B)/bstest: bstest.c $(L)/blescan.c $(L)/blescan.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -I$(L) -o $@ bstest.c $(L)/blescan.c

$(B)/blescan: $(MROOT)/flipper/cmds/blescan.c $(L)/blescan.c $(L)/blescan.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -DHOST -I$(L) -o $@ $(MROOT)/flipper/cmds/blescan.c $(L)/blescan.c

clean:
	rm -rf $(B)

.PHONY: test clean
