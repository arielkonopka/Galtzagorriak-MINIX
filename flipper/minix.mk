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
# R5.7: code that uses the FPU (float in hardware; double stays in software,
# the FPU is single precision).  softfp keeps the soft-float calling
# convention, so it links with the one libc.  The kernel saves such a
# process's FP registers on its own stack (+136 B of stack, no other RAM).
# Such a program must not use vfork: the child would run over the
# parent's saved S16-S31.  Link it with $(UFPLDFLAGS) (vfork becomes fork).
UFPFLAGS := -mfpu=fpv4-sp-d16 -mfloat-abi=softfp
UFPLDFLAGS := -Wl,--defsym=vfork=fork -Wl,--defsym=_vfork=fork
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
	   lrand getopt gmtime bread
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
	      test rfid fsck
# fsck: /etc/rc runs it before /usr is mounted (spec N.3), text in place.
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
	   fsck head id kill login ls mkdir mkfifo mknod mkfs mount od paste \
	   printenv printroot pwd rev rmdir sed sleep sort split strings stty \
	   passwd su sum sync tail tar tee test touch tr tty umount uname uniq update \
	   wc which whoami yes
# Commands written for the port (flipper/cmds).
PORTCMDS := mkpart erase pkg free gpio beep lat ver ir backlight cfcat settings rtc rfid ikey \
	    swapon gmodem guimod jiggle region gzupdate gztree radio
# setgroup: the settings groups' window app (spec K.7, K.8), below.
SETCMDS	:= setgroup
# The editor (src/commands/mined, T8): EDITOR and VISUAL in /etc/profile.
EDITCMDS := mined
# ZMODEM (src/commands/zmodem): rz.c and sz.c include rbsb.c and zm.c.
ZMODEM	:= rz sz
# Programs for the LCD and buttons (flipper/gui), linked with gui.c.
GUIAPPS	:= launcher info lights console power lfrfid spectrum infrared
# archive (issue #389): a window app (K.8), libgm statically, no gui.c.
WINBASE	:= archive nfc
# mtpd: files to a PC over MTP (spec R10.4, flipper/mtpd), below.
# rpcd: the stock RPC for the mobile app (spec R15.5, issue #388), below.
DAEMONS	:= mtpd rpcd
UPROGS	:= sh halt m2test $(CMDS) $(PORTCMDS) $(SETCMDS) $(EDITCMDS) $(GUIAPPS) $(WINBASE) \
	   $(DAEMONS)
# libfuri's tests (spec R5.1, R5.2) only on the QEMU image, where m2test
# runs them; furihello is also in the tests package (/usr/tests).
ifeq ($(BOARD),qemu)
UPROGS	+= furitest furihello gmdemo fptest guiflood spinfifo
endif
# Set-uid root programs, and the password helper of crypt(3) in /lib
# (on the root file system, so login works before /usr is mounted).
SUIDPROGS := passwd su guimod
# Set-gid programs, PROG:GID: infrared opens /dev/ir as group ir (13) when
# the user is in that group, then goes back to its own (AC7.3, issue #389).
SGIDPROGS := infrared:13 nfc:14
sgid_of = $(word 2,$(subst :, ,$(filter $(1):%,$(SGIDPROGS))))
# guimod (spec K.6, T23a): set-uid root only to write the real uid into a
# desktop record (a FIFO does not say who wrote it); root's rights end
# right after it opens /var/run/gui.
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
CHMEM_gzupdate := 8192	# check: 4 KB read buffer (heap), verifier 2 KB stack, stdio
CHMEM_gztree := 4096	# 1 KB hash buffer and SHA-512 state on the stack, stdio
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
CHMEM_tar    := 10240	# 6 KB: "cannot getcwd()" at the 2nd level, the
			# subdirectories were left out (setup -H, T27a part 8)
# Small one-shot tools used < 1 KB of the default 6 KB.
SMALLCMDS    := basename dirname echo cat cmp date id kill backlight cfcat \
		mkdir mkfifo mknod pwd rmdir sleep sync test touch tr tty \
		uname whoami yes printenv gpio beep ver rtc
$(foreach c,$(SMALLCMDS),$(eval CHMEM_$(c) := 4096))
CHMEM_settings := 4096	# diff/reset (stdio, opendir's 1 KB DIR), apply at boot
CHMEM_setgroup := 1536	# K.7/K.8 window app, ACK7.4: all stack (no heap, so no
			# MM gap: no malloc, no stdio, no signal handler);
			# deepest About ~0.9 KB (-fstack-usage) + environment
CHMEM_m2test := 16384
CHMEM_fptest := 8192
CHMEM_guiflood := 6144	# #324: all stack (sprintf), a 64 B record
CHMEM_spinfifo := 1024	# #331: EAGAIN spinners, all stack
CHMEM_furitest := 16384	# 3 KB main stack + up to 3 thread stacks of 1 KB + heap
CHMEM_furihello := 12288	# main stack + gui thread 2 KB + heap (Canvas 1.1 KB, views)
CHMEM_mined  := 12288	# T8 follow-up: one malloc per line; ~1.6 B per file byte:
			# files to ~7 KB (~3.5 KB copying all of it); out of memory -> <file>.save
CHMEM_pkg    := 8192	# exec needs heap for the argument block
CHMEM_swapper := 32768	# tests/swapper: a big image (swap, AC12.1)
CHMEM_swapsecret := 32768
CHMEM_swapsmall := 12288	# tests/swapper as a small parked app (AC12.10)
CHMEM_swaptime := 8192	# tests/swaptime: the timer side
CHMEM_swapwear := 12288	# tests/swapwear: 20 KB of .bss + this: a 32 KB image (AC12.14)
CHMEM_alarmrace := 8192	# tests/alarmrace: 6 small children (#308)
CHMEM_swaptime64 := 65536	# its helper: a 64 KB image to swap (#109)
CHMEM_fsck   := 90112	# spec N.4: fsck_mem (64 KB of maps; less if it does not
			# fit, fsck shrinks its zone window) + 24 KB (stack:
			# ~0.5 KB per directory level, stdio).  Runs at boot
			# from /etc/rc, when the RAM is free; later it may need
			# apps closed first.
CHMEM_subghz := 16384	# every decoder's state (malloc'd once, ~5 KB),
			# key files (<= 4 KB), stdio; the 2 KB frame buffer
			# is bss
CHMEM_launcher := 4096
CHMEM_gmdemo := 2560	# K.8 window app: 1 KB of stack (two 128 B buffers),
			# no heap, MM's 1.5 KB gap
