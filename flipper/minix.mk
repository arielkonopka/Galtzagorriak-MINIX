# MINIX 2.0.0 for ARM Cortex-M: C library, kernel, boot servers, user
# programs and the root file system image.  Included by Makefile.
#
#   make minix          kernel + MM + FS + INIT -> build/<board>/minix/minix.bin
#   make minix-run      run in QEMU (BOARD=qemu), disk = build/qemu/minix/disk.img
#   make minix-flash    flash the Flipper (BOARD=flipper, DFU mode)
#   make disk           root file system image (MINIX V2) for SD card / QEMU

MROOT	:= ..
COMMA	:= ,
NM	:= $(CROSS)nm
AR	:= $(CROSS)ar
GCCINC	:= $(shell $(CC) -print-file-name=include)
MB	:= $(BUILD)/minix

# K&R C from 1996: compile as gnu89, signed chars like ACK on the PC, and
# define the word-size symbols ACK predefines.
MCFLAGS	:= $(ARCH) -std=gnu89 -Os -g -fsigned-char -ffreestanding -fno-common \
	   -fno-strict-aliasing \
	   -fno-tree-loop-distribute-patterns -ffunction-sections -fdata-sections \
	   -nostdinc -I$(MROOT)/include -I$(MB)/gen -isystem $(GCCINC) \
	   -Ihal -Ihal/common -DMACHINE=FLIPPER $(filter -DBOARD_FLIPPER -DBOARD_QEMU,$(CFLAGS)) \
	   -D_EM_WSIZE=4 -D_EM_PSIZE=4 -D_EM_LSIZE=4 -D__minix=1 \
	   -Wall -Wno-parentheses -Wno-unused -Wno-implicit-int \
	   -Wno-missing-braces -Wno-char-subscripts -Wno-pointer-sign \
	   -Wno-old-style-definition -Wno-return-type -Wno-dangling-else \
	   -Wno-format -Wno-uninitialized -Wno-maybe-uninitialized \
	   -Wno-stringop-overflow	# FS passes "." for char[NAME_MAX]; reads stop at NUL
MCFLAGS	+= $(if $(LATTRACE),-DLATTRACE=1)
MCFLAGS	+= $(if $(WRAPTEST),-DREALTIME_WRAP=$(if $(filter 1,$(WRAPTEST)),10,$(WRAPTEST)))
MLDFLAGS := $(ARCH) -nostdlib -nostartfiles -Wl,--gc-sections \
	    -Wl,--no-warn-rwx-segments

