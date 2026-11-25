# canary cores: host unit tests (spec M.0, R16.5.7, R7b.4.4; task T35).
#   make test          build and run them with the host compiler
#   make target-check  the same sources with the ARM compiler, and sizes
CC	?= cc
CFLAGS	?= -O2 -Wall -Wextra -std=c99 -fsanitize=address,undefined -g
SRCS	= beacon.c gpiorules.c
RFSRCS	= jamdet.c follow.c
USBSRCS	= usbcan.c
OBJS	= beacon.o gpiorules.o jamdet.o follow.o usbcan.o
ARM	= arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -Os -std=gnu89 -Wall \
	  -Wextra -Werror -ffreestanding

test: test_canary test_rf test_usb
	./test_canary
	./test_rf
	./test_usb

test_canary: test_canary.c $(SRCS) beacon.h gpiorules.h
	$(CC) $(CFLAGS) -o $@ test_canary.c $(SRCS)

# jamdet (subghz canary) and follow (ble canary)
test_rf: test_rf.c $(RFSRCS) jamdet.h follow.h
	$(CC) $(CFLAGS) -o $@ test_rf.c $(RFSRCS)

# usbcan (usb canary)
test_usb: test_usb.c $(USBSRCS) usbcan.h
	$(CC) $(CFLAGS) -o $@ test_usb.c $(USBSRCS)

%.o: %.c
	$(ARM) -c $< -o $@

target-check: $(OBJS)
	arm-none-eabi-size $(OBJS)
	-rm -f $(OBJS)

clean:
	-rm -f test_canary test_rf test_usb $(OBJS)

.PHONY: test target-check clean