CHMEM_console  := 4096
CHMEM_infrared := 8192	# issue #389: the 4 KB frame is bss; heap: libir's
			# decoders and encoders (~0.5 KB), stdio buffers for
			# the remote file (1 KB), opendir's 1 KB DIR; Learn
			# and Debug fork a reader child (a copy of the app)
CHMEM_archive := 4096	# issue #389, K.8 window app: opendir's 1 KB DIR
			# (malloc: MM's 1.5 KB gap back in), 1 KB of stack
			# (two 128 B paths, the record buffers)
CHMEM_nfc := 16384	# issue #412 (AC14.3, <= 35 KB in all): a read's card
			# (nfc_device_t 5.3 KB) and keys found (0.6 KB), the
			# loopback cards (-L, 1.6 KB), stdio for a .nfc file or
			# a dictionary (1 KB each), opendir's 1 KB DIR; all
			# malloc'd while used, freed after
CHMEM_spectrum := 2048	# issue #157: all stack (no malloc, no stdio, no
			# handler); the frame buffer and the waterfall are bss
CHMEM_rfid     := 12288	# R9.0.6 long FSK/PSK emulation: 8 KB aborted (twoflip); rfid -s: its 4 KB capture ring (R9.0.5); emulate: the protocol dictionary (1.5 KB), a 2 KB
			# pulse buffer and 3 encoder states for the period
			# search; 6 KB aborted with "out of memory" (twoflip)
CHMEM_mtpd     := 2048	# R10.4 (<= 8 KB running, with data + bss):
			# all stack (no heap: its DIR is .bss, no stdio, no
			# handler, so no MM gap); deepest a folder delete
			# (rm_tree's 256 B path) or a move (two paths)
CHMEM_rpcd     := 16384	# issue #388: the session (rpc_state_size(),
			# 6272 B) and nanopb's request strings (a 1 KB ping,
			# a 255 B path) from the heap, stdio, stack
CHMEM_jiggle   := 768	# R11.3.6: <= 1 KB with data + bss (188 B); all
			# stack: argv, environment and calls (nattest memuse)

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
# libibkey (spec R6.7 part 3b, src/lib/ibkey): Cyfral and Metakom iButton
# keys (Unleashed's line code), linked into ikey.
IBKEY_DIR := $(MROOT)/src/lib/ibkey
$(MB)/ibkey/ibkey.o: $(IBKEY_DIR)/ibkey.c $(IBKEY_DIR)/ibkey.h
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@
$(MB)/cmd/port/ikey.o: cmds/ikey.c $(IBKEY_DIR)/ibkey.h $(MROOT)/include/minix/ibutton.h
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -I$(IBKEY_DIR) -MMD -MP -c $< -o $@
HID_DIR	:= $(MROOT)/src/lib/hid
$(MB)/cmd/m2test.o: m2/m2test.c $(HID_DIR)/libhid.h $(IBKEY_DIR)/ibkey.h tests/flipperpb/golden.h
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -I$(HID_DIR) -I$(IBKEY_DIR) -Itests/flipperpb -MMD -MP -c $< -o $@
# libhid (spec R11.3.2, src/lib/hid): text and pointer actions as USB HID
# reports, the stock .kl layouts (US built in), static; m2test types with
# it (R11.3.1), badusb and Teklatua (T28c, T28d) link it too.
$(MB)/hid/hid.o: $(HID_DIR)/hid.c $(HID_DIR)/libhid.h
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@
OBJS_m2test = $(MB)/cmd/m2test.o $(MB)/hid/hid.o $(MB)/ibkey/ibkey.o
# spinfifo (#331): processes spinning on EAGAIN, for m2test's L1 check.
OBJS_spinfifo = $(MB)/cmd/spinfifo.o
$(MB)/cmd/spinfifo.o: m2/spinfifo.c
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@
# guiflood (#324): ACK.3's four desktop modules, so they are not forks of
# m2test (each needed a copy of m2test's data).
OBJS_guiflood = $(MB)/cmd/guiflood.o
$(MB)/cmd/guiflood.o: m2/guiflood.c
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@
# fptest (R5.7): the FPU checks; m2fpu.c and fpregs.S use the FPU.
OBJS_fptest = $(MB)/cmd/fptest.o $(MB)/cmd/m2fpu.o $(MB)/cmd/fpregs.o
$(MB)/cmd/fptest.o: m2/fptest.c
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@
$(MB)/cmd/m2fpu.o: m2/m2fpu.c
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) $(UFPFLAGS) -MMD -MP -c $< -o $@
$(MB)/cmd/fpregs.o: m2/fpregs.S
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -c $< -o $@
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
OBJS_ikey += $(MB)/ibkey/ibkey.o	# libibkey (R6.7 part 3b), above
$(foreach c,$(ZMODEM),$(eval OBJS_$(c) = $(MB)/cmd/zmodem/$(c).o))
$(foreach c,$(GUIAPPS),$(eval OBJS_$(c) = $(MB)/cmd/gui/$(c).o $(MB)/cmd/gui/gui.o))
OBJS_launcher += $(MB)/cmd/gui/appline.o $(MB)/cmd/gui/winsrv.o $(MB)/cmd/gui/winrec.o \
		 $(MB)/cmd/gui/desktop.o $(MB)/cmd/gui/launcher_stock.o
# the Spectrum app's steps, scale and zoom (issue #157; tests/host spectest)
OBJS_spectrum += $(MB)/cmd/gui/specmath.o
# the Infrared app (issue #389): libir (src/lib/infrared) and the list UI
OBJS_infrared += $(MB)/cmd/gui/ui.o $(LIBIR_OBJ)
# the desktop's module records (spec K.3, K.6): guimod builds them
OBJS_guimod = $(MB)/cmd/port/guimod.o $(MB)/cmd/gui/desktop.o
# Programs linked statically against libc-pic, never the shared libc:
# its per-process area (608 B of GOT and data) alone would break their
# RAM budget.  Only for tiny resident programs that call little of libc.
STATICPROGS := jiggle
define static_prog
$(MB)/user/$(1).elf: $(call objs_of,$(1)) $(MB)/user/crtso.o $(MB)/libc-pic.a \
		ld/user-pic.ld
	@mkdir -p $$(dir $$@)
	@$(CC) $(MLDFLAGS) -Wl,-q -Tld/user-pic.ld -Wl,-Map=$$@.map \
	   $(MB)/user/crtso.o $(call objs_of,$(1)) $(MB)/libc-pic.a -lgcc -o $$@
endef
# jiggle (spec R11.3.6, T28e): a <= 1 KB process.  Static (above): data
# 48 B + bss 140 B (libc's atexit table, errno); one .data/.rodata per
# object (no -fdata-sections, -fno-merge-constants): its strings share
# GOT words.  No malloc, no stdio, no signal handler: no MM gap.
$(MB)/cmd/port/jiggle.o: cmds/jiggle.c
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -fno-data-sections -fno-merge-constants -MMD -MP \
	   -c $< -o $@