#############################################################################
# C library
#############################################################################
LIBDIRS	:= ansi posix other stdio syslib math
LIBC_SRC := $(foreach d,$(LIBDIRS),$(wildcard $(MROOT)/src/lib/$(d)/*.c)) \
	    $(MROOT)/src/lib/fphook/fltpr.c $(MROOT)/src/lib/fphook/strtod.c
# memcpy/memmove/memset: Cortex-M4 assembly (arm/rts/memops.S) instead of
# the byte-at-a-time C versions; the kernel uses them too.
LIBC_SRC := $(filter-out $(addprefix $(MROOT)/src/lib/ansi/,memcpy.c memmove.c \
		memset.c),$(LIBC_SRC))
LIBC_RTS := _sendrec.S setjmp.S __sigreturn.S brksize.S memops.S _vfork.S
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

# User programs are position-independent (ld/user-pic.ld): data only through
# the GOT with r9 as its base, so one copy of a program's text - in RAM or
# in the flash text cache - can serve every process.  Their C library is
# built a second time that way; kernel and servers keep the plain one.
PICFLAGS := -fpic -msingle-pic-base -mpic-register=r9 \
	    -mno-pic-data-is-text-relative
UCFLAGS	:= $(LIBCFLAGS) $(PICFLAGS)
LIBCP_OBJ := $(LIBC_OBJ:$(MB)/libc/%=$(MB)/libcp/%)
$(MB)/libcp/%.o: $(MROOT)/src/lib/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@
$(MB)/libcp/arm/%.o: $(MROOT)/src/lib/arm/rts/%
	@mkdir -p $(dir $@)
	$(CC) $(MCFLAGS) $(PICFLAGS) -c $< -o $@
$(MB)/libcp/syscall_aliases.o: $(MB)/libc/syscall_aliases.S
	$(CC) $(MCFLAGS) $(PICFLAGS) -c $< -o $@
$(MB)/libc-pic.a: $(LIBCP_OBJ)
	$(AR) rcs $@.new $^ && mv -f $@.new $@
	@echo "  libc-pic.a: $(words $^) objects"
$(MB)/user/crtso.o: $(MROOT)/src/lib/arm/rts/crtso.S
	@mkdir -p $(dir $@)
	$(CC) $(MCFLAGS) $(PICFLAGS) -c $< -o $@

# The shared C library in flash (spec Phase 2b, R2b.9; <minix/shlibc.h>,
# tools/mkshlibc.py).  The libc-pic objects are linked once, at
# SHLIBC_BASE (after the 192 KB system image, in minix.bin), and programs
# call them through 12-byte stubs and the entry table, whose order is
# frozen in libc.exports (append only; spec J.3).  Members whose
# per-process data is big (password and group files, mtab, crypt, scanf,
# the time zone code, the float conversion tables, regexp, math) stay
# static, linked into the programs that use them, together with every
# member that needs one of them.  SHLIBC=0: every program static, as before.
SHLIBC	?= 1
SHL	:= $(MB)/shlibc
SHF	:= $(MB)/shfuri
SHLIBC_BASE := $(if $(filter flipper,$(BOARD)),0x08030000,0x00030000)
SHLIBC_STATIC := getgrent getpwent mtab crypt doscan misc ext_comp \
	   getttyent ctermid cuserid getlogin getpass popen fsversion \
	   setlocale localeconv regexp regsub printk difftime hypot \
	   asctime ctime ecvt localtime mktime strftime tzset ttyslot \
	   scanf fscanf sscanf vscanf vsscanf \
	   asynchio termcap lock tmpnam tmpfile ttyname system execlp itoa \
	   peekpoke putenv freopen cfopen strtok qsort rand \
	   lrand getopt gmtime
SHLIBC_GCC := __aeabi_uldivmod __aeabi_ldivmod	# R2d.4: once, here
LIBCP_STATIC := $(filter $(MB)/libcp/math/%.o \
	   $(foreach m,$(SHLIBC_STATIC),%/$(m).o),$(LIBCP_OBJ))
# _brksize: the library has its own (tab.S), which crtso-sh sets to _end.
LIBCP_SHARED := $(filter-out $(LIBCP_STATIC) %/brksize.S.o,$(LIBCP_OBJ))
$(SHL)/tab.S: libc.exports tools/mkshlibc.py $(MROOT)/include/minix/gzversion.h
	@mkdir -p $(dir $@)
	python3 -I tools/mkshlibc.py tab $< $@
$(SHL)/tab.o: $(SHL)/tab.S
	$(CC) $(MCFLAGS) -c $< -o $@
# No --gc-sections: every global function of the members is exported.
$(SHL)/libc.elf: $(SHL)/tab.o $(LIBCP_SHARED) ld/shlibc.ld
	@echo "  LD $@"
	@$(CC) $(ARCH) -nostdlib -nostartfiles -Wl,--no-warn-rwx-segments \
	   -Wl,-q -Wl,--defsym=SHLIBC_BASE=$(SHLIBC_BASE) -Tld/shlibc.ld \
	   -Wl,-Map=$@.map $(SHL)/tab.o $(LIBCP_SHARED) \
	   $(SHLIBC_GCC:%=-Wl,-u,%) -lgcc -o $@
$(SHL)/libc.bin $(SHL)/libc.json $(SHL)/stubs.S $(SHL)/shlibc-syms.ld &: \
		$(SHL)/libc.elf libc.exports tools/mkshlibc.py
	python3 -I tools/mkshlibc.py image $< libc.exports $(SHL)/libc.bin \
	   $(SHL)/libc.json $(SHL)/stubs.S $(SHL)/shlibc-syms.ld \
	   --name SHLIBC --max 0xC000
$(SHL)/stubs.o: $(SHL)/stubs.S
	$(CC) $(MCFLAGS) -c $< -o $@
$(MB)/libc-static.a: $(LIBCP_STATIC) minix.mk
	$(AR) rcs $@.new $^ && mv -f $@.new $@
$(MB)/user/crtso-sh.o: $(MROOT)/src/lib/arm/rts/crtso.S
	@mkdir -p $(dir $@)
	$(CC) $(MCFLAGS) $(PICFLAGS) -DSHLIBC -c $< -o $@
.PHONY: shlibc
shlibc: $(SHL)/libc.bin

#############################################################################
# Kernel
#############################################################################
KDIR	:= $(MROOT)/src/kernel
KSRC	:= main.c proc.c system.c clock.c table.c memory.c tty.c driver.c \
	   drvlib.c misc.c arm/klib.c arm/cons.c arm/shadow.c arm/nodev.c \
	   arm/disk.c arm/flipdev.c arm/lat.c arm/mpx.S arm/flip.S
# The build id (spec J.2): gzbuild.h, rewritten only when it changes.
GZBUILD	:= $(MB)/gen/gzbuild.h
.PHONY: gzbuild-check
$(GZBUILD): gzbuild-check
	@mkdir -p $(dir $@)
	@python3 -I tools/gzbuild.py $@
GZ_RELEASE = $(shell sed -n 's/.*define GZ_RELEASE[^"]*"\(.*\)".*/\1/p' \
		$(MROOT)/include/minix/gzversion.h)
KOBJ	:= $(KSRC:%=$(MB)/kernel/%.o) \
	   $(filter-out %/main.o %/string.o,$(HAL_SRC:%.c=$(BUILD)/%.o))
# (string routines come from libc.a: memops.S, ansi/memcmp.c, strlen.c)

$(MB)/kernel/%.o: $(KDIR)/%
	@mkdir -p $(dir $@)
	$(CC) $(MCFLAGS) -I$(KDIR) -MMD -MP -c $< -o $@

$(MB)/kernel.elf: $(KOBJ) $(MB)/libc.a ld/$(BOARD).ld ld/sections.ld
	$(CC) $(MLDFLAGS) -Lld -Tld/$(BOARD).ld -Wl,-Map=$(MB)/kernel.map \
	   $(KOBJ) $(MB)/libc.a -lgcc -o $@
	$(SIZE) $@

# Boot screen (shown from kernel start until the launcher takes the LCD).
SPLASH_ART  := art/galtzagorri.png
SPLASH_OPTS := --crop 240,100,900,430 --threshold 110
hal/common/splash.c: $(SPLASH_ART) tools/mksplash.py
	python3 -I tools/mksplash.py $(SPLASH_ART) $@ $(SPLASH_OPTS) \
	   --preview art/splash-preview.png

#############################################################################
# Boot servers: MM, FS and INIT.  Each links at the flash and RAM addresses
# that follow the image before it: kernel -> mm -> fs -> init.
#############################################################################
RAM_END		:= 0x20030000
STACK_mm	:= 1024	# stack (+ gap) above bss
STACK_fs	:= 2048
STACK_init	:= 8192	# heap (brk) + stack: stdio, exec arg block
MM_SRC	:= main.c forkexit.c break.c exec.c signal.c alloc.c utility.c \
	   table.c putk.c trace.c getset.c fcache.c finst.c swap.c
FS_SRC	:= main.c open.c read.c write.c pipe.c device.c path.c mount.c \
	   link.c super.c inode.c cache.c cache2.c filedes.c stadir.c \
	   protect.c time.c lock.c misc.c utility.c table.c putk.c poll.c
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

$(MB)/srv/exit.o: srv/exit.c
	@mkdir -p $(dir $@)
	$(CC) $(LIBCFLAGS) -c $< -o $@

srv_head = $(MB)/srv/$(1)/bootimg.o $(MB)/srv/crtso.o $(MB)/srv/exit.o
SRV_mm	 = $(call srv_head,mm) $(MM_SRC:%=$(MB)/mm/%.o)
SRV_fs	 = $(call srv_head,fs) $(FS_SRC:%=$(MB)/fs/%.o)
# INIT: the real one (src/tools/init.c), or INIT=m2 for the test INIT that
# runs the milestone-2 checks and exits QEMU.
INIT	?= real
ifeq ($(INIT),m2)
SRV_init = $(MB)/srv/init/bootimg.o $(MB)/srv/crtso.o $(MB)/m2/init.o	# stdio: the libc exit
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

# The system image (192 KB at most), then the shared libc at 192 KB
# (SHLIBC_BASE, 48 KB at most) and the shared libfuri + libapp at 240 KB
# (SHFURI_BASE, 48 KB at most); the installed programs start at 288 KB
# (hal_flash_layout(), R2b.1; <minix/shlibc.h>).
$(MB)/minix.bin: $(MB)/kernel.elf $(MB)/mm.elf $(MB)/fs.elf $(MB)/init.elf \
		 tools/mkflash.py $(SHL)/libc.bin $(SHF)/furi.bin
	python3 -I tools/mkflash.py --libc=0x30000,$(SHL)/libc.bin \
	   --lib=0x3C000,$(SHF)/furi.bin \
	   $(OBJCOPY) $(NM) $@ $(filter %.elf,$^)
	@test $$(stat -c %s $@) -le 294912 || \
	   { echo "$@ is over 288 KB: it would overlap the installed programs"; \
	     touch -d @0 $@; exit 1; }

# make base-flash: minix-base.bin = minix.bin with the programs used at boot
# and login built into the installed region (R2b.6, tools/mkbaseflash.py).
# Flash it with DFU like minix.bin, together with the base package or card
# of the same build; /etc/rc binds them at the next boot (pkg bind).
BASE_FLASH := sh launcher getty login ls cat cp pkg update mount printroot \
	      test rfid
# rfid: 46 KB of text with liblfrfid (spec R9.0.1); from here it runs in
# place, instead of from RAM until the run-time cache has copied it (12
# pages written at run time).
$(MB)/minix-base.bin: $(MB)/minix.bin $(BASE_FLASH:%=$(MB)/bin/%) \
		      tools/mkbaseflash.py $(MB)/rom.img
	python3 -I tools/mkbaseflash.py $< $@ \
	   $(if $(filter flipper,$(BOARD)),0x08000000,0) 0x48000 \
	   --rom=$(MB)/rom.img $(BASE_FLASH:%=$(MB)/bin/%)

# The default settings image (spec L.2): /etc as built, read-only, 32 KB;
# in minix-base.bin after the installed region's directories (/dev/rom).
$(MB)/rom.img: rootfs/rom.proto tools/mkfsimg.py $(wildcard rootfs/etc/*)
	@mkdir -p $(MB)
	python3 -I tools/mkfsimg.py rootfs/rom.proto $@ --root .
.PHONY: base-flash
base-flash: $(MB)/minix-base.bin

#############################################################################
# User programs: position-independent (ld/user-pic.ld, -Wl,-q), converted
# to MINIX a.out (separate I&D) by tools/elf2mnx.py; their text runs from
# the flash text cache (src/mm/fcache.c).
#############################################################################
# The shell, the MINIX commands (src/commands/simple, halt) and m2test.
CMDS	:= basename cat chmod chown cksum clr cmp comm cp cut date dd df \
	   dirname du echo ed expand expr factor fgrep find fold getty grep \
	   head id kill login ls mkdir mkfifo mknod mkfs mount od paste \
	   printenv printroot pwd rev rmdir sed sleep sort split strings stty \
	   passwd su sum sync tail tar tee test touch tr tty umount uname uniq update \
	   wc which whoami yes
# Commands written for the port (flipper/cmds).
PORTCMDS := mkpart pkg free gpio beep lat ver ir backlight cfcat settings rtc rfid ikey \
	    swapon gmodem
# The editor (src/commands/mined, T8): EDITOR and VISUAL in /etc/profile.
EDITCMDS := mined
# ZMODEM (src/commands/zmodem): rz.c and sz.c include rbsb.c and zm.c.
ZMODEM	:= rz sz
# Programs for the LCD and buttons (flipper/gui), linked with gui.c.
GUIAPPS	:= launcher info lights console power lfrfid
UPROGS	:= sh halt m2test $(CMDS) $(PORTCMDS) $(EDITCMDS) $(GUIAPPS)
# libfuri's tests (spec R5.1, R5.2) only on the QEMU image, where m2test
# runs them; furihello is also in the tests package (/usr/tests).
ifeq ($(BOARD),qemu)
UPROGS	+= furitest furihello gmdemo
endif
# Set-uid root programs, and the password helper of crypt(3) in /lib
# (on the root file system, so login works before /usr is mounted).
SUIDPROGS := passwd su
# Programs that hold passwords or keys: a.out flag A_NOSWAP (Secret: yes),
# never swapped to the card in plain mode (spec R12.28).  login runs as
# root without set-uid, so the set-uid rule alone would not cover it.
SECRETPROGS := login passwd su swapsecret
LIBPROGS := pwdauth
# (rz/sz are buildable - make build/<board>/minix/bin/rz - but not shipped:
# MINIX 2.0's 1990 rz does not get along with today's lrzsz; pkg get and
# tools/flsend use their own CRC-checked transfer instead.)
# Apps: built like commands but shipped as packages (flipper/apps).
APPS	:= hello gmdemo
# Other names of multi-call programs, installed as copies.
ALIASES	:= mv=cp ln=cp cpdir=cp clone=cp [=test reboot=halt chgrp=chown
ALIASES	+= $(addsuffix =cp,rm)
# Stack + heap reserve per program (MINIX chmem).  RAM is ~114 KB for all
# user processes, and a fork needs a second copy of the data, so keep these
# small: default 6 KB (tools/elf2mnx.py), more only where needed.
# Values checked with "free -t on" (MM reports the untouched part of the
# reserve at exit), 2026-11-14: keep at least ~2 KB never used.
CHMEM_sh     := 10240	# echo /bin/* (97 names) uses 5.7 KB, three such globs
			# in one command 9.5 KB (heap in 256 B steps, word lists
			# not copied, 2026-11-16); exec needs no heap
			# (MM gathers argv/envp, mm/exec.c gather())
CHMEM_update := 2048
CHMEM_tail   := 8192	# reading a pipe it keeps a 4 KB ring
CHMEM_ed     := 10240	# used 5.3 KB editing a small file
CHMEM_sort   := 12288	# mallocs its 8 KB work area
CHMEM_ls     := 12288	# ls -l /bin (83 files) used 7.4 KB
CHMEM_sed    := 10240	# R2a.5: cmds[], labels and the RE pool are malloc'd
# opendir() mallocs a 1 KB DIR per open directory level (and malloc grows
# the heap in 1 KB steps): recursive tools need room for a few levels.
CHMEM_find   := 10240	# 6 KB: "can't read directory" already at /etc
CHMEM_du     := 8192	# 6 KB: did not descend at all
CHMEM_cp     := 8192	# cpdir /etc: 1.3 KB left of 6 KB (also rm -r, mv)
# Small one-shot tools used < 1 KB of the default 6 KB.
SMALLCMDS    := basename dirname echo cat cmp date id kill backlight cfcat \
		mkdir mkfifo mknod pwd rmdir sleep sync test touch tr tty \
		uname whoami yes printenv gpio beep ver rtc
$(foreach c,$(SMALLCMDS),$(eval CHMEM_$(c) := 4096))
CHMEM_settings := 3072	# K.7: a group screen; stdio for its one file
CHMEM_m2test := 16384
CHMEM_furitest := 16384	# 3 KB main stack + up to 3 thread stacks of 1 KB + heap
CHMEM_furihello := 12288	# main stack + gui thread 2 KB + heap (Canvas 1.1 KB, views)
CHMEM_mined  := 12288	# T8 follow-up: one malloc per line; ~1.6 B per file byte:
			# files to ~7 KB (~3.5 KB copying all of it); out of memory -> <file>.save
CHMEM_pkg    := 8192	# exec needs heap for the argument block
CHMEM_swapper := 32768	# tests/swapper: a big image (swap, AC12.1)
CHMEM_swapsecret := 32768
CHMEM_swapsmall := 12288	# tests/swapper as a small parked app (AC12.10)
CHMEM_swaptime := 8192	# tests/swaptime: the timer side
CHMEM_swaptime64 := 65536	# its helper: a 64 KB image to swap (#109)
CHMEM_launcher := 4096
CHMEM_gmdemo := 2560	# K.8 window app: 1 KB of stack (two 128 B buffers),
			# no heap, MM's 1.5 KB gap
CHMEM_console  := 4096

SH_SRC	:= sh1.c sh2.c sh3.c sh4.c sh5.c sh6.c
MINED_SRC := mined1.c mined2.c
$(MB)/cmd/mined/%.o: $(MROOT)/src/commands/mined/%
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@
# The shell without -fdata-sections: its static variables then share GOT
# entries (section anchors), 48 B less RAM per shell process.
$(MB)/cmd/sh/%.o: $(MROOT)/src/commands/sh/%
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -fno-data-sections -MMD -MP -c $< -o $@
$(MB)/cmd/reboot/%.o: $(MROOT)/src/commands/reboot/%
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@
$(MB)/cmd/m2test.o: m2/m2test.c
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@
$(MB)/cmd/zmodem/%.o: $(MROOT)/src/commands/zmodem/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -DPOSIX -MMD -MP -c $< -o $@
$(MB)/cmd/port/%.o: cmds/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@
$(MB)/cmd/%.o: $(MROOT)/src/commands/simple/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@

OBJS_sh	 = $(SH_SRC:%=$(MB)/cmd/sh/%.o)
OBJS_mined = $(MINED_SRC:%=$(MB)/cmd/mined/%.o)
OBJS_halt = $(MB)/cmd/reboot/halt.c.o $(MB)/cmd/reboot/log.c.o
$(foreach c,$(PORTCMDS),$(eval OBJS_$(c) = $(MB)/cmd/port/$(c).o))
$(foreach c,$(ZMODEM),$(eval OBJS_$(c) = $(MB)/cmd/zmodem/$(c).o))
$(foreach c,$(GUIAPPS),$(eval OBJS_$(c) = $(MB)/cmd/gui/$(c).o $(MB)/cmd/gui/gui.o))
OBJS_launcher += $(MB)/cmd/gui/appline.o $(MB)/cmd/gui/winsrv.o $(MB)/cmd/gui/winrec.o
# libgm (spec K.8, src/lib/gm): window apps describe their screens, the
# launcher draws them (gui/winsrv.c).  Static, one object: no malloc, no
# stdio; programs link it with --gc-sections.
GM_DIR	:= $(MROOT)/src/lib/gm
$(MB)/gm/gm.o: $(GM_DIR)/gm.c $(GM_DIR)/gm.h
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@
$(MB)/cmd/app/gmdemo.o: apps/gmdemo.c $(GM_DIR)/gm.h
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -I$(GM_DIR) -MMD -MP -c $< -o $@
# settings GROUP (spec K.7): the groups' files are src/commands/settings/
SETGROUPS := display sound
OBJS_settings = $(MB)/cmd/port/settings.o $(SETGROUPS:%=$(MB)/cmd/settings/%.o) \
		$(MB)/cmd/gui/gui.o
$(MB)/cmd/settings/%.o: $(MROOT)/src/commands/settings/%.c \
		$(MROOT)/src/commands/settings/sgroup.h
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@
# liblfrfid (spec R9.0.1, src/lib/lfrfid): the Unleashed LF RFID protocols,
# linked into rfid (code in flash; the decoders' state is malloc'd only
# while a read runs).  Third-party C11 code: built quietly (-w).
LFRFID_DIR := $(MROOT)/src/lib/lfrfid
LFRFID_C := $(shell cd $(LFRFID_DIR) && find . -name '*.c' | sort)
LFRFID_OBJ := $(patsubst ./%.c,$(MB)/lfrfid/%.o,$(LFRFID_C))
LFRFID_HDR := $(shell find $(LFRFID_DIR) -name '*.h')
LFRFID_CFLAGS = $(filter-out -std=gnu89,$(UCFLAGS)) -std=gnu11 \
	    -I$(LFRFID_DIR) -I$(LFRFID_DIR)/shim
$(MB)/lfrfid/%.o: $(LFRFID_DIR)/%.c $(LFRFID_HDR)
	@mkdir -p $(dir $@)
	@$(CC) $(LFRFID_CFLAGS) -w -c $< -o $@
$(MB)/cmd/port/rfid.o: cmds/rfid.c $(LFRFID_HDR)
	@mkdir -p $(dir $@)
	@$(CC) $(LFRFID_CFLAGS) -MMD -MP -c $< -o $@
OBJS_rfid = $(MB)/cmd/port/rfid.o $(LFRFID_OBJ)
# libmodem (spec section P, src/lib/modem): the link layer, the RFID line
# code and the Sub-GHz presets and legal tables, linked into gmodem (not
# MINIX's modem(1), src/commands/simple/modem.c, which the port leaves out).
MODEM_DIR := $(MROOT)/src/lib/modem
# heatshrink (ISC, src/lib/modem/heatshrink) is C99: gnu11 and quiet.
MODEM_C	:= $(notdir $(wildcard $(MODEM_DIR)/*.c))
MODEM_HS := $(notdir $(wildcard $(MODEM_DIR)/heatshrink/*.c))
MODEM_OBJ := $(MODEM_C:%.c=$(MB)/modem/%.o) $(MODEM_HS:%.c=$(MB)/modem/hs/%.o)
MODEM_CFLAGS := $(filter-out -std=gnu89,$(UCFLAGS)) -std=gnu11 -I$(MODEM_DIR)
$(MB)/modem/%.o: $(MODEM_DIR)/%.c $(MODEM_DIR)/modem.h
	@mkdir -p $(dir $@)
	@$(CC) $(MODEM_CFLAGS) -MMD -MP -c $< -o $@
$(MB)/modem/hs/%.o: $(MODEM_DIR)/heatshrink/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(MODEM_CFLAGS) -w -MMD -MP -c $< -o $@
$(MB)/cmd/port/gmodem.o: cmds/gmodem.c $(MODEM_DIR)/modem.h
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -I$(MODEM_DIR) -MMD -MP -c $< -o $@
OBJS_gmodem = $(MB)/cmd/port/gmodem.o $(MODEM_OBJ)
$(MB)/cmd/gui/%.o: gui/%.c gui/gui.h gui/appline.h
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@
objs_of	 = $(if $(OBJS_$(1)),$(OBJS_$(1)),$(MB)/cmd/$(1).o)

ifeq ($(SHLIBC),1)
# Against the shared libc: its stubs, then the static members it lacks.
define user_prog
$(MB)/user/$(1).elf: $(call objs_of,$(1)) $(MB)/user/crtso-sh.o \
		$(SHL)/stubs.o $(MB)/libc-static.a ld/user-shlibc.ld \
		$(SHL)/shlibc-syms.ld
	@mkdir -p $$(dir $$@)
	@$(CC) $(MLDFLAGS) -Wl,-q -L$(SHL) -Tld/user-shlibc.ld \
	   -Wl,-Map=$$@.map $(MB)/user/crtso-sh.o $(call objs_of,$(1)) \
	   $(SHL)/stubs.o $(MB)/libc-static.a -lgcc -o $$@
endef
ELF2MNX_LIBC := --libc $(SHL)/libc.json
# Against both shared libraries (FURIPROGS): the furi stubs first.
define furi_prog
$(MB)/user/$(1).elf: $(call objs_of,$(1)) $(MB)/user/crtso-sh.o \
		$(SHF)/stubs.o $(SHL)/stubs.o $(MB)/libc-static.a \
		ld/user-shfuri.ld $(SHL)/shlibc-syms.ld $(SHF)/shfuri-syms.ld
	@mkdir -p $$(dir $$@)
	@$(CC) $(MLDFLAGS) -Wl,-q -L$(SHL) -L$(SHF) -Tld/user-shfuri.ld \
	   -Wl,-Map=$$@.map $(MB)/user/crtso-sh.o $(call objs_of,$(1)) \
	   $(SHF)/stubs.o $(SHL)/stubs.o $(MB)/libc-static.a -lgcc -o $$@
endef
ELF2MNX_FURI = $(if $(filter $*,$(FURIPROGS)),--furi $(SHF)/furi.json)
else
furi_prog = $(call user_prog,$(1))
define user_prog
$(MB)/user/$(1).elf: $(call objs_of,$(1)) $(MB)/user/crtso.o $(MB)/libc-pic.a \
		ld/user-pic.ld
	@mkdir -p $$(dir $$@)
	@$(CC) $(MLDFLAGS) -Wl,-q -Tld/user-pic.ld -Wl,-Map=$$@.map \
	   $(MB)/user/crtso.o $(call objs_of,$(1)) $(MB)/libc-pic.a -lgcc -o $$@
endef
endif
$(foreach c,$(APPS),$(eval OBJS_$(c) = $(MB)/cmd/app/$(c).o))
OBJS_gmdemo += $(MB)/gm/gm.o	# libgm, statically (spec K.8)
# libfuri core (spec R5.1, flipper/lib/furi): the stock furi API for apps
# rebuilt from source.  In the shared furi library (R2b.9, AC5.3, below);
# furitest links the core statically.  C99 (the stock headers need it).
FURI_DIR := lib/furi
FURI_OBJ := $(addprefix $(MB)/furi/,thread.o sync.o timer.o record.o \
	    string.o check.o fdwait.o port_minix.o ctx_arm.o)
# libfuri GUI (spec R5.2): Canvas on u8g2, Gui, ViewDispatcher, modules.
# Programs link with --gc-sections: only what an app calls is in flash.
FURI_GUI_C := $(wildcard $(FURI_DIR)/src/gui/*.c $(FURI_DIR)/src/gui/modules/*.c \
	    $(FURI_DIR)/src/gui/modules/widget_elements/*.c $(FURI_DIR)/u8g2/*.c)
FURI_GUI_OBJ := $(patsubst $(FURI_DIR)/%.c,$(MB)/furi/%.o,$(FURI_GUI_C))
FURI_HDR := $(wildcard $(FURI_DIR)/include/*.h $(FURI_DIR)/include/*/*.h \
	    $(FURI_DIR)/include/gui/*/*.h $(FURI_DIR)/include/gui/modules/*/*.h \
	    $(FURI_DIR)/src/*.h $(FURI_DIR)/src/gui/*.h $(FURI_DIR)/src/gui/*/*/*.h \
	    $(FURI_DIR)/u8g2/*.h)
FURI_CFLAGS = $(UCFLAGS) -std=gnu99 -I$(FURI_DIR)/include
FURI_GUI_CFLAGS = $(FURI_CFLAGS) -I$(FURI_DIR)/include/gui \
	    -I$(FURI_DIR)/include/gui/modules -I$(FURI_DIR)/u8g2 -I$(FURI_DIR)/src/gui
$(MB)/furi/%.o: $(FURI_DIR)/src/%.c $(FURI_HDR)
	@mkdir -p $(dir $@)
	@$(CC) $(FURI_CFLAGS) -MMD -MP -c $< -o $@
$(MB)/furi/src/gui/%.o: $(FURI_DIR)/src/gui/%.c $(FURI_HDR)
	@mkdir -p $(dir $@)
	@$(CC) $(FURI_GUI_CFLAGS) -MMD -MP -c $< -o $@
$(MB)/furi/u8g2/%.o: $(FURI_DIR)/u8g2/%.c $(FURI_HDR)
	@mkdir -p $(dir $@)
	@$(CC) $(FURI_GUI_CFLAGS) -MMD -MP -c $< -o $@
$(MB)/furi/%.o: $(FURI_DIR)/src/%.S
	@mkdir -p $(dir $@)
	@$(CC) $(MCFLAGS) -c $< -o $@
$(MB)/libfuri.a: $(FURI_OBJ) $(FURI_GUI_OBJ)
	$(AR) rcs $@.new $^ && mv -f $@.new $@
.PHONY: libfuri
libfuri: $(MB)/libfuri.a
$(MB)/cmd/furitest.o: $(FURI_DIR)/test_furi.c $(MB)/libfuri.a
	@mkdir -p $(dir $@)
	@$(CC) $(FURI_CFLAGS) -MMD -MP -c $< -o $@
OBJS_furitest = $(MB)/cmd/furitest.o $(FURI_OBJ)
# libapp (spec F.2, R7b.4, flipper/lib/app): timers, notifications,
# settings for native apps.  In the shared furi library with libfuri
# (R2b.9, below).  timed and alert (flipper/apps/timed) use it; they ship
# as the package timed (/usr/lib).
APP_DIR := lib/app
APP_OBJ := $(MB)/app/tspec.o $(MB)/app/io.o $(MB)/app/tune.o
APP_CFLAGS = $(UCFLAGS) -I$(APP_DIR)/include
$(MB)/app/%.o: $(APP_DIR)/src/%.c $(APP_DIR)/include/app.h
	@mkdir -p $(dir $@)
	@$(CC) $(APP_CFLAGS) -MMD -MP -c $< -o $@
$(MB)/libapp.a: $(APP_OBJ)
	$(AR) rcs $@.new $^ && mv -f $@.new $@
.PHONY: libapp
libapp: $(MB)/libapp.a
# The shared libfuri + libapp library in flash (spec R2b.9, R5.2, 7b; the
# "furi" library, <minix/shlibc.h>, ld/shfuri.ld, tools/mkshlibc.py): one
# copy of libfuri (core, GUI, u8g2, fonts) and libapp at SHFURI_BASE, after
# the shared libc, in minix.bin.  Only the functions the apps call are
# exported (furi.exports, frozen order, append only; spec J.3), and only
# what they reach is linked (--gc-sections).  Its per-process data (the
# furi area) follows the libc area in the data of the programs that use
# it (FURIPROGS, ld/user-shfuri.ld); the others do not carry it.
SHFURI_BASE := $(if $(filter flipper,$(BOARD)),0x0803C000,0x0003C000)
SHFURI_LIBOBJ := $(FURI_OBJ) $(FURI_GUI_OBJ) $(APP_OBJ)
$(SHF)/tab.S: furi.exports tools/mkshlibc.py $(MROOT)/include/minix/gzversion.h
	@mkdir -p $(dir $@)
	python3 -I tools/mkshlibc.py tab $< $@ --furi
$(SHF)/tab.o: $(SHF)/tab.S
	$(CC) $(MCFLAGS) -c $< -o $@
$(SHF)/furi.elf: $(SHF)/tab.o $(SHFURI_LIBOBJ) $(SHL)/stubs.o \
		$(MB)/libc-static.a $(SHL)/shlibc-syms.ld ld/shfuri.ld
	@echo "  LD $@"
	@$(CC) $(ARCH) -nostdlib -nostartfiles -Wl,--no-warn-rwx-segments \
	   -Wl,--gc-sections -Wl,-q -L$(SHL) \
	   -Wl,--defsym=SHFURI_BASE=$(SHFURI_BASE) -Tld/shfuri.ld \
	   -Wl,-Map=$@.map $(SHF)/tab.o $(SHFURI_LIBOBJ) $(SHL)/stubs.o \
	   $(MB)/libc-static.a -lgcc -o $@
$(SHF)/furi.bin $(SHF)/furi.json $(SHF)/stubs.S $(SHF)/shfuri-syms.ld &: \
		$(SHF)/furi.elf furi.exports tools/mkshlibc.py $(SHL)/libc.json
	python3 -I tools/mkshlibc.py image $< furi.exports $(SHF)/furi.bin \
	   $(SHF)/furi.json $(SHF)/stubs.S $(SHF)/shfuri-syms.ld \
	   --dep $(SHL)/libc.json --name SHFURI --max 0xC000
$(SHF)/stubs.o: $(SHF)/stubs.S
	$(CC) $(MCFLAGS) -c $< -o $@
.PHONY: shfuri
shfuri: $(SHF)/furi.bin
# Programs linked against it; with SHLIBC=0 they link both statically.
FURIPROGS := furihello remind timed alert
FURI_LINK := $(if $(filter 1,$(SHLIBC)),,$(SHFURI_LIBOBJ))
TIMEDPROGS := timed alert
$(MB)/cmd/timed/%.o: apps/timed/%.c $(APP_DIR)/include/app.h
	@mkdir -p $(dir $@)
	@$(CC) $(APP_CFLAGS) -Igui -MMD -MP -c $< -o $@
OBJS_timed = $(MB)/cmd/timed/timed.o $(FURI_LINK)
OBJS_alert = $(MB)/cmd/timed/alert.o $(MB)/cmd/gui/gui.o $(FURI_LINK)
CHMEM_timed := 5120	# R7b.4: about 3 KB while it runs; 7b: struct app_timer
			# grew (Tone:, Log:), 4 KB left opendir() no heap under INIT
CHMEM_alert := 6144	# gui, poll, the 1 KB picture under it (R7b.2); timedtest memuse
# The native app remind (spec 7b.1, F.4): libfuri GUI, libapp timers;
# package remind (/usr/bin, own user).  alarm: WINPROGS below.
NATPROGS := remind
NAT_CFLAGS = $(FURI_GUI_CFLAGS) -I$(FURI_DIR) -I$(APP_DIR)/include -Iapps/native
$(MB)/cmd/nat/%.o: apps/%.c $(FURI_HDR) $(APP_DIR)/include/app.h apps/native/nat.h
	@mkdir -p $(dir $@)
	@$(CC) $(NAT_CFLAGS) -MMD -MP -c $< -o $@
$(foreach p,$(NATPROGS),$(eval OBJS_$(p) = $(MB)/cmd/nat/$(p)/$(p).o \
	$(MB)/cmd/nat/native/nat_core.o $(FURI_LINK)))
# chmem: libfuri's main stack 3 KB and gui thread 2 KB (#188), the heap
# (Canvas 1.1 KB, views, the lists) and MM's 1.5 KB safety gap above it.
# nattest (3 runs, 12 KB): never used remind 3.8-4.0 KB, alarm 3.2-3.4 KB;
# what is left keeps at least 1 KB for the heap to grow (issue #201).
CHMEM_remind := 11264	# 7b.1
# Ordua (alarm) is a window app (spec K.8, T25b): libgm, libapp and
# nat_core statically, no libfuri.  No malloc (opendir() is not used),
# no stdio; chmem: the stack (timer_add() keeps 2 x 512 B on it while it
# saves) and MM's 1.5 KB gap.
WINPROGS := alarm
$(MB)/cmd/win/%.o: apps/%.c $(APP_DIR)/include/app.h apps/native/nat.h \
		$(GM_DIR)/gm.h
	@mkdir -p $(dir $@)
	@$(CC) $(APP_CFLAGS) -I$(GM_DIR) -Iapps/native -MMD -MP -c $< -o $@
OBJS_alarm = $(MB)/cmd/win/alarm/alarm.o $(MB)/cmd/nat/native/nat_core.o \
	$(MB)/gm/gm.o $(APP_OBJ)
CHMEM_alarm  := 2048	# 7b.2, K.8
$(MB)/cmd/furihello.o: $(FURI_DIR)/hello.c $(MB)/libfuri.a
	@mkdir -p $(dir $@)
	@$(CC) $(FURI_GUI_CFLAGS) -I$(FURI_DIR) -MMD -MP -c $< -o $@
OBJS_furihello = $(MB)/cmd/furihello.o $(FURI_LINK)
# Security tests (flipper/tests, spec rule 0.4), package tests: /usr/tests.
TESTS	:= sigret unaligned badreloc fcpoison mpufrag abiexec fccheck \
	   pkgbad pkgplant sticky pwdcheck swapper swapsecret swapsmall \
	   swaptime swaptime64 regtest
$(foreach c,$(TESTS),$(eval OBJS_$(c) = $(MB)/cmd/test/$(c).o))
ifneq ($(BOARD),qemu)	# (QEMU: in UPROGS)
TESTS	+= furihello	# libfuri GUI (R5.2): OBJS_furihello above
endif
# swapsecret: swapper built with Secret: yes (A_NOSWAP, R12.28, AC12.13)
OBJS_swapsecret = $(MB)/cmd/test/swapper.o
# swapsmall: the same, a small image (placement, AC12.10)
OBJS_swapsmall = $(MB)/cmd/test/swapper.o
# swaptime64: swaptime's helper with a 64 KB image (swap-in time, #109)
OBJS_swaptime64 = $(MB)/cmd/test/swaptime.o
# regchild (issue #160): regtest's child, its own entry before crtso so
# it sees the registers exactly as exec left them (regstart.S).
$(MB)/cmd/test/regstart.o: tests/regstart.S
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -c $< -o $@
$(MB)/user/regchild.elf: $(MB)/cmd/test/regstart.o $(MB)/cmd/test/regtest.o \
		$(SHL)/stubs.o $(MB)/libc-static.a ld/user-shlibc.ld $(SHL)/shlibc-syms.ld
	@mkdir -p $(dir $@)
	@$(CC) $(MLDFLAGS) -Wl,-q -L$(SHL) -Tld/user-shlibc.ld \
	   -Wl,-Map=$@.map $(MB)/cmd/test/regstart.o $(MB)/cmd/test/regtest.o \
	   $(SHL)/stubs.o $(MB)/libc-static.a -lgcc -o $@
$(MB)/cmd/test/%.o: tests/%.c tests/t.h
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@
# The hardware self test (spec 7b.3): the app and its modules, package
# stest (/usr/bin/selftest, /usr/lib/selftest/*; a short name: tar names
# of packages are at most 13 characters).
ST_MODS	:= board memory flash input av pins	# module names must not be base commands (gpio!)
STPROGS	:= selftest $(ST_MODS)
# Programs share one build directory: a module named like a base command
# would replace it (it happened with gpio).
$(if $(filter $(STPROGS),$(UPROGS) $(LIBPROGS) $(TESTS) $(TIMEDPROGS) $(APPS)),\
  $(error selftest: $(filter $(STPROGS),$(UPROGS) $(LIBPROGS) $(TESTS) $(TIMEDPROGS) $(APPS)) also names another program))
$(MB)/cmd/selftest/%.o: apps/selftest/%.c apps/selftest/st.h gui/gui.h
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -Igui -MMD -MP -c $< -o $@
$(foreach p,$(STPROGS),$(eval OBJS_$(p) = $(MB)/cmd/selftest/$(p).o $(MB)/cmd/selftest/st.o $(MB)/cmd/gui/gui.o))
CHMEM_selftest := 5120	# opendir() mallocs a 1 KB DIR, and MM keeps 1.5 KB
			# (SAFETY_BYTES) between heap and stack: 3 KB failed on the device
CHMEM_board := 3072	# 2 x 512 B buffers (static), stdio
CHMEM_input := 2048
CHMEM_av := 2048
CHMEM_pins := 2048
CHMEM_memory := 2048
CHMEM_flash := 4096	# SHA-256 work space, opendir() (1 KB) when naming one
$(foreach p,$(filter-out $(FURIPROGS),$(sort $(UPROGS) $(APPS)) $(LIBPROGS) $(TESTS) $(TIMEDPROGS) $(STPROGS) $(NATPROGS) $(WINPROGS)),$(eval $(call user_prog,$(p))))
$(foreach p,$(FURIPROGS),$(eval $(call furi_prog,$(p))))
$(MB)/cmd/app/%.o: apps/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@

# Packages (tools/mkpkg.py), shipped in /lib/pkg on the root file system.
PKGDIR	:= $(MB)/pkgs
PKGS	:= $(PKGDIR)/hello_1.0.tar $(PKGDIR)/tests_1.0.tar $(PKGDIR)/timed_1.2.tar \
	   $(PKGDIR)/stest_1.0.tar $(PKGDIR)/gmdemo_1.0.tar \
	   $(PKGDIR)/remind_1.0.tar $(PKGDIR)/alarm_1.1.tar
# Packages pkg must refuse (AC3.4, tests/pkgbad.c): data of the tests one.
BADPKGS	:= $(foreach b,dep long dotdot name abs link,$(PKGDIR)/bad/$(b).tar)
$(BADPKGS) &: tools/mkbadpkg.py
	python3 -I tools/mkbadpkg.py $(PKGDIR)/bad
$(PKGDIR)/tests_1.0.tar: $(TESTS:%=$(MB)/bin/%) $(MB)/bin/regchild $(BADPKGS) tools/mkpkg.py
	python3 -I tools/mkpkg.py -n tests -v 1.0 -o $(PKGDIR) \
	   -D "Security tests (spec Phases 1 and 3)" \
	   $(foreach t,$(TESTS),/usr/tests/$(t)=$(MB)/bin/$(t)$(COMMA)755) \
	   /usr/tests/regchild=$(MB)/bin/regchild$(COMMA)755 \
	   $(foreach b,$(BADPKGS),/usr/tests/bad/$(notdir $(b))=$(b)$(COMMA)644)
$(PKGDIR)/hello_1.0.tar: $(MB)/bin/hello tools/mkpkg.py
	python3 -I tools/mkpkg.py -n hello -v 1.0 -o $(PKGDIR) \
	   -D "Kaixo! The first Galtzagorriak app" /usr/bin/hello=$<
# Timers and alerts (spec F.2, R7b.4): INIT runs /usr/lib/timed.
$(PKGDIR)/timed_1.2.tar: $(TIMEDPROGS:%=$(MB)/bin/%) tools/mkpkg.py \
		rootfs/timed.postinst
	python3 -I tools/mkpkg.py -n timed -v 1.2 -o $(PKGDIR) \
	   --postinst rootfs/timed.postinst \
	   -D "Timers and alerts for apps (timed, alert)" \
	   $(foreach p,$(TIMEDPROGS),/usr/lib/$(p)=$(MB)/bin/$(p)$(COMMA)755)
# The native apps (spec 7b.1, 7b.2): their own users, the launcher line.
$(PKGDIR)/remind_1.0.tar: $(MB)/bin/remind tools/mkpkg.py rootfs/remind.postinst \
		rootfs/remind.prerm
	python3 -I tools/mkpkg.py -n remind -v 1.0 -d timed -o $(PKGDIR) \
	   --timers --notify reminder --flash \
	   --postinst rootfs/remind.postinst --prerm rootfs/remind.prerm \
	   -D "Interval reminder: water, stretching, ..." /usr/bin/remind=$<
# Ordua: a window app (spec K.8, T25b), ended on Back (Background: exit).
$(PKGDIR)/alarm_1.1.tar: $(MB)/bin/alarm tools/mkpkg.py rootfs/alarm.postinst \
		rootfs/alarm.prerm
	python3 -I tools/mkpkg.py -n alarm -v 1.1 -d timed -o $(PKGDIR) \
	   --timers --notify alarm --flash --window --background exit \
	   -i @alarm --postinst rootfs/alarm.postinst --prerm rootfs/alarm.prerm \
	   -D "Ordua: alarm clock with a desk clock (window app)" /usr/bin/alarm=$<
# The example window app (spec K.8): the program, its launcher line, and
# its source with libgm in /usr/share/examples/gui.
$(PKGDIR)/gmdemo_1.0.tar: $(MB)/bin/gmdemo tools/mkpkg.py rootfs/gmdemo.postinst \
		rootfs/gmdemo.prerm apps/gmdemo.c $(GM_DIR)/gm.c $(GM_DIR)/gm.h
	python3 -I tools/mkpkg.py -n gmdemo -v 1.0 -o $(PKGDIR) --window --flash \
	   -i @app --postinst rootfs/gmdemo.postinst --prerm rootfs/gmdemo.prerm \
	   -D "Example window app (spec K.8): the launcher draws it" \
	   /usr/bin/gmdemo=$< \
	   /usr/share/examples/gui/gmdemo.c=apps/gmdemo.c \
	   /usr/share/examples/gui/gm.c=$(GM_DIR)/gm.c \
	   /usr/share/examples/gui/gm.h=$(GM_DIR)/gm.h
# The hardware self test (spec 7b.3).
$(PKGDIR)/stest_1.0.tar: $(STPROGS:%=$(MB)/bin/%) tools/mkpkg.py \
		rootfs/selftest.postinst rootfs/selftest.prerm \
		$(ST_MODS:%=rootfs/selftest/%.d)
	python3 -I tools/mkpkg.py -n stest -v 1.0 -o $(PKGDIR) \
	   --postinst rootfs/selftest.postinst --prerm rootfs/selftest.prerm \
	   -D "Hardware self test (spec 7b.3)" \
	   /usr/bin/selftest=$(MB)/bin/selftest$(COMMA)755 \
	   $(foreach m,$(ST_MODS),/usr/lib/selftest/$(m)=$(MB)/bin/$(m)$(COMMA)755) \
	   $(foreach m,$(ST_MODS),/usr/lib/selftest/$(m).d=rootfs/selftest/$(m).d$(COMMA)644)
# The system programs as a package, to update a running Flipper over the
# cable (tools/flsend build/flipper/minix/pkgs/base_$(BASE_VER).tar) instead
# of rewriting the card.  Raise BASE_VER for every update sent.
BASE_VER := 4.12
BASEPKG	:= $(PKGDIR)/base_$(BASE_VER).tar
$(BASEPKG): $(UPROGS:%=$(MB)/bin/%) $(LIBPROGS:%=$(MB)/bin/%) tools/mkpkg.py \
	    rootfs/base.postinst
	python3 -I tools/mkpkg.py -n base -v $(BASE_VER) -o $(PKGDIR) \
	   --postinst rootfs/base.postinst \
	   --os $(GZ_RELEASE) \
	   -D "Galtzagorriak MINIX system programs" \
	   $(foreach p,$(UPROGS),/bin/$(p)=$(MB)/bin/$(p)$(if $(filter $(p),$(SUIDPROGS)),$(COMMA)4755)) \
	   $(foreach p,$(LIBPROGS),/lib/$(p)=$(MB)/bin/$(p)$(COMMA)4755) \
	   $(foreach a,$(ALIASES),/bin/$(word 1,$(subst =, ,$(a)))=$(MB)/bin/$(word 2,$(subst =, ,$(a))))
.PHONY: base
base: $(BASEPKG)
.PHONY: pkgs
pkgs: $(PKGS)

# Convert ELF -> MINIX a.out with the chmem value above (default 6 KB).
$(MB)/bin/%: $(MB)/user/%.elf tools/elf2mnx.py minix.mk \
		$(if $(filter 1,$(SHLIBC)),$(SHL)/libc.json)
	@mkdir -p $(dir $@)
	@python3 -I tools/elf2mnx.py $< $@ $(if $(CHMEM_$*),--stack $(CHMEM_$*)) \
	   $(if $(filter $*,$(SECRETPROGS)),--secret) $(ELF2MNX_LIBC) \
	   $(ELF2MNX_FURI)
$(if $(filter 1,$(SHLIBC)),$(FURIPROGS:%=$(MB)/bin/%)): $(SHF)/furi.json

# Root file system: rootfs/proto -> fs.img (dd to the SD card partition)
# and disk.img (MBR + partition 1, the QEMU "SD card").  @BINFILES@ in the
# proto becomes one entry per program.
$(MB)/proto: rootfs/proto minix.mk tools/mkproto.py $(PKGS)
	@mkdir -p $(MB)
	python3 -I tools/mkproto.py $< $@ $(MB)/bin "$(UPROGS)" "$(ALIASES)" \
	   "$(PKGS)" "$(SUIDPROGS)"
# The QEMU disk is as big as the SD card (sparse), so the first boot can
# partition it like the real card.
$(MB)/fs.img $(MB)/disk.img: $(MB)/proto tools/mkfsimg.py \
		$(wildcard rootfs/etc/*) $(UPROGS:%=$(MB)/bin/%) $(LIBPROGS:%=$(MB)/bin/%)
	python3 -I tools/mkfsimg.py $(MB)/proto $(MB)/fs.img --root . \
	   --mbr $(MB)/disk.img --disk-mb $(CARD_MB)

# SD card image: MBR + the 32 MB root partition, trimmed to the part in use.
# Write it to the WHOLE card (dd of=/dev/sdX); the first boot adds /usr,
# swap and /home in the rest (/etc/setup).
CARD_MB	?= 3800
$(MB)/card.img: $(MB)/proto tools/mkfsimg.py \
		$(wildcard rootfs/etc/*) $(UPROGS:%=$(MB)/bin/%) $(LIBPROGS:%=$(MB)/bin/%)
	python3 -I tools/mkfsimg.py $(MB)/proto $(MB)/root.img --root . --trim \
	   --mbr $@

.PHONY: minix minix-run minix-flash libc disk card ramcheck latcheck

# The latency trace build (spec R0.8e): the same system with the kernel's
# event ring (arm/lat.c); flash it, then run lat on the device.
latcheck:
	$(MAKE) LATTRACE=1 BOARD=$(BOARD) minix base
	@echo "Latency build: build/$(BOARD)-lat/minix/minix.bin and its base"
	@echo "package; flash both, then run 'lat -w 30' while testing."

# Static RAM of the system images against ram-budget.txt (spec rule 0.3).
# Version numbers and CHANGELOG.md agree (spec J.4); run with ramcheck.
.PHONY: versioncheck
versioncheck:
	@python3 -I tools/versioncheck.py $(MROOT)/include/minix/gzversion.h \
	   $(MROOT)/CHANGELOG.md
# The Year-2038 audit (spec R2d.7): no time in an int or a long in the
# sources that are built; run with ramcheck and make check.
TIMECHECK_SRC := $(CMDS:%=$(MROOT)/src/commands/simple/%.c) \
	   $(MROOT)/src/commands/sh $(MROOT)/src/commands/mined \
	   $(MROOT)/src/commands/reboot/halt.c $(MROOT)/src/commands/reboot/log.c \
	   cmds gui apps lib/app tests m2 $(MROOT)/src/tools/init.c \
	   $(LIBDIRS:%=$(MROOT)/src/lib/%) $(MROOT)/include
.PHONY: timecheck check
timecheck:
	@python3 -I tools/timecheck.py $(TIMECHECK_SRC)
check: versioncheck timecheck
ramcheck: $(MB)/minix.bin versioncheck timecheck bsscheck
	@python3 -I tools/ramcheck.py ram-budget.txt $(MB)/kernel.elf \
	   $(MB)/mm.elf $(MB)/fs.elf $(MB)/init.elf
# No base program above 4 KB of .bss except those in bss-exceptions.txt
# (spec R2a.5); run with ramcheck.
.PHONY: bsscheck
bsscheck: $(UPROGS:%=$(MB)/user/%.elf) $(LIBPROGS:%=$(MB)/user/%.elf)
	@python3 -I tools/bsscheck.py bss-exceptions.txt 4096 $^
card: $(MB)/card.img
disk: $(MB)/disk.img
minix: $(MB)/minix.bin
libc: $(MB)/libc.a

QEMU_MINIX := qemu-system-arm -M mps2-an386 -nographic -monitor none \
	   -serial stdio -semihosting-config enable=on,target=native,userspace=on

minix-run: $(MB)/minix.bin $(MB)/disk.img
	@test "$(BOARD)" = qemu || { echo "minix-run needs BOARD=qemu"; exit 1; }
	timeout 90 $(QEMU_MINIX) -device loader,file=$<,addr=0

minix-flash: $(MB)/minix.bin
	@test "$(BOARD)" = flipper || { echo "minix-flash needs BOARD=flipper"; exit 1; }
	dfu-util -a 0 -s 0x08000000:leave -D $<

-include $(shell find $(MB) -name '*.d' 2>/dev/null)

# Objects that print the build id.
$(MB)/kernel/tty.c.o $(MB)/cmd/port/ver.o: $(GZBUILD)
