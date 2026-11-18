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

TESTS	:= parttest usbtest ibemutest icontest wintest desktest settimetest

all: $(TESTS:%=run-%) run-libfuri run-libapp run-native run-lfrfid run-modem run-hid run-subghz

# libfuri core (spec R5.1): its own host tests, run here so CI runs them.
run-libfuri:
	$(MAKE) -C $(MROOT)/flipper/lib/furi test

# libapp (spec R7b.4): the timer rules, the notification policy, the spool.
run-libapp:
	$(MAKE) -C $(MROOT)/flipper/lib/app test

# remind and alarm (spec 7b.1, 7b.2): the log reader, times, the form.
run-native:
	$(MAKE) -C $(MROOT)/flipper/apps/native test
# liblfrfid (spec R9.0.2, AC9.0.1): every Unleashed LF RFID protocol round
# trip, encoder -> edges -> decoders (make lfrfidtest).
run-lfrfid:
	$(MAKE) -C $(MROOT)/flipper/tests/lfrfid test
# libmodem (spec section P, ACP.1, ACP.2): data modems between two
# Flippers; the lossy-channel, line-code, legal and fuzz tests (make modemtest).
run-modem:
	$(MAKE) -C $(MROOT)/flipper/tests/modem test
# libhid (spec R11.3.2): layouts, all printable ASCII round trip in US and
# DE, UTF-8, the report builders (make hidtest).
run-hid:
	$(MAKE) -C $(MROOT)/flipper/tests/hid test
# libsubghz (Phase 9, issue #302): the Unleashed Sub-GHz protocols; every
# RAW capture of the Unleashed unit tests, encoder round trips, a fuzz
# (make subghztest).
run-subghz:
	$(MAKE) -C $(MROOT)/flipper/tests/subghz test

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

# appline.c: the launcher's /etc/apps lines and icons (R5.6a, T21a).
$(B)/icontest: icontest.c $(MROOT)/flipper/gui/appline.c $(MROOT)/flipper/gui/appline.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) icontest.c $(MROOT)/flipper/gui/appline.c -o $@

# winrec.c and libgm: the window records of spec K.8 (T23b).
$(B)/wintest: wintest.c $(MROOT)/flipper/gui/winrec.c $(MROOT)/flipper/gui/winrec.h \
		$(MROOT)/src/lib/gm/gm.c $(MROOT)/src/lib/gm/gm.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) wintest.c $(MROOT)/flipper/gui/winrec.c \
	    $(MROOT)/src/lib/gm/gm.c -o $@

# desktop.c: the desktop's module records, /var/run/gui (spec K.3, K.6, T23a).
$(B)/desktest: desktest.c $(MROOT)/flipper/gui/desktop.c $(MROOT)/flipper/gui/desktop.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) desktest.c $(MROOT)/flipper/gui/desktop.c -o $@
# settings time and about (spec K.7, T24b, T24d): the zone table, /etc/TZ,
# local time from a rule, the About rows, a group's file.
SETDIR	:= $(MROOT)/src/commands/settings
SETSRC	:= zones.c afmt.c conf.c groups.c display.c sound.c
$(B)/settimetest: settimetest.c $(SETSRC:%=$(SETDIR)/%) $(SETDIR)/zones.h \
		$(SETDIR)/afmt.h $(SETDIR)/sgroup.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -Wno-unused-result settimetest.c $(SETSRC:%=$(SETDIR)/%) -o $@

clean:
	rm -rf $(B)

.PHONY: all clean run-libfuri run-libapp run-native run-lfrfid run-modem run-hid run-subghz
