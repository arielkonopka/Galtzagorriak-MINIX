# libapp (spec F.2, R7b.4): host tests.
#   make test     build test_app with the host compiler and run it
# The MINIX build (libapp.a, timed, alert) is in ../../minix.mk.
CC	?= cc
SAN	:= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS	?= -O1 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 $(SAN)
SRC	:= src/tspec.c src/io.c src/tune.c src/wav.c
B	:= build

test: $(B)/test_app
	cd $(B) && ./test_app

$(B)/test_app: test_app.c $(SRC) include/app.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -DAPP_ROOT='"root"' -Iinclude -o $@ test_app.c $(SRC)

clean:
	-rm -rf $(B)

.PHONY: test clean