# mtpd (spec R10.4, T29a): the MTP container parser and operations
# (mtpcore.c), the files (mtpfs.c: handles = inode numbers, the parent
# ring), the stream transport for tests (mtpfd.c); the same sources build
# on the PC (make -C mtpd test, fuzz).  No stdio, no malloc but
# opendir's DIR, no signal handler.  One .data/.bss/.rodata per object
# (as the window apps, WIN_CFLAGS): its statics share GOT words.
MTPD_SRC := mtpd mtpcore mtpfs mtpfd
$(MB)/cmd/mtpd/%.o: mtpd/%.c mtpd/mtp.h
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -fno-data-sections -fno-merge-constants -MMD -MP \
	   -c $< -o $@
OBJS_mtpd = $(MTPD_SRC:%=$(MB)/cmd/mtpd/%.o)
# rpcd (spec R15.5, R15.7; issue #388): the core (src/lib/rpcd), the stock
# protobuf (src/lib/flipperpb) and nanopb, built as C11 (nanopb needs
# <stdint.h>/<stdbool.h>: GCC's freestanding ones, through -isystem
# $(GCCINC)); the MINIX platform part is cmds/rpcd.c.  The same core runs
# on the PC (make -C tests/rpcd test).
NPB_DIR	:= $(MROOT)/src/lib/nanopb
FPB_DIR	:= $(MROOT)/src/lib/flipperpb
RPC_DIR	:= $(MROOT)/src/lib/rpcd
RPC_CFLAGS := $(UCFLAGS) -std=gnu11 -DPB_ENABLE_MALLOC -I$(NPB_DIR) \
	   -I$(FPB_DIR) -I$(FPB_DIR)/gen -I$(RPC_DIR)
