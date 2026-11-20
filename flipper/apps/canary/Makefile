# canary cores: host unit tests (spec M.0, R16.5.7, R7b.4.4; task T35).
#   make test          build and run them with the host compiler
#   make target-check  the same sources with the ARM compiler, and sizes
CC	?= cc
CFLAGS	?= -O2 -Wall -Wextra -std=c99 -fsanitize=address,undefined -g
SRCS	= beacon.c gpiorules.c

test: test_canary
	./test_canary

test_canary: test_canary.c $(SRCS) beacon.h gpiorules.h
	$(CC) $(CFLAGS) -o $@ test_canary.c $(SRCS)

target-check:
	arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -Os -std=gnu89 -Wall -Wextra \
	   -Werror -ffreestanding -c beacon.c -o beacon.o
	arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -Os -std=gnu89 -Wall -Wextra \
	   -Werror -ffreestanding -c gpiorules.c -o gpiorules.o
	arm-none-eabi-size beacon.o gpiorules.o; rm -f beacon.o gpiorules.o

clean:
	-rm -f test_canary

.PHONY: test target-check clean
