# The native apps' helpers (spec 7b.1, 7b.2): host tests.
#   make test     build test_nat with the host compiler and run it
# The MINIX build (remind, alarm) is in ../../minix.mk.
CC	?= cc
SAN	:= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS	?= -O1 -g -Wall -Wextra -std=gnu11 $(SAN)
B	:= build

test: $(B)/test_nat
	./$(B)/test_nat

$(B)/test_nat: test_nat.c nat_core.c nat.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -o $@ test_nat.c nat_core.c

clean:
	-rm -rf $(B)

.PHONY: test clean
