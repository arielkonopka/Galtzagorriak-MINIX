# PC tests of kernel code that parses outside input (security reviews,
# docs/security/).  The kernel sources are compiled unchanged with host
# stubs (stubs/), under AddressSanitizer and UBSan.
#
#	make -C tests/host		build and run every test
#
# Needs a host C compiler only (gcc or clang).
MROOT	:= ../../..
CC	?= cc
SAN	:= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS	:= -O1 -g -Wall -Wno-unused-function $(SAN)
B	:= build

TESTS	:= parttest usbtest ibemutest

all: $(TESTS:%=run-%) run-libfuri run-libapp run-lfrfid

# libfuri core (spec R5.1): its own host tests, run here so CI runs them.
run-libfuri:
	$(MAKE) -C $(MROOT)/flipper/lib/furi test

# libapp (spec R7b.4): the timer rules, the notification policy, the spool.
run-libapp:
	$(MAKE) -C $(MROOT)/flipper/lib/app test

# liblfrfid (spec R9.0.2, AC9.0.1): every Unleashed LF RFID protocol round
# trip, encoder -> edges -> decoders (make lfrfidtest).
run-lfrfid:
	$(MAKE) -C $(MROOT)/flipper/tests/lfrfid test

run-%: $(B)/%
	./$<

# drvlib.c: copied next to its stub headers, so that its "kernel.h" is
# the stub one; <ibm/partition.h> is the real one.
$(B)/drvlib.c: $(MROOT)/src/kernel/drvlib.c $(MROOT)/src/kernel/drvlib.h
	@mkdir -p $(B)/ibm
	cp $(MROOT)/src/kernel/drvlib.c $(MROOT)/src/kernel/drvlib.h $(B)/
	cp $(MROOT)/include/ibm/partition.h $(B)/ibm/

# (its "can't read partition table" messages go to t_quiet: thousands)
$(B)/drvlib.o: $(B)/drvlib.c stubs/*.h
	$(CC) $(CFLAGS) -DT_QUIET -Istubs -I$(B) -c $< -o $@

$(B)/parttest: parttest.c $(B)/drvlib.o stubs/*.h
	$(CC) $(CFLAGS) -Istubs -I$(B) parttest.c $(B)/drvlib.o -o $@

# usb.c: the PRIMASK save/restore is ARM assembly, compiled out (T_ASM).
USBINC	:= -Istubs/usb -I$(MROOT)/flipper/vendor/libusb_stm32/inc \
	   -DSTM32WB55xx -DUSBD_SOF_DISABLED
$(B)/usb.c: $(MROOT)/flipper/hal/flipper/usb.c
	@mkdir -p $(B)
	sed -e 's/__asm volatile/T_ASM/' -e 's/uint32_t primask;/uint32_t primask = 0;/' $< > $@

$(B)/usbtest: usbtest.c $(B)/usb.c stubs/usb/*.h \
		$(MROOT)/flipper/vendor/libusb_stm32/src/usbd_core.c
	$(CC) $(CFLAGS) $(USBINC) -D'T_ASM(...)=((void) 0)' usbtest.c \
	    $(B)/usb.c $(MROOT)/flipper/vendor/libusb_stm32/src/usbd_core.c -o $@

# ibemu.c: the Dallas key's 1-Wire slave (R6.7 part 3), included whole.
$(B)/ibemutest: ibemutest.c $(MROOT)/src/kernel/arm/ibemu.c
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -Wno-old-style-definition ibemutest.c -o $@

clean:
	rm -rf $(B)

.PHONY: all clean run-libfuri run-libapp run-lfrfid
