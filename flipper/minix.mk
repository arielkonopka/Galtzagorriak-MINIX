# MINIX 2.0.0 for ARM Cortex-M: C library, kernel, boot servers, user
# programs and the root file system image.  Included by Makefile.
#
#   make minix          kernel + MM + FS + INIT -> build/<board>/minix/minix.bin
#   make minix-run      run in QEMU (BOARD=qemu), disk = build/qemu/minix/disk.img
#   make minix-flash    flash the Flipper (BOARD=flipper, DFU mode)
#   make disk           root file system image (MINIX V2) for SD card / QEMU

MROOT	:= ..
NM	:= $(CROSS)nm
AR	:= $(CROSS)ar
GCCINC	:= $(shell $(CC) -print-file-name=include)
MB	:= $(BUILD)/minix

# K&R C from 1996: compile as gnu89, signed chars like ACK on the PC, and
# define the word-size symbols ACK predefines.
MCFLAGS	:= $(ARCH) -std=gnu89 -Os -g -fsigned-char -ffreestanding -fno-common \
	   -fno-strict-aliasing \
	   -fno-tree-loop-distribute-patterns -ffunction-sections -fdata-sections \
	   -nostdinc -I$(MROOT)/include -isystem $(GCCINC) -Ihal \
	   -DMACHINE=FLIPPER $(filter -DBOARD_FLIPPER -DBOARD_QEMU,$(CFLAGS)) \
	   -D_EM_WSIZE=4 -D_EM_PSIZE=4 -D_EM_LSIZE=4 -D__minix=1 \
	   -Wall -Wno-parentheses -Wno-unused -Wno-implicit-int \
	   -Wno-missing-braces -Wno-char-subscripts -Wno-pointer-sign \
	   -Wno-old-style-definition -Wno-return-type -Wno-dangling-else \
	   -Wno-format -Wno-uninitialized -Wno-maybe-uninitialized \
	   -Wno-stringop-overflow	# FS passes "." for char[NAME_MAX]; reads stop at NUL
MLDFLAGS := $(ARCH) -nostdlib -nostartfiles -Wl,--gc-sections \
	    -Wl,--no-warn-rwx-segments

