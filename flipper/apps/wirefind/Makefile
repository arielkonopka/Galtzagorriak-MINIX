# wirefind cores: host unit tests (user request 2026-11-25).
#   make test          build and run them with the host compiler
#   make target-check  the same source with the ARM compiler, and its size
CC	?= cc
CFLAGS	?= -O2 -Wall -Wextra -std=c99 -fsanitize=address,undefined -g
ARM	= arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -Os -std=gnu89 -Wall \
	  -Wextra -Werror -ffreestanding

test: test_wf
	./test_wf

test_wf: test_wf.c wirefind.c wirefind.h
	$(CC) $(CFLAGS) -o $@ test_wf.c wirefind.c -lm

wirefind.o: wirefind.c wirefind.h
	$(ARM) -c wirefind.c -o $@

target-check: wirefind.o
	arm-none-eabi-size wirefind.o
	-rm -f wirefind.o

clean:
	-rm -f test_wf wirefind.o

.PHONY: test target-check clean
