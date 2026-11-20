# make -C tests/radio test (task T30a): libradio (src/lib/radio) on the PC
# under AddressSanitizer and UBSan, and tools/radio-fetch.py offline with a
# fake release tree (fetchtest.sh).  `make online` also checks the pinned
# STM32CubeWB tag over HTTPS (network needed).
MROOT	:= ../../..
L	:= $(MROOT)/src/lib/radio
CC	?= cc
SAN	:= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS	:= -std=gnu11 -O1 -g -Wall -Wextra $(SAN)
B	:= build

test: $(B)/radiotest $(B)/radio
	./$(B)/radiotest
	sh ./fetchtest.sh $(MROOT)/flipper/tools/radio-fetch.py
	sh ./clitest.sh ./$(B)/radio

online:
	python3 -I $(MROOT)/flipper/tools/radio-fetch.py list --check

$(B)/radiotest: radiotest.c $(L)/radio.c $(L)/radio.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -I$(L) -o $@ radiotest.c $(L)/radio.c

clean:
	rm -rf $(B)

.PHONY: test online clean

# The command itself on the PC (paths under $RADIO_ROOT): clitest.sh.
$(B)/radio: $(MROOT)/flipper/cmds/radio.c $(L)/radio.c $(L)/radio.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -DHOST -I$(L) -I$(MROOT)/src/lib/modem -o $@ \
	  $(MROOT)/flipper/cmds/radio.c $(L)/radio.c $(MROOT)/src/lib/modem/rgsha.c
