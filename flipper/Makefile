# MINIX 2.0.0 for Flipper Zero -- build system.
#
#   make                 build for QEMU (default)
#   make run             build and run in qemu-system-arm -M mps2-an386
#   make BOARD=flipper   build for the real device
#   make BOARD=flipper flash   flash over USB DFU (Flipper in DFU mode)

BOARD	?= qemu
PROG	?= m0
BUILD	:= build/$(BOARD)

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
CFLAGS	+= -DBOARD_FLIPPER -DSTM32WB55xx -Ivendor/cmsis_wb -Ivendor/cmsis_core
else ifeq ($(BOARD),qemu)
CFLAGS	+= -DBOARD_QEMU
else
$(error BOARD must be qemu or flipper)
endif

# freestanding string routines used by the HAL
HAL_SRC	:= hal/common/vectors.c hal/common/kprintf.c hal/common/fbcon.c \
	   hal/common/string.c hal/$(BOARD)/board.c
SRC	:= $(HAL_SRC) $(PROG)/main.c
OBJ	:= $(SRC:%.c=$(BUILD)/%.o)

QEMU	:= qemu-system-arm -M mps2-an386 -nographic -monitor none \
	   -serial stdio -semihosting-config enable=on,target=native

.PHONY: all run flash clean
all: $(BUILD)/$(PROG).elf $(BUILD)/$(PROG).bin

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/$(PROG).elf: $(OBJ) ld/$(BOARD).ld ld/sections.ld
	$(CC) $(LDFLAGS) $(OBJ) $(LIBS) -o $@
	$(SIZE) $@

$(BUILD)/$(PROG).bin: $(BUILD)/$(PROG).elf
	$(OBJCOPY) -O binary $< $@

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

#############################################################################
# MINIX: kernel + boot servers.   make minix [BOARD=qemu|flipper]
#        make minix-run   (QEMU)    make minix-flash  (Flipper)
# The boot servers are currently the milestone-1 stand-ins in m1/.
#############################################################################
MROOT	:= ..
NM	:= $(CROSS)nm
GCCINC	:= $(shell $(CC) -print-file-name=include)
MB	:= $(BUILD)/minix

# K&R C from 1996: compile as gnu89, signed chars like ACK on the PC.
MCFLAGS	:= $(ARCH) -std=gnu89 -Os -g -fsigned-char -ffreestanding -fno-common \
	   -fno-tree-loop-distribute-patterns -ffunction-sections -fdata-sections \
	   -nostdinc -I$(MROOT)/include -isystem $(GCCINC) -Ihal \
	   -DMACHINE=FLIPPER $(filter -DBOARD_%,$(CFLAGS)) \
	   -D_EM_WSIZE=4 -D_EM_PSIZE=4 -D_EM_LSIZE=4 \
	   -Wall -Wno-parentheses -Wno-unused -Wno-implicit-int \
	   -Wno-missing-braces -Wno-char-subscripts -Wno-pointer-sign \
	   -Wno-old-style-definition -Wno-return-type -Wno-dangling-else \
	   -Wno-format -Wno-uninitialized -Wno-maybe-uninitialized

KDIR	:= $(MROOT)/src/kernel
KSRC	:= main.c proc.c system.c clock.c table.c memory.c tty.c driver.c \
	   drvlib.c misc.c arm/klib.c arm/cons.c arm/shadow.c arm/nodev.c \
	   arm/mpx.S
KLIB	:= other/printk.c ansi/strcpy.c ansi/strcmp.c ansi/strncpy.c \
	   ansi/strncmp.c ansi/strtol.c posix/_sigset.c other/errno.c \
	   arm/rts/_sendrec.S
KOBJ	:= $(KSRC:%=$(MB)/kernel/%.o) $(KLIB:%=$(MB)/lib/%.o) \
	   $(MB)/lib/syscall_aliases.o \
	   $(filter-out %/main.o,$(HAL_SRC:%.c=$(BUILD)/%.o))

