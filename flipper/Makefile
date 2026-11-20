# MINIX 2.0.0 for Flipper Zero -- build system.
#
#   make                 build for QEMU (default)
#   make run             build and run in qemu-system-arm -M mps2-an386
#   make BOARD=flipper   build for the real device
#   make BOARD=flipper flash   flash over USB DFU (Flipper in DFU mode)

BOARD	?= qemu
PROG	?= m0
# LATTRACE=1: the latency trace build (spec R0.8e), in its own directory.
# WRAPTEST=1: the uptime-wrap test build (R2d.6, AC2d.3): the tick counter
# starts 10 s before it wraps at 2^32 (WRAPTEST=n: n s); its own directory.
BUILD	:= build/$(BOARD)$(if $(LATTRACE),-lat)$(if $(WRAPTEST),-wrap)

CROSS	?= arm-none-eabi-
CC	:= $(CROSS)gcc
OBJCOPY	:= $(CROSS)objcopy
SIZE	:= $(CROSS)size

ARCH	:= -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
CFLAGS	:= $(ARCH) -std=gnu11 -Os -g -ffreestanding -fno-common -fno-tree-loop-distribute-patterns \
	   -ffunction-sections -fdata-sections \
	   -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration \
	   -Ihal -Ihal/common
LDFLAGS	:= $(ARCH) -nostdlib -nostartfiles -Wl,--gc-sections \
	   -Lld -Tld/$(BOARD).ld -Wl,-Map=$(BUILD)/$(PROG).map
LIBS	:= -lgcc

ifeq ($(BOARD),flipper)
CFLAGS	+= -DBOARD_FLIPPER -DSTM32WB55xx -Ivendor/cmsis_wb -Ivendor/cmsis_core \
	   -Ivendor/libusb_stm32/inc -DUSBD_SOF_DISABLED
BOARD_SRC := hal/flipper/usb.c vendor/libusb_stm32/src/usbd_core.c \
	   vendor/libusb_stm32/src/usbd_stm32wb55_devfs.c
else ifeq ($(BOARD),qemu)
CFLAGS	+= -DBOARD_QEMU -DQEMU_DISK='"$(BUILD)/minix/disk.img"'
else
$(error BOARD must be qemu or flipper)
endif

# freestanding string routines used by the HAL
HAL_SRC	:= hal/common/vectors.c hal/common/kprintf.c hal/common/fbcon.c \
	   hal/common/string.c hal/common/splash.c hal/$(BOARD)/board.c \
	   $(BOARD_SRC)
SRC	:= $(HAL_SRC) $(PROG)/main.c
OBJ	:= $(SRC:%.c=$(BUILD)/%.o)

QEMU	:= qemu-system-arm -M mps2-an386 -nographic -monitor none \
	   -serial stdio -semihosting-config enable=on,target=native

.PHONY: all run flash clean lfrfidtest modemtest hidtest subghztest ibkeytest dhttest
all: $(BUILD)/$(PROG).elf $(BUILD)/$(PROG).bin

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
# Third-party code: build quietly, without our warning flags.
$(BUILD)/vendor/%.o: vendor/%.c
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -W%,$(CFLAGS)) -w -MMD -MP -c $< -o $@

$(BUILD)/$(PROG).elf: $(OBJ) ld/$(BOARD).ld ld/sections.ld
	$(CC) $(LDFLAGS) $(OBJ) $(LIBS) -o $@
	$(SIZE) $@

$(BUILD)/$(PROG).bin: $(BUILD)/$(PROG).elf
	$(OBJCOPY) -O binary $< $@

# liblfrfid on the PC (spec R9.0.2): every LF RFID protocol round trip.
lfrfidtest:
	$(MAKE) -C tests/lfrfid test

# libibkey on the PC (spec R6.7 part 3b): Cyfral and Metakom iButton keys,
# encoder -> the line -> decoders.
ibkeytest:
	$(MAKE) -C tests/ibkey test

# libdht on the PC (issue #358): the AM2301/DHT21/DHT22 frame decoder.
dhttest:
	$(MAKE) -C tests/dht test

# libmodem on the PC (spec section P, ACP.1, ACP.2): frames, ARQ, carousel,
# the RFID line code, the Sub-GHz presets, legal check and duty counter.
modemtest:
	$(MAKE) -C tests/modem test

# libhid on the PC (spec R11.3.2): layouts, US and DE round trip, UTF-8.
hidtest:
	$(MAKE) -C tests/hid test

# libsubghz on the PC (Phase 9, issue #302): the Unleashed Sub-GHz
# captures decoded, encoder round trips, a decoder fuzz.
subghztest:
	$(MAKE) -C tests/subghz test

run: $(BUILD)/$(PROG).elf
	@test "$(BOARD)" = qemu || { echo "run needs BOARD=qemu"; exit 1; }
	timeout 30 $(QEMU) -kernel $<

# Flipper DFU: hold LEFT+BACK, release BACK (keep LEFT) -> ROM DFU bootloader.
# Only the image itself is written; option bytes and the radio stack are not
# touched.  Restore stock firmware with qFlipper -> Repair.
flash: $(BUILD)/$(PROG).bin
	@test "$(BOARD)" = flipper || { echo "flash needs BOARD=flipper"; exit 1; }
	dfu-util -a 0 -s 0x08000000:leave -D $<

clean:
	rm -rf build

-include $(OBJ:.o=.d)

# MINIX itself: see minix.mk.
include minix.mk
