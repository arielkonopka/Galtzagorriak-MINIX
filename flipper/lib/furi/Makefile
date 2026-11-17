# libfuri core (spec R5.1): host tests.
#   make test     build test_furi with the host compiler and run it
# The MINIX build (libfuri.a, /bin/furitest in QEMU) is in ../../minix.mk.
CC	?= cc
SAN	:= -fsanitize=undefined -fno-sanitize-recover=undefined
CFLAGS	?= -O1 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 $(SAN)
SRC	:= src/thread.c src/sync.c src/timer.c src/record.c src/string.c \
	   src/check.c src/fdwait.c src/port_host.c
B	:= build

test: $(B)/test_furi
	./$(B)/test_furi

$(B)/test_furi: test_furi.c $(SRC) src/furi_i.h include/furi.h include/core/*.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -DFURI_HOST -Iinclude -o $@ test_furi.c $(SRC)

clean:
	-rm -rf $(B)

.PHONY: test clean