FPB_GEN	:= $(notdir $(basename $(wildcard $(FPB_DIR)/gen/*.pb.c)))
$(MB)/rpcd/npb/%.o: $(NPB_DIR)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(RPC_CFLAGS) -MMD -MP -c $< -o $@
$(MB)/rpcd/fpb/%.o: $(FPB_DIR)/gen/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(RPC_CFLAGS) -MMD -MP -c $< -o $@
$(MB)/rpcd/core.o: $(RPC_DIR)/rpcd.c $(RPC_DIR)/rpcd.h
	@mkdir -p $(dir $@)
	@$(CC) $(RPC_CFLAGS) -MMD -MP -c $< -o $@
$(MB)/rpcd/main.o: cmds/rpcd.c $(RPC_DIR)/rpcd.h
	@mkdir -p $(dir $@)
	@$(CC) $(RPC_CFLAGS) -MMD -MP -c $< -o $@
OBJS_rpcd = $(MB)/rpcd/main.o $(MB)/rpcd/core.o \
	   $(patsubst %,$(MB)/rpcd/npb/%.o,pb_common pb_decode pb_encode) \
	   $(FPB_GEN:%=$(MB)/rpcd/fpb/%.o)
# libgm (spec K.8, src/lib/gm): window apps describe their screens, the
# launcher draws them (gui/winsrv.c).  Static, one object: no malloc, no
# stdio; programs link it with --gc-sections.
GM_DIR	:= $(MROOT)/src/lib/gm
$(MB)/gm/gm.o: $(GM_DIR)/gm.c $(GM_DIR)/gm.h
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@
$(MB)/cmd/gui/archive.o: gui/archive.c $(GM_DIR)/gm.h
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -I$(GM_DIR) -MMD -MP -c $< -o $@
OBJS_archive = $(MB)/cmd/gui/archive.o $(MB)/gm/gm.o
# nfc (spec Phase 14, issue #412 item 4): a window app (K.8) on the
# hand-written libnfc reader (src/lib/libnfc: poller, Crypto-1, the
# streaming dictionary, the .nfc files), C11 as that library is.
NFCL_DIR := $(MROOT)/src/lib/libnfc
NFCL_SRC := crypto1 iso14443_3a mf_classic mf_ultralight mf_dict mf_attack \
	    nfc_file nfc_poller
NFCL_HDR := $(wildcard $(NFCL_DIR)/*.h)
NFCL_CFLAGS = $(filter-out -std=gnu89,$(UCFLAGS)) -std=gnu11 -I$(NFCL_DIR) \
	    -I$(GM_DIR) -Igui
$(MB)/libnfc/%.o: $(NFCL_DIR)/%.c $(NFCL_HDR)
	@mkdir -p $(dir $@)
	@$(CC) $(NFCL_CFLAGS) -MMD -MP -c $< -o $@
$(MB)/cmd/gui/nfc.o: gui/nfc.c gui/nfcloop.h $(GM_DIR)/gm.h $(NFCL_HDR) \
		$(MROOT)/include/minix/nfc.h
	@mkdir -p $(dir $@)
	@$(CC) $(NFCL_CFLAGS) -MMD -MP -c $< -o $@
$(MB)/cmd/gui/nfcloop.o: gui/nfcloop.c gui/nfcloop.h $(NFCL_HDR)
	@mkdir -p $(dir $@)
	@$(CC) $(NFCL_CFLAGS) -MMD -MP -c $< -o $@
OBJS_nfc = $(MB)/cmd/gui/nfc.o $(MB)/cmd/gui/nfcloop.o $(MB)/gm/gm.o \
	   $(NFCL_SRC:%=$(MB)/libnfc/%.o)
$(MB)/cmd/app/gmdemo.o: apps/gmdemo.c $(GM_DIR)/gm.h
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -I$(GM_DIR) -MMD -MP -c $< -o $@
# The system settings (spec K.7): the groups' files are
# src/commands/settings/.  settings (flipper/cmds) keeps diff/reset (L.4)
# and applies the files at boot; setgroup GROUP (win.c) is the window app
# of the launcher's SETTINGS page (K.8).  Their objects as the window
# apps' (WIN_CFLAGS below): one GOT word per section, unused rodata in
# flash.
SETGROUPS := groups conf display sound time about zones afmt
SET_OBJ	:= $(SETGROUPS:%=$(MB)/cmd/settings/%.o) $(MB)/cmd/settings/gm.o
OBJS_settings = $(MB)/cmd/port/settings.o $(SET_OBJ)
OBJS_setgroup = $(MB)/cmd/settings/win.o $(SET_OBJ)
SET_CFLAGS = $(UCFLAGS) -fno-data-sections -fno-merge-constants -I$(GM_DIR)
$(MB)/cmd/settings/%.o: $(MROOT)/src/commands/settings/%.c \
		$(MROOT)/src/commands/settings/sgroup.h $(GM_DIR)/gm.h
	@mkdir -p $(dir $@)
	@$(CC) $(SET_CFLAGS) -MMD -MP -c $< -o $@
$(MB)/cmd/settings/gm.o: $(GM_DIR)/gm.c $(GM_DIR)/gm.h
	@mkdir -p $(dir $@)
	@$(CC) $(SET_CFLAGS) -MMD -MP -c $< -o $@
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
# libir (spec Phase 7, issue #389, src/lib/infrared): the Unleashed infrared
# encoders and decoders (byte for byte) and irfile.c, linked into infrared.
# Third-party C11 code: built quietly (-w); the shim comes in by -include.
LIBIR_DIR := $(MROOT)/src/lib/infrared
LIBIR_C := $(shell cd $(LIBIR_DIR) && find . -name '*.c' | sort)
LIBIR_OBJ := $(patsubst ./%.c,$(MB)/libir/%.o,$(LIBIR_C))
LIBIR_HDR := $(shell find $(LIBIR_DIR) -name '*.h')
LIBIR_CFLAGS = $(filter-out -std=gnu89,$(UCFLAGS)) -std=gnu11 \
	    -I$(LIBIR_DIR) -I$(LIBIR_DIR)/shim -I$(LIBIR_DIR)/encoder_decoder \
	    -include irshim.h
$(MB)/libir/%.o: $(LIBIR_DIR)/%.c $(LIBIR_HDR)
	@mkdir -p $(dir $@)
	@$(CC) $(LIBIR_CFLAGS) -w -c $< -o $@
$(MB)/cmd/gui/infrared.o: gui/infrared.c gui/gui.h gui/ui.h $(LIBIR_HDR)
	@mkdir -p $(dir $@)
	@$(CC) $(LIBIR_CFLAGS) -MMD -MP -c $< -o $@
# libsubghz (Phase 9, issue #302, src/lib/subghz): the Unleashed Sub-GHz
# protocols, linked into subghz (code in flash; the decoders' state is
# malloc'd while it runs).  Third-party C11 code: built quietly (-w).
SUBGHZ_DIR := $(MROOT)/src/lib/subghz
SUBGHZ_C := $(shell cd $(SUBGHZ_DIR) && find . -name '*.c' | sort)
SUBGHZ_OBJ := $(patsubst ./%.c,$(MB)/subghz/%.o,$(SUBGHZ_C))
SUBGHZ_HDR := $(shell find $(SUBGHZ_DIR) -name '*.h')
SUBGHZ_CFLAGS = $(filter-out -std=gnu89,$(UCFLAGS)) -std=gnu11 \
	    -I$(SUBGHZ_DIR) -I$(SUBGHZ_DIR)/shim -I$(SUBGHZ_DIR)/subghz
$(MB)/subghz/%.o: $(SUBGHZ_DIR)/%.c $(SUBGHZ_HDR)
	@mkdir -p $(dir $@)
	@$(CC) $(SUBGHZ_CFLAGS) -w -c $< -o $@
$(MB)/cmd/port/subghz.o: cmds/subghz.c $(SUBGHZ_HDR) $(MROOT)/include/minix/subghz.h
	@mkdir -p $(dir $@)
	@$(CC) $(SUBGHZ_CFLAGS) -MMD -MP -c $< -o $@
OBJS_subghz = $(MB)/cmd/port/subghz.o $(SUBGHZ_OBJ)
# Its own package (subghz_1.0.tar, Flash: yes): 130 KB of text runs in
# place from the installed region, never from RAM or the run-time cache.
SGPROGS	:= subghz
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
OBJS_gmodem = $(MB)/cmd/port/gmodem.o \
	   $(filter-out $(MB)/modem/rgblob.o $(MB)/modem/rgsha.o,$(MODEM_OBJ))
# gzupdate (spec section S, T33a part 3) links gzsig (src/lib/gzsig, the
# verifier atea shares, part 2).  gzkeys.h: the public keys atea's slots
# will hold (RS.6): keys/release.pub, the builder's update.pub, and for
# the QEMU tests UPDATE_TESTKEY (a file with 64 hex digits).
GZSIG_DIR := $(MROOT)/src/lib/gzsig
GZSIG_OBJ := $(addprefix $(MB)/gzsig/,sha512.o ed25519.o gzu.o)
$(MB)/gzsig/%.o: $(GZSIG_DIR)/%.c $(GZSIG_DIR)/gzsig.h
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@
GZKEYS	:= $(MB)/gen/gzkeys.h
# QEMU: the throw-away key of tools/updatetest.py, whose good and bad files
# go to /lib/gzu for m2test's ACS.2 check (tools/gzucases.py).
ifeq ($(BOARD),qemu)
UPDATE_TESTKEY ?= $(MB)/updatetest/trusted.pub
GZU_RUN	:= $(MB)/gzu/run
$(MB)/updatetest/trusted.pub: tools/updatetest.py tools/mkupdate.py \
		tools/gzsign tools/gzupdate-check.py
	python3 -I tools/updatetest.py -o $(MB)/updatetest --board qemu-mps2
$(GZU_RUN): $(MB)/updatetest/trusted.pub tools/gzucases.py
	python3 -I tools/gzucases.py $(MB)/updatetest $(MB)/gzu
endif
GZKEY_FILES = $(strip $(UPDATE_KEYS) $(UPDATE_TESTKEY))
.PHONY: gzkeys-check
$(GZKEYS): gzkeys-check $(UPDATE_TESTKEY)
	@mkdir -p $(dir $@)
	@python3 -I tools/gzkeys.py $@ $(GZKEY_FILES)
$(MB)/cmd/port/gzupdate.o: cmds/gzupdate.c $(GZSIG_DIR)/gzsig.h $(GZKEYS) \
		$(MROOT)/include/minix/gzversion.h
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -I$(GZSIG_DIR) -MMD -MP -c $< -o $@
OBJS_gzupdate = $(MB)/cmd/port/gzupdate.o $(GZSIG_OBJ)
# gztree (RS.14): the base-file switch INIT runs; SHA-512 only.
$(MB)/cmd/port/gztree.o: cmds/gztree.c $(GZSIG_DIR)/gzsig.h \
		$(MROOT)/include/minix/gzversion.h
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -I$(GZSIG_DIR) -MMD -MP -c $< -o $@
OBJS_gztree = $(MB)/cmd/port/gztree.o $(MB)/gzsig/sha512.o
# region (spec section Q, issue #292): the radio region tables on the card.
$(MB)/cmd/port/region.o: cmds/region.c $(MODEM_DIR)/modem.h $(MROOT)/include/minix/subghz.h
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -I$(MODEM_DIR) -MMD -MP -c $< -o $@
REGION_FILES := $(sort $(wildcard rootfs/usr/share/region/*.reg))
# RQ.5: the SHA-256 of every shipped table, a const list linked into region
# (34 B a table, flash); a changed table needs `region set -f`.
$(MB)/cmd/port/reghash.c: tools/mkreghash.py $(REGION_FILES)
	@mkdir -p $(dir $@)
	python3 -I tools/mkreghash.py $@ $(REGION_FILES)
$(MB)/cmd/port/reghash.o: $(MB)/cmd/port/reghash.c $(MODEM_DIR)/modem.h
	@$(CC) $(UCFLAGS) -I$(MODEM_DIR) -c $< -o $@
OBJS_region = $(MB)/cmd/port/region.o $(MB)/modem/region.o $(MB)/modem/rgblob.o \
	   $(MB)/modem/rgsha.o $(MB)/cmd/port/reghash.o $(MB)/modem/cc1101.o \
	   $(MB)/modem/duty.o
# radio (spec R16.0.2, R2b.1a; T30a): CPU2 stack images on the card;
# libradio (src/lib/radio) and libmodem's SHA-256.
RADIO_DIR := $(MROOT)/src/lib/radio
$(MB)/cmd/port/radio.o: cmds/radio.c $(RADIO_DIR)/radio.h $(MODEM_DIR)/modem.h
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -I$(RADIO_DIR) -I$(MODEM_DIR) -MMD -MP -c $< -o $@
$(MB)/radio/radio.o: $(RADIO_DIR)/radio.c $(RADIO_DIR)/radio.h
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -I$(RADIO_DIR) -MMD -MP -c $< -o $@
OBJS_radio = $(MB)/cmd/port/radio.o $(MB)/radio/radio.o $(MB)/modem/rgsha.o
$(MB)/cmd/gui/%.o: gui/%.c gui/gui.h gui/appline.h gui/launcher_stock.h gui/ui.h
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
FURIPROGS := furihello timed alert notepost
FURI_LINK := $(if $(filter 1,$(SHLIBC)),,$(SHFURI_LIBOBJ))
TIMEDPROGS := timed alert
$(MB)/cmd/timed/%.o: apps/timed/%.c $(APP_DIR)/include/app.h
	@mkdir -p $(dir $@)
	@$(CC) $(APP_CFLAGS) -Igui -MMD -MP -c $< -o $@
OBJS_timed = $(MB)/cmd/timed/timed.o $(FURI_LINK)
# notepost (tests package, T23c): notify_post() from the shell, for nattest
$(MB)/cmd/test/notepost.o: tests/notepost.c $(APP_DIR)/include/app.h
	@mkdir -p $(dir $@)
	@$(CC) $(APP_CFLAGS) -MMD -MP -c $< -o $@
OBJS_alert = $(MB)/cmd/timed/alert.o $(MB)/cmd/gui/gui.o $(FURI_LINK)
CHMEM_timed := 5120	# R7b.4: about 3 KB while it runs; 7b: struct app_timer
			# grew (Tone:, Log:), 4 KB left opendir() no heap under INIT
CHMEM_alert := 6144	# gui, poll, the 1 KB picture under it (R7b.2); timedtest memuse
# The native apps (spec 7b.1, 7b.2) are window apps now (WINAPPS below);
# the nat rules stay for native apps that still use libfuri.
NATPROGS :=
NAT_CFLAGS = $(FURI_GUI_CFLAGS) -I$(FURI_DIR) -I$(APP_DIR)/include -Iapps/native
$(MB)/cmd/nat/%.o: apps/%.c $(FURI_HDR) $(APP_DIR)/include/app.h apps/native/nat.h
	@mkdir -p $(dir $@)
	@$(CC) $(NAT_CFLAGS) -MMD -MP -c $< -o $@
$(foreach p,$(NATPROGS),$(eval OBJS_$(p) = $(MB)/cmd/nat/$(p)/$(p).o \
	$(MB)/cmd/nat/native/nat_core.o $(FURI_LINK)))
# chmem: libfuri's main stack 3 KB and gui thread 2 KB (#188), the heap
# (Canvas 1.1 KB, views, the lists) and MM's 1.5 KB safety gap above it.
# Window apps with timers (spec K.8, T25a): remind (Kanpaia) describes its
# windows with libgm and keeps its timers with libapp, both linked
# statically (--gc-sections), never the shared libfuri: no Canvas, no gui
# thread, no furi data area.  Package remind (/usr/bin, own user).
# Their objects without -fdata-sections and with -fno-merge-constants:
# under -mno-pic-data-is-text-relative every static variable and string
# literal of its own section costs a GOT word in RAM; in one .data/.bss/
# .rodata per object they share one (section anchors): remind's GOT 868 ->
# ~300 B.  Unused rodata then stays in flash, not RAM.
WINAPPS	:= remind alarm usbremote
WIN_CFLAGS = $(APP_CFLAGS) -fno-data-sections -fno-merge-constants \
	-I$(GM_DIR) -Iapps/native -I$(HID_DIR)
$(MB)/cmd/win/%.o: apps/%.c $(GM_DIR)/gm.h $(APP_DIR)/include/app.h \
		apps/native/nat.h
	@mkdir -p $(dir $@)
	@$(CC) $(WIN_CFLAGS) -MMD -MP -c $< -o $@
$(MB)/cmd/win/lib/%.o: $(APP_DIR)/src/%.c $(APP_DIR)/include/app.h
	@mkdir -p $(dir $@)
	@$(CC) $(WIN_CFLAGS) -MMD -MP -c $< -o $@
$(MB)/cmd/win/lib/gm.o: $(GM_DIR)/gm.c $(GM_DIR)/gm.h
	@mkdir -p $(dir $@)
	@$(CC) $(WIN_CFLAGS) -MMD -MP -c $< -o $@
$(MB)/cmd/win/lib/hid.o: $(HID_DIR)/hid.c $(HID_DIR)/libhid.h
	@mkdir -p $(dir $@)
	@$(CC) $(WIN_CFLAGS) -MMD -MP -c $< -o $@
$(MB)/cmd/win/lib/nat_core.o: apps/native/nat_core.c apps/native/nat.h
	@mkdir -p $(dir $@)
	@$(CC) $(WIN_CFLAGS) -MMD -MP -c $< -o $@
WIN_LIB	:= $(addprefix $(MB)/cmd/win/lib/,gm.o nat_core.o tspec.o io.o tune.o)
OBJS_remind = $(MB)/cmd/win/remind/remind.o $(WIN_LIB)
CHMEM_remind := 2304	# K.8 (ACK8.3: 4.0 KB open with ~1.8 KB data+bss):
			# all stack, ~1.7 KB used (nattest memuse).  No heap:
			# MM's 1.5 KB gap is checked only by brk() and a caught
			# signal, and remind never calls brk and catches none.
# Ordua (alarm, T25b): the same libraries; no malloc (alarms are read by
# name, no opendir()), no stdio, no signal handler (libgm ignores SIGPIPE),
# so no MM gap either: the stack, where timer_add() keeps 2 x 512 B while
# it saves (nattest memuse).
OBJS_alarm = $(MB)/cmd/win/alarm/alarm.o $(WIN_LIB)
CHMEM_alarm  := 2560	# K.8 (ACK8.3: 4352 B open, <= 4.5 KB): 1008 B
			# never used (nattest memuse, after a Save)
# Teklatua (usbremote, T28d, spec R11.3.5): libgm and libhid, no libapp;
# no malloc, no stdio, no signal handler, so no MM gap (K.8, #251): the
# stack holds the 256 B layout and the record and event buffers.
OBJS_usbremote = $(MB)/cmd/win/usbremote/usbremote.o \
		 $(addprefix $(MB)/cmd/win/lib/,gm.o hid.o)
$(MB)/cmd/win/usbremote/usbremote.o: $(HID_DIR)/libhid.h
CHMEM_usbremote := 2048	# K.8 / R11.3.5: 2816 B open (nattest, <= 4.5 KB);
			# 968 B never used (memuse, after typing, Media, pad)

# Bad USB (spec R11.3.3, R11.3.4, T28c): the badusb command (the script
# engine ducky.c, libhid) and the Bad USB window app (libgm), both built
# like the window apps (WIN_CFLAGS: one GOT word per object's sections),
# with their own copy of libhid in those flags.  Package badusb.
BU_DIR	:= apps/badusb
BUPROGS	:= badusb
WINAPPS	+= badusb-gui
$(MB)/cmd/bu/%.o: $(BU_DIR)/%.c $(BU_DIR)/ducky.h $(HID_DIR)/libhid.h \
		$(GM_DIR)/gm.h
	@mkdir -p $(dir $@)
	@$(CC) $(WIN_CFLAGS) -I$(HID_DIR) -MMD -MP -c $< -o $@
$(MB)/cmd/bu/hid.o: $(HID_DIR)/hid.c $(HID_DIR)/libhid.h
	@mkdir -p $(dir $@)
	@$(CC) $(WIN_CFLAGS) -MMD -MP -c $< -o $@
OBJS_badusb = $(MB)/cmd/bu/badusb.o $(MB)/cmd/bu/ducky.o $(MB)/cmd/bu/hid.o
CHMEM_badusb := 2048	# R11.3.3 (<= 3 KB running): the layout (256 B)
			# and the line (257 B) on the stack, no heap, no
			# handler (the stop signals are blocked and looked
			# at), so no MM gap (nattest memuse)
OBJS_badusb-gui = $(MB)/cmd/bu/badusbgui.o $(MB)/cmd/win/lib/gm.o \
	$(MB)/cmd/bu/hid.o
CHMEM_badusb-gui := 2304	# K.8 (<= 4.5 KB open): no heap, no
			# handler, so no MM gap (nattest memuse)
$(MB)/cmd/furihello.o: $(FURI_DIR)/hello.c $(MB)/libfuri.a
	@mkdir -p $(dir $@)
	@$(CC) $(FURI_GUI_CFLAGS) -I$(FURI_DIR) -MMD -MP -c $< -o $@
OBJS_furihello = $(MB)/cmd/furihello.o $(FURI_LINK)
# Security tests (flipper/tests, spec rule 0.4), package tests: /usr/tests.
TESTS	:= sigret unaligned badreloc fcpoison mpufrag abiexec fccheck \
	   pkgbad pkgplant sticky pwdcheck swapper swapsecret swapsmall \
	   swaptime swaptime64 swapwear regtest hidfake sgfake alarmrace \
	   notepost prwtest
$(foreach c,$(TESTS),$(eval OBJS_$(c) = $(MB)/cmd/test/$(c).o))
OBJS_notepost = $(MB)/cmd/test/notepost.o $(FURI_LINK)	# T23c: libapp
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
$(foreach p,$(filter-out $(FURIPROGS) $(STATICPROGS),$(sort $(UPROGS) $(APPS)) $(LIBPROGS) $(TESTS) $(TIMEDPROGS) $(STPROGS) $(NATPROGS) $(WINAPPS) $(BUPROGS) $(SGPROGS)),$(eval $(call user_prog,$(p))))
$(foreach p,$(FURIPROGS),$(eval $(call furi_prog,$(p))))
$(foreach p,$(STATICPROGS),$(eval $(call static_prog,$(p))))
$(MB)/cmd/app/%.o: apps/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@

# Packages (tools/mkpkg.py), shipped in /lib/pkg on the root file system.
PKGDIR	:= $(MB)/pkgs
PKGS	:= $(PKGDIR)/hello_1.0.tar $(PKGDIR)/tests_1.0.tar $(PKGDIR)/timed_1.2.tar \
	   $(PKGDIR)/stest_1.0.tar $(PKGDIR)/gmdemo_1.1.tar \
	   $(PKGDIR)/remind_1.1.tar $(PKGDIR)/alarm_1.1.tar \
	   $(PKGDIR)/badusb_1.0.tar \
	   $(PKGDIR)/usbrem_1.0.tar $(PKGDIR)/subghz_1.0.tar
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
# remind 1.1 (T25a): a window app (Type: window), ended on Back.
$(PKGDIR)/remind_1.1.tar: $(MB)/bin/remind tools/mkpkg.py rootfs/remind.postinst \
		rootfs/remind.prerm
	python3 -I tools/mkpkg.py -n remind -v 1.1 -d timed -o $(PKGDIR) \
	   --timers --notify reminder --flash --window --background exit \
	   -i @bell --postinst rootfs/remind.postinst --prerm rootfs/remind.prerm \
	   -D "Kanpaia: reminders (once, daily, every N minutes)" /usr/bin/remind=$<
# Ordua: a window app (spec K.8, T25b), ended on Back (Background: exit).
$(PKGDIR)/alarm_1.1.tar: $(MB)/bin/alarm tools/mkpkg.py rootfs/alarm.postinst \
		rootfs/alarm.prerm
	python3 -I tools/mkpkg.py -n alarm -v 1.1 -d timed -o $(PKGDIR) \
	   --timers --notify alarm --flash --window --background exit \
	   -i @alarm --postinst rootfs/alarm.postinst --prerm rootfs/alarm.prerm \
	   -D "Ordua: alarm clock with a desk clock (window app)" /usr/bin/alarm=$<
# Teklatua (spec R11.3.5, T28d), package usbrem (a tar name is at most 14
# characters): a window app as the user usbremote
# (group usb), ended on Back; jiggle (R11.3.6) is its own command.
$(PKGDIR)/usbrem_1.0.tar: $(MB)/bin/usbremote tools/mkpkg.py \
		rootfs/usbremote.postinst rootfs/usbremote.prerm
	python3 -I tools/mkpkg.py -n usbrem -v 1.0 -o $(PKGDIR) \
	   --flash --window --background exit -i @usb \
	   --postinst rootfs/usbremote.postinst --prerm rootfs/usbremote.prerm \
	   -D "Teklatua: USB keyboard, numpad, media keys, mouse (window app)" \
	   /usr/bin/usbremote=$<

# Bad USB (spec R11.3.3, R11.3.4): badusb, the window app, the example
# scripts in /ext/badusb (/home/flipper/badusb: the package's files, an
# upgrade puts them back) and the German layout (libhid has US built in).
BU_EX	:= $(wildcard $(BU_DIR)/examples/*.txt)
$(PKGDIR)/badusb_1.0.tar: $(MB)/bin/badusb $(MB)/bin/badusb-gui tools/mkpkg.py \
		rootfs/badusb.postinst rootfs/badusb.prerm $(BU_EX) \
		$(HID_DIR)/layouts/de-DE.kl
	python3 -I tools/mkpkg.py -n badusb -v 1.0 -o $(PKGDIR) --flash \
	   --window --background exit -i @usb \
	   --postinst rootfs/badusb.postinst --prerm rootfs/badusb.prerm \
	   -D "Bad USB: DuckyScript as a USB keyboard (badusb, window app)" \
	   /usr/bin/badusb=$(MB)/bin/badusb \
	   /usr/bin/badusb-gui=$(MB)/bin/badusb-gui \
	   /usr/share/hid/layouts/de-DE.kl=$(HID_DIR)/layouts/de-DE.kl$(COMMA)644 \
	   $(foreach e,$(BU_EX),/home/flipper/badusb/$(notdir $(e))=$(e)$(COMMA)644)
# Sub-GHz (spec Phase 9, issue #302): the subghz command with every
# Unleashed protocol (libsubghz), in place in flash; the user's keystore
# goes to /home/flipper/subghz/assets (D.3: none shipped).
$(PKGDIR)/subghz_1.0.tar: $(MB)/bin/subghz tools/mkpkg.py
	python3 -I tools/mkpkg.py -n subghz -v 1.0 -o $(PKGDIR) --flash \
	   -D "Sub-GHz remotes: rx, decode, tx, tx_from_file, chat (Unleashed protocols)" \
	   /usr/bin/subghz=$(MB)/bin/subghz
# The example window app (spec K.8): the program, its launcher line, and
# its source with libgm in /usr/share/examples/gui; with them the example
# desktop module of spec K.6 (battile.sh, a battery tile in three lines).
$(PKGDIR)/gmdemo_1.1.tar: $(MB)/bin/gmdemo tools/mkpkg.py rootfs/gmdemo.postinst \
		rootfs/gmdemo.prerm apps/gmdemo.c $(GM_DIR)/gm.c $(GM_DIR)/gm.h \
		apps/battile.sh
	python3 -I tools/mkpkg.py -n gmdemo -v 1.1 -o $(PKGDIR) --window --flash \
	   -i @app --postinst rootfs/gmdemo.postinst --prerm rootfs/gmdemo.prerm \
	   -D "Example window app (spec K.8): the launcher draws it" \
	   /usr/bin/gmdemo=$< \
	   /usr/share/examples/gui/gmdemo.c=apps/gmdemo.c \
	   /usr/share/examples/gui/gm.c=$(GM_DIR)/gm.c \
	   /usr/share/examples/gui/gm.h=$(GM_DIR)/gm.h \
	   /usr/share/examples/gui/battile.sh=apps/battile.sh$(COMMA)644
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
BASE_VER := 4.67
BASEPKG	:= $(PKGDIR)/base_$(BASE_VER).tar
$(BASEPKG): $(UPROGS:%=$(MB)/bin/%) $(LIBPROGS:%=$(MB)/bin/%) tools/mkpkg.py \
	    rootfs/base.postinst $(REGION_FILES)
	python3 -I tools/mkpkg.py -n base -v $(BASE_VER) -o $(PKGDIR) \
	   --postinst rootfs/base.postinst \
	   --os $(GZ_RELEASE) \
	   -D "Galtzagorriak MINIX system programs" \
	   $(foreach p,$(UPROGS),/bin/$(p)=$(MB)/bin/$(p)$(if $(filter $(p),$(SUIDPROGS)),$(COMMA)4755)$(if $(call sgid_of,$(p)),$(COMMA)2755:$(call sgid_of,$(p)))) \
	   $(foreach p,$(LIBPROGS),/lib/$(p)=$(MB)/bin/$(p)$(COMMA)4755) \
	   $(foreach r,$(REGION_FILES),/usr/share/region/$(notdir $(r))=$(r)$(COMMA)644) \
	   $(foreach a,$(ALIASES),/bin/$(word 1,$(subst =, ,$(a)))=$(MB)/bin/$(word 2,$(subst =, ,$(a))))
.PHONY: base
base: $(BASEPKG)

# make update: the signed update file (spec section S, RS.17).  mkupdate.py
# builds it from minix-base.bin, the base package, the app packages and the
# release notes; tools/gzsign signs it (key: GZ_UPDATE_KEY or
# ~/.config/galtzagorriak/update.key; without one it stops and says how to
# make one); gzupdate-check.py checks it with the keys atea will trust
# (keys/release.pub and ~/.config/galtzagorriak/update.pub, RS.6).
# Until atea (T33b) the flash section starts at the image's own address.
UPDATE		:= $(MB)/Galtzagorriak.update
UPDATE_BOARD	:= $(if $(filter flipper,$(BOARD)),flipper-f7,qemu-mps2)
UPDATE_LOAD	:= $(if $(filter flipper,$(BOARD)),0x08000000,0)
UPDATE_FROM_MIN	?= 0.1.2
UPDATE_PKGS	:= $(filter-out %/tests_1.0.tar %/hello_1.0.tar,$(PKGS))
UPDATE_KEYS	= $(wildcard keys/release.pub) \
		  $(wildcard $(HOME)/.config/galtzagorriak/update.pub)
.PHONY: update
update: $(MB)/minix-base.bin $(BASEPKG) $(UPDATE_PKGS) $(GZBUILD) \
	tools/mkupdate.py tools/gzsign tools/gzupdate-check.py
	python3 -I tools/mkupdate.py -o $(UPDATE).tmp \
	   --version-h $(MROOT)/include/minix/gzversion.h \
	   --changelog $(MROOT)/CHANGELOG.md --gzbuild $(GZBUILD) \
	   --libc $(SHL)/libc.json --furi $(SHF)/furi.json \
	   --board $(UPDATE_BOARD) --flash $(MB)/minix-base.bin \
	   --load $(UPDATE_LOAD) --root $(BASEPKG) \
	   --from-min $(UPDATE_FROM_MIN) $(UPDATE_PKGS:%=--pkg %)
	python3 -I tools/gzsign $(UPDATE).tmp || { rm -f $(UPDATE).tmp; exit 1; }
	@keys="$(strip $(UPDATE_KEYS))"; \
	if [ -z "$$keys" ]; then \
	   keys=$$(python3 -I tools/gzsign --pub) || exit 1; \
	   echo "update: no keys/release.pub or ~/.config/galtzagorriak/update.pub:" \
	        "no atea slot would trust this file; checking with its own key"; \
	fi; \
	python3 -I tools/gzupdate-check.py --board $(UPDATE_BOARD) -v \
	   $$(for k in $$keys; do printf -- '--key %s ' $$k; done) $(UPDATE).tmp \
	   || { rm -f $(UPDATE).tmp; exit 1; }
	mv $(UPDATE).tmp $(UPDATE)
	cd $(MB) && sha512sum $(notdir $(UPDATE)) > $(notdir $(UPDATE)).sha512
	@echo "update: $(UPDATE) and .sha512"
# The ACS.1 bad files, checked on the host (needs no build).
.PHONY: updatetest
updatetest:
	python3 -I tools/updatetest.py -o $(MB)/updatetest --board $(UPDATE_BOARD)
# QEMU: a real update file for the apply test (tools/gzapplytest.py): this
# build's minix-base.bin and base package, this build's epoch (the test
# runs `gzupdate apply -e` with a lower one), signed with updatetest's
# throw-away key, in /lib/gzu/apply.upd.
ifeq ($(BOARD),qemu)
GZU_APPLY := $(MB)/gzu/apply.upd
$(GZU_APPLY): $(GZU_RUN) $(MB)/minix-base.bin $(BASEPKG) $(GZBUILD) \
		tools/mkupdate.py tools/gzsign
	python3 -I tools/mkupdate.py -o $@.tmp \
	   --version-h $(MROOT)/include/minix/gzversion.h \
	   --changelog $(MROOT)/CHANGELOG.md --gzbuild $(GZBUILD) \
	   --libc $(SHL)/libc.json --furi $(SHF)/furi.json \
	   --board qemu-mps2 --flash $(MB)/minix-base.bin --load 0 \
	   --root $(BASEPKG)
	python3 -I tools/gzsign --key $(MB)/updatetest/trusted.key $@.tmp
	mv $@.tmp $@
endif
.PHONY: pkgs
pkgs: $(PKGS)

# Convert ELF -> MINIX a.out with the chmem value above (default 6 KB).
$(MB)/bin/%: $(MB)/user/%.elf tools/elf2mnx.py minix.mk \
		$(if $(filter 1,$(SHLIBC)),$(SHL)/libc.json)
	@mkdir -p $(dir $@)
	@python3 -I tools/elf2mnx.py $< $@ $(if $(CHMEM_$*),--stack $(CHMEM_$*)) \
	   $(if $(filter $*,$(SECRETPROGS)),--secret) \
	   $(if $(filter $*,$(STATICPROGS)),,$(ELF2MNX_LIBC)) \
	   $(ELF2MNX_FURI)
$(if $(filter 1,$(SHLIBC)),$(FURIPROGS:%=$(MB)/bin/%)): $(SHF)/furi.json

# Root file system: rootfs/proto -> fs.img (dd to the SD card partition)
# and disk.img (MBR + partition 1, the QEMU "SD card").  @BINFILES@ in the
# proto becomes one entry per program.
$(MB)/proto: rootfs/proto minix.mk tools/mkproto.py $(PKGS) $(GZU_RUN) \
		$(GZU_APPLY)
	@mkdir -p $(MB)
	python3 -I tools/mkproto.py $< $@ $(MB)/bin "$(UPROGS)" "$(ALIASES)" \
	   "$(PKGS)" "$(SUIDPROGS) $(SGIDPROGS)" $(if $(GZU_RUN),$(dir $(GZU_RUN)))
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
	   $(MROOT)/src/commands/settings \
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

# The M2 self-test took 97 s under QEMU 10.0 (Debian 13) once #152 added
# the FPU checks: 90 s cut it off at its last test.  Slower hosts and a
# growing test list need room; a real hang still ends.
# INIT=m2: QEMU's second serial line (/dev/mtpline, spec R10.4) is a
# socket with tools/mtphost.py on it; m2test's mtpd test serves it, and
# its log follows QEMU's output.
MTPSOCK	:= $(MB)/mtpline.sock
M2TO	?= 240		# seconds QEMU gets for the M2 run
minix-run: $(MB)/minix.bin $(MB)/disk.img
	@test "$(BOARD)" = qemu || { echo "minix-run needs BOARD=qemu"; exit 1; }
ifeq ($(INIT),m2)
	@rm -f $(MTPSOCK) $(MB)/mtphost.log
	@python3 -I tools/mtphost.py test --unix $(MTPSOCK) --name-max 14 \
	   --quick --wait 30 > $(MB)/mtphost.log 2>&1 & \
	timeout $(M2TO) $(QEMU_MINIX) -device loader,file=$<,addr=0 \
	   -chardev socket,id=mtp,path=$(MTPSOCK),server=on,wait=off \
	   -serial chardev:mtp; \
	r=$$?; wait; cat $(MB)/mtphost.log; rm -f $(MTPSOCK); exit $$r
else
	timeout $(M2TO) $(QEMU_MINIX) -device loader,file=$<,addr=0
endif

minix-flash: $(MB)/minix.bin
	@test "$(BOARD)" = flipper || { echo "minix-flash needs BOARD=flipper"; exit 1; }
	dfu-util -a 0 -s 0x08000000:leave -D $<

-include $(shell find $(MB) -name '*.d' 2>/dev/null)

# Objects that print the build id.
$(MB)/kernel/tty.c.o $(MB)/cmd/port/ver.o $(MB)/mm/alloc.c.o \
		$(MB)/mm/finst.c.o: $(GZBUILD)
