# Host tests for mined (T8): mined built for the PC with AddressSanitizer,
# driven in a pseudo-terminal by test_mined.py.
#	make -C src/commands/mined/hosttest test
CC	?= cc
CFLAGS	:= -std=gnu89 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	   -fsigned-char -D_POSIX_SOURCE -D_DEFAULT_SOURCE -Ishim -w

mined.host: ../mined1.c ../mined2.c ../mined.h shim/minix/config.h
	$(CC) $(CFLAGS) ../mined1.c ../mined2.c -o $@

.PHONY: test clean
test: mined.host
	python3 -I test_mined.py ./mined.host

clean:
	rm -f mined.host