# ARM versions of the src/lib/syscall/*.s name aliases (foo -> _foo).
$(MB)/lib/syscall_aliases.S: tools/genaliases.py $(wildcard $(MROOT)/src/lib/syscall/*.s)
	@mkdir -p $(dir $@)
	python3 -I tools/genaliases.py $(MROOT)/src/lib/syscall $@
$(MB)/lib/syscall_aliases.o: $(MB)/lib/syscall_aliases.S
	$(CC) $(MCFLAGS) -c $< -o $@

$(MB)/kernel/%.o: $(KDIR)/%
	@mkdir -p $(dir $@)
	$(CC) $(MCFLAGS) -I$(KDIR) -MMD -MP -c $< -o $@
$(MB)/lib/%.o: $(MROOT)/src/lib/%
	@mkdir -p $(dir $@)
	$(CC) $(MCFLAGS) -MMD -MP -c $< -o $@

$(MB)/kernel.elf: $(KOBJ) ld/$(BOARD).ld ld/sections.ld
	$(CC) $(ARCH) -nostdlib -nostartfiles -Wl,--gc-sections -Lld \
	   -Tld/$(BOARD).ld -Wl,-Map=$(MB)/kernel.map $(KOBJ) -lgcc -o $@
	$(SIZE) $@

# Boot servers.  Each one links at the flash and RAM addresses that follow
# the image before it: kernel -> mm -> fs -> init.
RAM_END		:= 0x20030000
STACK_mm	:= 2048
STACK_fs	:= 2048
STACK_init	:= 1024
SRV_COMMON	:= arm/rts/crtso.S arm/rts/_sendrec.S
next_sym = $$($(NM) $(1) | awk '$$3 == "$(2)" { print "0x" $$1 }')

$(MB)/srv/%/bootimg.o: $(MROOT)/src/lib/arm/rts/bootimg.S
	@mkdir -p $(dir $@)
	$(CC) $(MCFLAGS) -DIMAGE_NAME='"$(shell echo $* | tr a-z A-Z)"' \
	   -DSTACK_SIZE=$(STACK_$*) -c $< -o $@
$(MB)/m1/%.o: m1/%.c m1/m1.h
	@mkdir -p $(dir $@)
	$(CC) $(MCFLAGS) -c $< -o $@

srv_objs = $(MB)/srv/$(1)/bootimg.o $(SRV_COMMON:%=$(MB)/lib/%.o) \
	   $(MB)/m1/$(1).o $(MB)/m1/m1lib.o

define link_server
	$(CC) $(ARCH) -nostdlib -nostartfiles -Wl,--gc-sections \
	   -Wl,--defsym=FLASH_ORG=$(call next_sym,$(2),_next_flash) \
	   -Wl,--defsym=RAM_ORG=$(call next_sym,$(2),_next_ram) \
	   -Wl,--defsym=RAM_END=$(RAM_END) \
	   -Wl,--defsym=STACK_SIZE=$(STACK_$(1)) \
	   -Tld/server.ld -Wl,-Map=$(MB)/$(1).map $(call srv_objs,$(1)) \
	   -lgcc -o $@
	$(SIZE) $@
endef

$(MB)/mm.elf: $(MB)/kernel.elf $(call srv_objs,mm) ld/server.ld
	$(call link_server,mm,$(MB)/kernel.elf)
$(MB)/fs.elf: $(MB)/mm.elf $(call srv_objs,fs) ld/server.ld
	$(call link_server,fs,$(MB)/mm.elf)
$(MB)/init.elf: $(MB)/fs.elf $(call srv_objs,init) ld/server.ld
	$(call link_server,init,$(MB)/fs.elf)

$(MB)/minix.bin: $(MB)/kernel.elf $(MB)/mm.elf $(MB)/fs.elf $(MB)/init.elf \
		 tools/mkflash.py
	python3 -I tools/mkflash.py $(OBJCOPY) $(NM) $@ $(filter %.elf,$^)

.PHONY: minix minix-run minix-flash
minix: $(MB)/minix.bin

QEMU_MINIX := qemu-system-arm -M mps2-an386 -nographic -monitor none \
	   -serial stdio -semihosting-config enable=on,target=native,userspace=on

minix-run: $(MB)/minix.bin
	@test "$(BOARD)" = qemu || { echo "minix-run needs BOARD=qemu"; exit 1; }
	timeout 30 $(QEMU_MINIX) -device loader,file=$<,addr=0

minix-flash: $(MB)/minix.bin
	@test "$(BOARD)" = flipper || { echo "minix-flash needs BOARD=flipper"; exit 1; }
	dfu-util -a 0 -s 0x08000000:leave -D $<

-include $(shell find $(MB) -name '*.d' 2>/dev/null)