#############################################################################
# C library
#############################################################################
LIBDIRS	:= ansi posix other stdio syslib math
LIBC_SRC := $(foreach d,$(LIBDIRS),$(wildcard $(MROOT)/src/lib/$(d)/*.c)) \
	    $(MROOT)/src/lib/fphook/fltpr.c $(MROOT)/src/lib/fphook/strtod.c
LIBC_RTS := _sendrec.S setjmp.S __sigreturn.S brksize.S
LIBC_OBJ := $(LIBC_SRC:$(MROOT)/src/lib/%.c=$(MB)/libc/%.o) \
	    $(LIBC_RTS:%=$(MB)/libc/arm/%.o) $(MB)/libc/syscall_aliases.o
LIBCFLAGS := $(MCFLAGS) -D_MINIX -D_POSIX_SOURCE -I$(MROOT)/src

$(MB)/libc/%.o: $(MROOT)/src/lib/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(LIBCFLAGS) -MMD -MP -c $< -o $@
$(MB)/libc/arm/%.o: $(MROOT)/src/lib/arm/rts/%
	@mkdir -p $(dir $@)
	$(CC) $(MCFLAGS) -c $< -o $@
# ARM versions of the src/lib/syscall/*.s name aliases (foo -> _foo).
$(MB)/libc/syscall_aliases.S: tools/genaliases.py \
		$(wildcard $(MROOT)/src/lib/syscall/*.s)
	@mkdir -p $(dir $@)
	python3 -I tools/genaliases.py $(MROOT)/src/lib/syscall $@
$(MB)/libc/syscall_aliases.o: $(MB)/libc/syscall_aliases.S
	$(CC) $(MCFLAGS) -c $< -o $@

$(MB)/libc.a: $(LIBC_OBJ)
	rm -f $@
	@$(AR) rcs $@ $^
	@echo "  libc.a: $(words $^) objects"

#############################################################################
# Kernel
#############################################################################
KDIR	:= $(MROOT)/src/kernel
KSRC	:= main.c proc.c system.c clock.c table.c memory.c tty.c driver.c \
	   drvlib.c misc.c arm/klib.c arm/cons.c arm/shadow.c arm/nodev.c \
	   arm/disk.c arm/mpx.S
KOBJ	:= $(KSRC:%=$(MB)/kernel/%.o) \
	   $(filter-out %/main.o,$(HAL_SRC:%.c=$(BUILD)/%.o))

$(MB)/kernel/%.o: $(KDIR)/%
	@mkdir -p $(dir $@)
	$(CC) $(MCFLAGS) -I$(KDIR) -MMD -MP -c $< -o $@

$(MB)/kernel.elf: $(KOBJ) $(MB)/libc.a ld/$(BOARD).ld ld/sections.ld
	$(CC) $(MLDFLAGS) -Lld -Tld/$(BOARD).ld -Wl,-Map=$(MB)/kernel.map \
	   $(KOBJ) $(MB)/libc.a -lgcc -o $@
	$(SIZE) $@

#############################################################################
# Boot servers: MM, FS and INIT.  Each links at the flash and RAM addresses
# that follow the image before it: kernel -> mm -> fs -> init.
#############################################################################
RAM_END		:= 0x20030000
STACK_mm	:= 1024	# stack (+ gap) above bss
STACK_fs	:= 2048
STACK_init	:= 8192	# heap (brk) + stack: stdio, exec arg block
MM_SRC	:= main.c forkexit.c break.c exec.c signal.c alloc.c utility.c \
	   table.c putk.c trace.c getset.c
FS_SRC	:= main.c open.c read.c write.c pipe.c device.c path.c mount.c \
	   link.c super.c inode.c cache.c cache2.c filedes.c stadir.c \
	   protect.c time.c lock.c misc.c utility.c table.c putk.c
next_sym = $$($(NM) $(1) | awk '$$3 == "$(2)" { print "0x" $$1 }')

$(MB)/mm/%.o: $(MROOT)/src/mm/%
	@mkdir -p $(dir $@)
	$(CC) $(MCFLAGS) -MMD -MP -c $< -o $@
$(MB)/fs/%.o: $(MROOT)/src/fs/%
	@mkdir -p $(dir $@)
	$(CC) $(MCFLAGS) -MMD -MP -c $< -o $@
$(MB)/m2/%.o: m2/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LIBCFLAGS) -MMD -MP -c $< -o $@

$(MB)/srv/%/bootimg.o: $(MROOT)/src/lib/arm/rts/bootimg.S minix.mk
	@mkdir -p $(dir $@)
	$(CC) $(MCFLAGS) -DIMAGE_NAME='"$(shell echo $* | tr a-z A-Z)"' \
	   -DSTACK_SIZE=$(STACK_$*) -c $< -o $@
$(MB)/srv/crtso.o: $(MROOT)/src/lib/arm/rts/crtso.S
	@mkdir -p $(dir $@)
	$(CC) $(MCFLAGS) -c $< -o $@

srv_head = $(MB)/srv/$(1)/bootimg.o $(MB)/srv/crtso.o
SRV_mm	 = $(call srv_head,mm) $(MM_SRC:%=$(MB)/mm/%.o)
SRV_fs	 = $(call srv_head,fs) $(FS_SRC:%=$(MB)/fs/%.o)
# INIT: the real one (src/tools/init.c), or INIT=m2 for the test INIT that
# runs the milestone-2 checks and exits QEMU.
INIT	?= real
ifeq ($(INIT),m2)
SRV_init = $(call srv_head,init) $(MB)/m2/init.o
else
SRV_init = $(call srv_head,init) $(MB)/tools/init.o
endif
$(MB)/tools/%.o: $(MROOT)/src/tools/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LIBCFLAGS) -MMD -MP -c $< -o $@

define link_server
	$(CC) $(MLDFLAGS) \
	   -Wl,--defsym=FLASH_ORG=$(call next_sym,$(2),_next_flash) \
	   -Wl,--defsym=RAM_ORG=$(call next_sym,$(2),_next_ram) \
	   -Wl,--defsym=RAM_END=$(RAM_END) \
	   -Wl,--defsym=STACK_SIZE=$(STACK_$(1)) \
	   -Tld/server.ld -Wl,-Map=$(MB)/$(1).map $(SRV_$(1)) $(MB)/libc.a \
	   -lgcc -o $@
	$(SIZE) $@
endef

$(MB)/mm.elf: $(MB)/kernel.elf $(SRV_mm) $(MB)/libc.a ld/server.ld
	$(call link_server,mm,$(MB)/kernel.elf)
$(MB)/fs.elf: $(MB)/mm.elf $(SRV_fs) $(MB)/libc.a ld/server.ld
	$(call link_server,fs,$(MB)/mm.elf)
# Relink when switching between INIT=real and INIT=m2.
$(MB)/.init-$(INIT):
	@mkdir -p $(MB)
	@rm -f $(MB)/.init-*
	@touch $@
$(MB)/init.elf: $(MB)/fs.elf $(SRV_init) $(MB)/libc.a ld/server.ld $(MB)/.init-$(INIT)
	$(call link_server,init,$(MB)/fs.elf)

$(MB)/minix.bin: $(MB)/kernel.elf $(MB)/mm.elf $(MB)/fs.elf $(MB)/init.elf \
		 tools/mkflash.py
	python3 -I tools/mkflash.py $(OBJCOPY) $(NM) $@ $(filter %.elf,$^)

#############################################################################
# User programs: linked at 0 with relocations (ld/user.ld, -Wl,-q), then
# converted to MINIX a.out by tools/elf2mnx.py.
#############################################################################
# The shell, the MINIX commands (src/commands/simple, halt) and m2test.
CMDS	:= basename cat chmod chown cksum clr cmp comm cp cut date dd df \
	   dirname du echo ed expand expr factor fgrep find fold getty grep \
	   head id kill login ls mkdir mkfifo mknod mkfs mount od paste \
	   printenv printroot pwd rev rmdir sed sleep sort split strings stty \
	   su sum sync tail tar tee test touch tr tty umount uname uniq update \
	   wc which whoami yes
UPROGS	:= sh halt m2test $(CMDS)
# Other names of multi-call programs, installed as copies.
ALIASES	:= mv=cp ln=cp cpdir=cp clone=cp [=test reboot=halt
ALIASES	+= $(addsuffix =cp,rm)
# Stack + heap reserve per program (MINIX chmem).  RAM is ~114 KB for all
# user processes, and a fork needs a second copy of the data, so keep these
# small: default 6 KB (tools/elf2mnx.py), more only where needed.
CHMEM_sh     := 8192
CHMEM_update := 2048
CHMEM_ed     := 16384
CHMEM_sort   := 12288
CHMEM_ls     := 12288
CHMEM_rev    := 8192
CHMEM_tee    := 8192
CHMEM_m2test := 16384

SH_SRC	:= sh1.c sh2.c sh3.c sh4.c sh5.c sh6.c
$(MB)/cmd/sh/%.o: $(MROOT)/src/commands/sh/%
	@mkdir -p $(dir $@)
	@$(CC) $(LIBCFLAGS) -MMD -MP -c $< -o $@
$(MB)/cmd/reboot/%.o: $(MROOT)/src/commands/reboot/%
	@mkdir -p $(dir $@)
	@$(CC) $(LIBCFLAGS) -MMD -MP -c $< -o $@
$(MB)/cmd/m2test.o: m2/m2test.c
	@mkdir -p $(dir $@)
	@$(CC) $(LIBCFLAGS) -MMD -MP -c $< -o $@
$(MB)/cmd/%.o: $(MROOT)/src/commands/simple/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(LIBCFLAGS) -MMD -MP -c $< -o $@

OBJS_sh	 = $(SH_SRC:%=$(MB)/cmd/sh/%.o)
OBJS_halt = $(MB)/cmd/reboot/halt.c.o $(MB)/cmd/reboot/log.c.o
objs_of	 = $(if $(OBJS_$(1)),$(OBJS_$(1)),$(MB)/cmd/$(1).o)

define user_prog
$(MB)/user/$(1).elf: $(call objs_of,$(1)) $(MB)/srv/crtso.o $(MB)/libc.a ld/user.ld
	@mkdir -p $$(dir $$@)
	@$(CC) $(MLDFLAGS) -Wl,-q -Tld/user.ld -Wl,-Map=$$@.map \
	   $(MB)/srv/crtso.o $(call objs_of,$(1)) $(MB)/libc.a -lgcc -o $$@
endef
$(foreach p,$(UPROGS),$(eval $(call user_prog,$(p))))

# Convert ELF -> MINIX a.out with the chmem value above (default 6 KB).
$(MB)/bin/%: $(MB)/user/%.elf tools/elf2mnx.py minix.mk
	@mkdir -p $(dir $@)
	@python3 -I tools/elf2mnx.py $< $@ $(if $(CHMEM_$*),--stack $(CHMEM_$*))

# Root file system: rootfs/proto -> fs.img (dd to the SD card partition)
# and disk.img (MBR + partition 1, the QEMU "SD card").  @BINFILES@ in the
# proto becomes one entry per program.
$(MB)/proto: rootfs/proto minix.mk tools/mkproto.py
	@mkdir -p $(MB)
	python3 -I tools/mkproto.py $< $@ $(MB)/bin "$(UPROGS)" "$(ALIASES)"
$(MB)/fs.img $(MB)/disk.img: $(MB)/proto tools/mkfsimg.py \
		$(wildcard rootfs/etc/*) $(UPROGS:%=$(MB)/bin/%)
	python3 -I tools/mkfsimg.py $(MB)/proto $(MB)/fs.img --root . \
	   --mbr $(MB)/disk.img

.PHONY: minix minix-run minix-flash libc disk
disk: $(MB)/disk.img
minix: $(MB)/minix.bin
libc: $(MB)/libc.a

QEMU_MINIX := qemu-system-arm -M mps2-an386 -nographic -monitor none \
	   -serial stdio -semihosting-config enable=on,target=native,userspace=on

minix-run: $(MB)/minix.bin $(MB)/disk.img
	@test "$(BOARD)" = qemu || { echo "minix-run needs BOARD=qemu"; exit 1; }
	timeout 60 $(QEMU_MINIX) -device loader,file=$<,addr=0

minix-flash: $(MB)/minix.bin
	@test "$(BOARD)" = flipper || { echo "minix-flash needs BOARD=flipper"; exit 1; }
	dfu-util -a 0 -s 0x08000000:leave -D $<

-include $(shell find $(MB) -name '*.d' 2>/dev/null)
