# Galtzagorriak MINIX: modernization and security report

Repository: `arielkonopka/Galtzagorriak-MINIX`, `main` at `62ea5c1` (Position-independent user programs).
Date: 2026-11-14. Read-only review: nothing in the repository was changed.
Update (same day): Part 4 (swap design) and the option-byte decision added; Part 3 (Cortex-M4 / STM32WB55 features) added after reviewing
`5e3d0f0` (flash text cache), which also produced the new hotspot **S12**.

RAM figures marked *(measured)* come from a scratch build of `make BOARD=flipper minix`
(arm-none-eabi-gcc 13.2) and `arm-none-eabi-nm -S`. Other figures are estimates.

Ground rule used throughout: kernel, MM, FS and INIT already execute from flash (XIP), so
**code and `const` data there cost no RAM**; only `.data`, `.bss`, stacks and buffers do.
Parts 1 and 2 were written against `62ea5c1`, when user program text and rodata were still copied into
RAM per process. Since `5e3d0f0` (flash text cache), PIC user text runs from flash too.

Current RAM use of the boot images *(measured)*:

| Image  | text (flash) | .data (RAM + flash copy) | .bss (RAM) |
|--------|-------------:|-------------------------:|-----------:|
| kernel | 26108 | 652 | 16872 |
| MM     |  9068 | 372 | 10260 |
| FS     | 19032 | 476 | 27380 (of which 18.8 KB is the buffer cache in SRAM2) |
| INIT   |  5532 | 372 |  1376 |

---

## Part 1: Security hotspots

Ranked most severe first. "Any user" includes the launcher apps, which run as `nobody`.

### S1. Critical: `sigreturn` lets any process write kernel memory and make itself privileged

- `src/kernel/system.c:725` and `:748` copy `sc_sp` from a user-supplied `sigcontext`
  into `rp->p_reg.sp` without any check (only `control` and `psw` are protected, `:733`).
- `src/kernel/arm/mpx.S:165-179`: `restart` then builds the exception frame with a
  **privileged** `stmia r0, {r4-r11}` at `p_reg.sp - 32`, and only *afterwards* reads
  `P_CONTROL` (`:181`).
- So a process calls `sigreturn()` with a forged context whose `sc_sp` points 32 bytes past
  a target, and whose `r0..r3/r12/lr/pc` hold the values to write. The kernel writes them
  there. Pointing the frame at the caller's own `proc[].p_reg.control` with `r0 = 0`
  makes the process privileged on the very same return. An unaligned `sc_sp` instead
  faults in Handler mode and halts the system.

**Fix (0 B RAM, a few dozen bytes of flash):**
1. In `do_sigreturn`, reject the context unless `[sc_sp - 32, sc_sp)` lies inside the
   process's own D..S range and `sc_sp` is 4-byte aligned (e.g. `umap(rp, S, sc_sp - 32, 32) != 0`).
   Same for the `SC_NOREGLOCALS` branch.
2. Defense in depth in `mpx.S`: for unprivileged processes, write the frame in `restart`
   and read it in `SAVE` with `STRT`/`LDRT` (unprivileged loads/stores, ARMv7-M has them).
   Then the MPU, not the code, guarantees that a bad PSP can only fault, never write
   kernel memory. Eight `strt` instead of one `stmia`: about 30 bytes of flash.

### S2. Critical: exec relocation writes anywhere in memory (any user can become root)

- `src/mm/exec.c:590-603` (PIC path) and `:615-635` (GEMDOS path): the first offset and
  every delta come from the executable file, and `*w = ...` / `*((long *)adr) += off`
  write at the resulting address **with no bounds or alignment check**.
- MM runs privileged with the default memory map (`src/kernel/arm/klib.c:196-199`), so
  these writes can hit the kernel, the process table, FS, or any other process.
- Anyone who can create a file and `chmod +x` it can do this: `/tmp` is mode 777
  (`flipper/rootfs/proto`), and `nobody` apps can write there.
- `:141` also ignores `relocate()` failing, so a broken binary runs half-relocated.

**Fix (0 B RAM, ~60 B flash in MM):** before every write, check
`dbase <= adr && adr + 4 <= dbase + data_bytes` (PIC) or the text+data range (GEMDOS),
and `(adr & 3) == 0`; on failure, or when `relocate()` returns -1, kill the new image
(the exec is past the point of no return, so `sys_exit`-style, like a bad stack copy).
Once every program is PIC, delete the GEMDOS branch entirely (less flash, less attack surface).
Longer term see M12: run MM unprivileged.

### S3. High: MPU silently falls back to "whole RAM" for a process

- `src/kernel/arm/klib.c:276-284`: if a process layout cannot be covered with 7 regions,
  it gets read/write access to the entire 256 KB SRAM window, including the kernel, MM, FS
  and the buffer cache. A process can influence its layout through `chmem`/`brk` sizes.

**Fix (0 B RAM):** fail closed. Either `cause_sig(SIGKILL)` there, or (better) make MM
refuse at exec/brk time any layout that `mpu_cover()` can't express (share the function
or round sizes to coverable subregion multiples; the rounding costs at most one 256 B
subregion per process).

### S4. High: root has no password, and the console is on USB and the GPIO header

- `flipper/rootfs/etc/passwd:1` is `root::0:0:...`; the README says to log in as root
  with no password. Anyone who plugs a USB cable (or touches pins 13/14) gets a root shell.
- There is no `/etc/shadow`: once a password is set, its hash sits in world-readable
  `/etc/passwd`, open to offline cracking.

**Fix (0 B RAM):** ship root as `##root` with a `/etc/shadow` (mode 600) and force a
password at first boot (`/etc/setup` already runs once and is the natural place). The
shadow mechanism already exists in `pwdauth` (`"##"` branch) and `passwd` (`shadow` flag).

### S5. High: `pkg get` installs whatever is in a world-writable directory

- `flipper/cmds/pkg.c:406,477`: `INCOMING` is `/usr/tmp/incoming`, created with
  `mkdir(..., 0700)` and the result is ignored. `get()` (`:521-529`) then installs
  **every** `*.tar` in that directory as root, not only the files just received.
- `/usr/tmp` is created 1777 (`flipper/rootfs/etc/setup`), but MINIX 2.0 FS does not
  implement the sticky bit at all (nothing in `src/fs` tests `S_ISVTX`), so it behaves
  as 777. A `nobody` app can pre-create `/usr/tmp/incoming` and drop a package whose
  `CONTROL/postinst` runs as root the next time root runs `pkg get`.
- The same applies to `pkg.<pid>` (`:255`): predictable name in a shared directory.

**Fix (0 B RAM):** keep the incoming and scratch directories under `/usr/var/pkg`
(root-only), `stat()` after `mkdir` and require `uid == 0 && mode == 0700`, and install only
the names `receive()` itself stored in this run. Optionally implement the sticky-bit
check in FS `unlink`/`rename` (about 10 lines, 0 RAM).

### S6. Medium: any user can halt the system with an unaligned message pointer

- `src/kernel/proc.c:185` checks that a message lies in the caller's memory but not its
  alignment. `cp_mess` compiles to `ldmia`/`stmia` (checked in the build: `cp_mess` at
  `0x08000320`), which always faults on unaligned addresses, in Handler mode, which
  `kernel_fault_report` turns into "System halted after panic".

**Fix (0 B RAM, 4 instructions):** `if ((vir_bytes) m_ptr & 3) return(EFAULT);` in
`mini_send` and `mini_rec`.

### S7. Medium: `pkg` parsing and path handling

- Stack overflows from package contents: `pkg.c:277-279` copies a `Depends:` token of up
  to 255 chars into `rel[PATH_MAX]` (off by one) and then `sprintf`s
  `"/usr/var/pkg/info/" + token + ".control"` into `buf[255]`; `:222` `sprintf(src, "%s%s", tmp, line)`
  overflows when a tar member path is near `PATH_MAX`.
- `Package:` name is used in paths unvalidated (`dbpath`, `:88-91`): `../` escapes the DB.
- `tar xf` is run on untrusted archives (`:268`); MINIX tar does not strip `../`, so a
  member can land outside the scratch directory during unpacking. `pkg remove` unlinks
  every path in the `.list`, including ones like `/bin/sh`.
- Integrity is CRC-32 only; there is no authenticity check, and `postinst` runs as root.

**Fix:** a small `snprintf` (or length checks) in pkg; validate names with the existing
`good_name()` rules; reject members containing `..` or starting with `/` before
extracting (`tar tf` is already run, so check the list first and extract only if clean).
For authenticity: verify a detached Ed25519 signature (e.g. Monocypher's verify is a few
KB of code) or a SHA-256 manifest against a key/hash in the root FS. RAM: transient, a
few hundred bytes of stack while `pkg` runs; pkg's own text grows (RAM today, flash once
the text cache lands). The SHA-256 code is already in `pwdauth.c`.

### S8. Medium: privileged FS and an over-wide FS data segment

- FS runs privileged (`klib.c:199`) and parses the SD card, which is removable media.
  MINIX 2.0's superblock/inode/zone validation is thin, so a crafted card image is a
  path to kernel compromise rather than just an FS crash.
- `klib.c:192` stretches FS's D segment to `SRAM2_END`, which also covers all user
  memory in SRAM1. So `numap()` checks for FS (and requests made in FS's name) no
  longer mean anything.

**Fix:** see M12 (unprivileged FS with an MPU window for its data plus the two SRAM2
blocks). Also add sanity checks in `read_super()` (zone/inode counts versus device size,
map block counts) and in zone lookups. 0 RAM.

### S9. Low-medium: password handling details

- Salt entropy (`src/commands/simple/passwd.c:179-188`): time + pid + ticks. The Flipper has
  no set clock at boot, so salts repeat across devices. Use the STM32WB RNG (already on
  the CPU-specific work list) through `/dev/random`.
- `pwdauth` is set-uid root and answers "is this the password of user X" for any local
  user with no delay: an unlimited guessing oracle (about 1 s per guess on the device).
  Add a failure delay (`sleep(2)`) and/or only allow the `##` lookup when the real uid
  is 0 or the caller is checking its own user.
- `sha256_crypt` clears `a`, `dp`, the contexts, but not `ds` (minor).

### S10. Low: hardening gaps worth closing cheaply

- **W^X with the MPU** (0 RAM): user text in RAM is mapped RW and data/stack are
  executable (`mpu_cover` sets `AP_RW_RW` and never `XN`). Give the text range `AP_RO`
  and data/stack `XN`. Costs one extra region at most.
- **Stack/heap guard** (0 RAM): one high-numbered MPU region with no access, 32 B at the
  current break, catches the stack running into the heap (ARMv7-M has no PSPLIM).
- `flipper/hal/flipper/usb.c:284`: `SET_LINE_CODING` copies 7 bytes without checking
  `req->wLength`. Harmless today (the buffer is 128 B), but check it.
- `/dev/console` is 622: any user can write fake login prompts to the console.
- `launcher` children inherit fd 0 to 2 from `/etc/rc` (`/dev/null`, `/dev/log`), which is
  fine today; if the launcher is ever started from a tty, close/reopen them for apps.

### S12. Critical (new in `5e3d0f0`): the flash text cache can be poisoned with another program's identity

- The cache key is the CRC-32 and size **taken from the executable's header**
  (`a_syms`, `src/mm/exec.c:299`, looked up at `:117`). `fc_fill()` (`src/mm/fcache.c:268-293`)
  writes the file's text into flash and records it under that header CRC; nothing ever computes
  the CRC of the bytes actually written, and `fc_lookup()` (`:119-130`) trusts the match.
- Attack: a user writes a binary whose header claims the CRC and size of `/bin/su` (or
  `passwd`, `/lib/pwdauth`) but whose text is their own. If `su` is not cached at that moment
  (first boot, or after evicting it, and eviction is "oldest first", so running a handful of
  programs does it), the user's text is stored under `su`'s key. The next time anyone runs the
  set-uid `su`, MM finds that entry and runs the attacker's code with root's euid.
- Even computing the CRC would not be enough: CRC-32 is linear, so a 4-byte patch makes any text
  match any CRC.
- Side issue: any user can trigger flash erases at will by executing many distinct PIC
  binaries. Flash pages last about 10,000 erase cycles, so the cache and its two directory
  pages can be worn out on purpose.

**Fix (0 B RAM):**
1. On every cache **hit**, compare the file's text against the flash copy through `mbuf`
   before using it (a read of the text, but no RAM, and no trust in any checksum). On a
   **fill**, the flash content then needs no separate verification.
2. Or key the cache by SHA-256 of the text, computed by MM while filling (SHA code already
   exists in `pwdauth.c`; +32 B per directory record in flash, about 110 B of transient MM stack
   or reuse `mbuf`).
3. Only cache files that unprivileged users cannot have written: owned by root or bin, not
   group/other-writable, on `/` or `/usr`. Everything else runs from RAM as before. This
   also removes the flash-wear attack.

**Simpler alternative adopted in the spec (R1.4):** put the **owner uid of the program file** into
the cache key and only cache files that only their owner (or root) can write. A forged CRC can then
alias text only among one owner's own files, so no read-back comparison is needed. Programs
installed into flash at install time (spec Phase 4b) never use the run-time cache at all.

### S11. Physical access (informational)

The DFU entry (Left+Back) and RDP level 0 mean anyone holding the device can read or
reflash it. That's the right trade-off while developing; RDP level 1 would block flash
readout over SWD but needs care because of the CPU2 radio stack. No action proposed now.

---

## Part 2: Archaic solutions and modernization proposals

Each item notes RAM and flash impact. Nothing here increases RAM; most reduce it.

### M1. `vfork()` / spawn instead of shadow-copy `fork()` (biggest RAM win)

**Archaic:** fork on a machine without an MMU is done the Atari ST way: the whole data+stack
image is copied into a shadow buffer and swapped on every switch
(`src/kernel/arm/shadow.c`). PORTING.md measures ~15 KB per forked shell. Almost every
fork in the system (sh, init, getty, launcher, `pkg run()`) is immediately followed by `exec`.

**Modern:** `vfork()` semantics, as uClinux and every no-MMU Unix do it: the child borrows the
parent's memory and stack, and the parent sleeps until the child calls `exec` or `_exit`. No copy, no
shadow buffer, no swapping. Add a `VFORK` call in MM (flag the parent as waiting, skip
`mkshadow`) and switch `sh`'s simple-command path, `init`, `launcher` and `pkg` to it; keep
real `fork()` for the shell's subshells.

**RAM:** saves one full image copy per fork+exec (≈ 8 to 15 KB peak per pipeline stage).
**Flash:** about +300 B in MM and the kernel. Also removes most shadow swaps (CPU and latency).

### M2. Program text from flash (done in `5e3d0f0`; see S12 before relying on it)

Since `5e3d0f0`, PIC program text runs from a 376 KB flash cache, and QEMU reports 106 KB free
after login (was 73 KB). This also means `const` data in user programs now costs flash, not
RAM, which raises the value of M3 and M7. S12 has to be fixed first, because the cache key is
currently forgeable.

### M3. Move constant tables out of RAM *(measured)*

These are never written, but are declared non-`const`, so they sit in `.data` (RAM, plus
the same bytes again in flash as the load image). Making them `const` moves them to
`.rodata` in flash.

| Image | Symbol | Bytes | Where |
|-------|--------|------:|-------|
| kernel | `__ctype` | 257 | `src/lib/ansi/chartab.c:3` |
| kernel | `tasktab` | 208 | `src/kernel/table.c:136` |
| kernel | `w_dtab`, `m_dtab` | 72 | `src/kernel/arm/disk.c:42`, `memory.c` |
| kernel | `k_environ` | 39 | `src/kernel/arm/klib.c:528` |
| kernel | `X2C_tab`, `x2c_tab` | 34 | `src/lib/other/printk.c` |
| MM | `call_vec` | 308 | `src/mm/table.c:16` |
| MM | `X2C_tab`, `x2c_tab` | 34 | `src/lib/other/printk.c` |
| FS | `call_vector` | 308 | `src/fs/table.c:18` |
| FS | `dmap` (never assigned at run time) | 112 | `src/fs/table.c:115` |
| FS | `X2C_tab`, `x2c_tab` | 34 | `src/lib/other/printk.c` |
| INIT | `_sys_errlist` | 268 | `src/lib/ansi/errlist.c:11` |

**RAM saved:** about 1.65 KB across the boot images, with no flash change.
For PIC user programs, a `const` *pointer* table still needs relocation and lands in
`.data.rel.ro` (RAM). So `_sys_errlist` should become one string plus a `const unsigned short`
offset table (no pointers), and `__ctype` should be `const`. That saves ~260 B to ~520 B of RAM
per process that uses ctype or `strerror`/`perror` (most commands), once M2 is in.

### M4. Static IRQ table *(measured)*

`irq_table` (`src/kernel/glo.h:57`) is 64 function pointers = 256 B of RAM, of which three
are used (UART, USB, clock). A `const` table of the handlers known at build time, or a
`u8_t` index into a small `const` table, makes it 0 to 64 B. **RAM: -192 to -256 B.**

### M5. Remove double buffering in the kernel drivers *(measured)*

- `w_buf[1024]` (`src/kernel/arm/disk.c:32`): a bounce buffer between the SD card and the
  caller. Memory is flat, `numap()` already validated the range, and tasks never preempt
  each other, so data can go straight to/from `user_phys` (set `p_physio` around it so a
  shadow swap can't move the image, as the shadow code expects). **-1024 B.**
- `tmp_buf[512]` (`src/kernel/driver.c:49`): used only to read partition tables. Reuse
  the disk buffer (or a 512 B stack buffer in the WINCHESTER task's existing stack
  headroom: measured 640 of 1024 B used). **-512 B.**
- `fbuf[1024]` (`src/kernel/arm/flipdev.c:240`): a kernel copy of the screen. The app
  already keeps its own framebuffer (`flipper/gui/gui.c:10`). Stream the user's buffer
  to SPI directly, page by page. A `/dev/lcd` read then returns what the last writer
  wrote only if kept; the launcher redraws anyway, so drop read support or keep a 1-bit
  "dirty" flag. **-1024 B.**
- Console input is buffered three times: USB `rx_ring` 256 B (`usb.c`), `ibuf` 256 B
  (`cons.c:19`), and `tty_inbuf` 512 B (`u16_t[256]`, `tty.h:68`). Let `cons_read()` take
  from the USB ring directly and keep `ibuf` only for the UART (or shrink it to 64 B, as
  the UART now has flow control by masking its interrupt). **-192 to -256 B.**

**Total RAM saved by M5:** about 2.8 KB. Flash: roughly neutral.

### M6. K&R C and disabled warnings

**Archaic:** K&R definitions with `_PROTOTYPE` macros, built as `-std=gnu89` with
`-Wno-return-type -Wno-format -Wno-implicit-int -Wno-uninitialized ...`
(`flipper/minix.mk:18-28`). These switches hide exactly the class of bugs found in the
sigreturn and pkg code.

**Modern:** convert new and port-specific code (`src/kernel/arm/`, `flipper/`, the
changed MM/FS/kernel files) to ANSI prototypes and build those with `-std=gnu11 -Wall
-Wextra -Wformat=2 -Werror=implicit-function-declaration -Werror=return-type`. Keep
the old flags only for untouched 1996 files. With real prototypes gcc stops widening
`char`/`short` arguments to `int`, which slightly shrinks code. **RAM 0, flash 0 to slightly smaller.**

### M7. Link-time optimisation

Add `-flto` for kernel, servers and user programs. Typical Thumb-2 `-Os` gains are 5 to 15%
of code. For user programs that's **RAM** today (text is in RAM) and flash later. It
also turns mismatched K&R declarations across files into hard errors, which is useful.
Do it after M6 for the files you care about.

### M8. Word-wise copy primitives

`flipper/hal/common/string.c` uses byte loops for `memcpy`, `memset` and `memmove`, and the
kernel's `phys_copy` and `copyclicks` go through them. Every fork, shadow swap, message
and disk block passes through these. Word/LDM-STM loops (already on the PORTING.md CPU
list) give 4 to 8 times the speed. **RAM 0, flash about +150 B.**

### M9. SD card driver

- One command per 512 B sector (`CMD17`/`CMD24`, `board.c:506-550`) → multi-block
  `CMD18`/`CMD25` for a 1 KB block: about 2 times the throughput, 0 RAM.
- No data CRC: enable `CMD59` and the SPI peripheral's hardware CRC16 for integrity at 0
  cost (the hardware computes it while shifting).
- Polled byte-by-byte SPI → DMA. With DMA, the WINCHESTER task can block during a transfer and let user
  processes run. Then SPI2 needs an owner flag, because LCD (flipdev) and SD share it; today it's safe
  only because tasks never preempt each other. 0 RAM.

### M10. Power: tickless idle and interrupt-driven buttons

SysTick runs at 60 Hz forever, and `flipdev_tick()` polls the buttons on every tick (no debounce).
EXTI interrupts on the six button lines (with a short debounce in the tick only while a
button is down) and stopping SysTick in `idle_task` when no alarm is due are the
battery-friendly modern pattern. **RAM 0, flash about +200 B.**

### M11. Hardware CRC and RNG

Already planned: CRC-32 unit for `pkg get` (replaces the bitwise loop at `pkg.c:410-421`)
and for flash-cache keys; the RNG for salts (S9), package scratch names (S5) and `/dev/random`.
**RAM 0.**

### M12. Unprivileged MM and FS (classic MINIX design, now enforceable)

On the PC, MM and FS were ordinary processes that touched other memory only through kernel
calls. Here they run privileged for convenience (`klib.c:196-199`), which turns any MM/FS bug
(S2, S8) into a kernel compromise. Give them MPU regions like user processes (FS: its data plus
the two SRAM2 blocks; MM: its data), and do the relocation and paint/report through a
`SYS_RELOC`-style kernel call or a temporary MPU window onto the target image.
**RAM 0** (a few more MPU loads per switch). Flash: about +200 B.

### M13. Static stack sizing

Kernel task stacks are sized by painting and run-time high-water marks (`main.c`, `do_kstacks`),
which only sees paths that happened to run. `-fstack-usage` plus a call-graph tool (or
`-Wstack-usage=N`) gives a worst case at build time. Keep the painting as a cross-check.
**RAM 0**, and it may let `TTY_STACK`/`PRINTER_STACK` (768 B each, 352 and 176 measured) shrink safely.

### M14. Small leftovers

- `__functab` (128 B *(measured)* in each of MM and FS): `atexit` table in servers that
  never exit. Stub `atexit` for server builds. **-256 B.**
- `print_buf` (100 B each in MM and FS): could print straight through `putk`-style
  messages per line. **-up to 200 B.**
- `MAXPKG` and similar limits in pkg are fine; `copy()` uses a 512 B stack buffer, fine.

---

## Part 3: Cortex-M4 / STM32WB55 specific features

Same rules: RAM cost for each item, and whether it helps security (**Sec**), modernization or
speed (**Mod**), or power (**Pwr**). Code in the kernel, servers and (since `5e3d0f0`) cached
user text is in flash, so instruction-count changes cost no RAM. Checked against the
current state: `-mfloat-abi=soft` with the FPU off, `lock()` is `cpsid i`, PendSV unused,
ART caches on and flushed after flash writes (good), and no CRC, RNG, AES, PKA, IWDG or HSEM use yet.

### Core (ARMv7-M) features

**A1. `LDRT`/`STRT` for every kernel access to user memory (Sec, 0 B RAM, ~40 B flash).**
ARMv7-M has load/store instructions that run with *unprivileged* permissions even in
Handler mode. Use them in `SAVE`/`restart` (`mpx.S:47-63`, `:165-187`) for unprivileged
processes, and in a small `copy_to_user`/`copy_from_user` pair for `phys_copy` targets that
belong to user processes. The MPU then enforces every bound the C code forgets: S1 (sigreturn
stack pointer) becomes a fault instead of a kernel write. There is no multi-register form, so
it's eight `strt` instead of one `stmia`; a 4-byte loop for bulk copies is still fast.

**A2. Use more of the MPU you already program (Sec, 0 B RAM).**
- *W^X*: user text now runs from the flash cache, so every RAM region of a user process can
  get `XN` (execute-never) in `mpu_cover()`: no code injection into the stack or heap. Flash
  region 0 stays read-only and executable.
- *NULL guard*: address 0 aliases flash on the STM32WB (boot remap), so a user NULL dereference
  silently reads kernel code instead of faulting. A no-access region at `0x00000000` (256 B,
  higher region number than region 0) turns it into SIGSEGV.
- *Stack/heap guard*: a 32 B no-access region at the current break (moves with `brk`).
  ARMv7-M has no `PSPLIM`, and this is its replacement.
- *Fail closed* instead of the whole-RAM fallback (S3).
- *Subregions* are already used well by `mpu_cover()`; CLZ (A6) can replace its 24-step loop.
- Region budget: flash + NULL guard + heap guard leaves 5 regions for the process, which
  `mpu_cover()` almost always manages; when it doesn't, kill the process (S3) rather than
  dropping isolation.
- Keep `PRIVDEFENA`, but stop disabling the whole MPU for flash writes
  (`src/kernel/arm/klib.c:51-59` in `5e3d0f0`): add a temporary privileged-RW region over
  just the cache page being written. Then a stray pointer during the 20 ms write still faults.

**A3. `BASEPRI` instead of `cpsid i` for `lock()` (Sec/Mod, 0 B RAM).**
`lock()` (`klib.c`, 100+ call sites) uses PRIMASK, which also blocks MemManage, BusFault and
UsageFault: a fault inside a locked section escalates to HardFault and loses its cause.
Setting `BASEPRI = KERNEL_PRIO` masks exactly the MINIX exceptions and keeps faults (priority
0) and any future higher-priority interrupt (radio/IPCC, USB timing) live. It's the same cost
(`msr basepri`) and removes the 20 ms total blackout during flash erases.

**A4. PendSV for deferred rescheduling (Mod, RAM: 0 to -0.9 KB).**
Today every exception copies the full hardware frame into `proc[].p_reg` and `restart`
rebuilds it (16 words each way). The standard Cortex-M design is:
- handlers do their work and only *pend* PendSV when a switch is needed;
- PendSV (lowest priority) saves r4-r11 and the PSP and switches.

Interrupts then nest naturally (tail-chaining), and IRQ latency stops depending on kernel
path length. Optional second step: keep the hardware frame on the process stack (it's
process memory and gets shadow-swapped with it) and drop those 8 words from `struct stackframe_s`:
about 32 B × 28 slots ≈ **-0.9 KB RAM**. That step touches every `p_reg.pc`/`sp` user (signals,
exec, ptrace), so it belongs after A1.

**A5. Sleep modes (Pwr, 0 B RAM).**
`idle_task` already uses `WFI`. Next steps, in increasing gain:
- `SEVONPEND` + `WFE` lets masked events wake the core.
- Tickless idle: stop SysTick when no alarm is due (M10).
- STM32WB **Stop2** with **LPTIM1** as the wake timer and EXTI for the buttons, USB VBUS and the UART
  RX pin: microamps instead of milliamps while the Flipper sits at the launcher. SRAM1/SRAM2 are
  kept in Stop2, so nothing needs saving.

**A6. Bit-manipulation instructions (Mod, 0 B RAM).**
`CLZ`, `RBIT` (count trailing zeros = `RBIT`+`CLZ`), `UBFX`/`BFI` and `REV`:
- FS `alloc_bit()` scans bitmaps a bit at a time. Test 32 bits per word and use CTZ for the first free bit.
- `mpu_cover()` loops `k` from 31 down to 8. `CLZ`/`CTZ` of `lo` and `hi-lo` give it directly.
- `irq_dispatch`, the button change loop (`flipdev_tick`) and the signal masks: `CTZ` over the changed bits.
- `REV` in SHA-256 (`pwdauth.c` `sha_block` loads big-endian words with 4 shifts and ORs per word).

gcc emits these from `__builtin_clz`/`__builtin_ctz`/`__builtin_bswap32`.

**A7. Bit-banding and `LDREX`/`STREX` (Mod, 0 B RAM).**
The M4 maps every bit of SRAM1 (0x2000_0000 to 0x200F_FFFF) to a word at 0x2200_0000+. Single-bit
flags shared with interrupt handlers (`uart_held`, `rd_waiting`, `tty_events`, USB
`configured`/`dtr`/`tx_busy`) can be set or cleared atomically without `lock()`. The ring
indices are single-producer/single-consumer and already lock-free. Where a read-modify-write is
needed, `LDREX`/`STREX` replaces masking interrupts. This shortens interrupt-disabled windows; no RAM.

**A8. DSP/SIMD instructions (Mod, 0 B RAM).**
- Word-at-a-time string routines: `UADD8` + `SEL` (or `USUB8` and the GE flags) find a zero byte
  in 4 bytes at once. That makes `strlen`, `memchr` and `strchr` in libc 2 to 4 times faster, with the code in flash.
- `memcpy`/`memset` with `LDM`/`STM` 4-8 word bursts (M8).
- LCD: framebuffer drawing in `gui.c` (the user library) can update 32 pixels per op.
- Later, for the speaker, IR or Sub-GHz demodulation: `SMLAD`/`SSAT`, Q15 filters and Goertzel.

**A9. FPU with lazy stacking, opt-in per program (Mod, 0 B kernel RAM).**
The system is built `-mfloat-abi=soft`, and `EXC_RETURN 0xFFFFFFFD` assumes no FP frame. Soft-float
helpers (~2 to 5 KB) sit in every program that uses `double` (`awk`, `bc`, `printf %f`).
Proposal:
- enable CP10/CP11 with `FPCCR.ASPEN|LSPEN` (lazy stacking);
- build only FP-using programs with `-mfpu=fpv4-sp-d16 -mfloat-abi=softfp`, which keeps the ABI, so one libc still works;
- in `mpx.S`, accept `EXC_RETURN` with bit 4 clear, and save S16-S31 **on the process's own stack** when
  `CONTROL.FPCA` is set.

The extended frame (S0-S15, FPSCR) is pushed by hardware onto the *process* stack, and only
for processes that touched the FPU. Kernel RAM: 0. Per-process stack: +136 B, only for FP programs.
**Sec:** clear S0-S31/FPSCR on exec and before entering a process that didn't own the FPU, so
floating-point registers can't leak data between processes.
Note the hardware is single precision: `double` stays in software either way; the gain is for `float`.

**A10. DWT watchpoint as a kernel stack guard (Sec, 0 B RAM).**
The DWT has 4 comparators (the cycle counter is already used for timeouts). Program one to
trigger a DebugMonitor exception on a write to the lowest word of `kernel_stack`, and move it to
the current task's guard word on each switch. That catches stack overflows at the faulting store,
instead of the `STACK_GUARD` word being checked later. It doesn't work while a halting debugger
is attached, which is fine for production.

### STM32WB55 peripherals

**A11. CRC unit (Mod, 0 B RAM).** Use it for `pkg get` (`pkg.c:410-421` bitwise loop) and the SD
data CRC (M9). **Not** as an identity: S12 shows why a CRC must never decide which code runs.

**A12. True RNG (Sec, 0 to 4 B RAM).**
- Password salts (S9).
- Unpredictable package scratch names (S5).
- `/dev/random`.
- A per-boot stack-protector canary: with text in flash, `-fstack-protector-strong`
  costs only flash plus a 4 B `__stack_chk_guard` per program (+4 B RAM per process).
- Random fill for the SHA-256-crypt rounds salt.

Follow the reference manual's health checks: CECS/SECS error bits; discard the first word after a seed error.

**A13. AES1 accelerator (Sec, 0 B RAM).**
- Encrypt swap (memory plan point 3): AES-CTR with a fresh key from the RNG each boot.
  The key stays in AES1's key registers, never in SRAM, and swapped-out process images
  (which may contain passwords typed into `su`) are unreadable from a removed SD card.
- Optional encrypted `/home` with a key derived from the login password.
- A fast keyed MAC (AES-CMAC) if you ever want a cryptographic text-cache key without SHA-256 cost.

Keep off **AES2** and the PKA while CPU2's BLE stack runs, or arbitrate with HSEM (A16).

**A14. PKA (public key accelerator) for signed packages (Sec, ~0 B SRAM).**
The PKA does ECDSA P-256 verification in hardware, with its operands in the PKA's **own RAM**,
not SRAM1. Package signing (S7) becomes:
1. SHA-256 the tar while receiving (software, about 110 B context in `pkg`'s stack);
2. one PKA ECDSA-verify against a public key stored in the root FS (or, better, in flash next to the kernel).

The PKA also does the point-on-curve check, which is what makes verification safe against invalid-curve inputs.

**A15. Flash protection (Sec, 0 B RAM; decided: ECC now, the rest after BLE).**
- **WRP** on the kernel/server area (first 128 KB): even a kernel bug in `arm_flash()` or in the
  flash HAL cannot rewrite the kernel. DFU can still clear it, but every development reflash
  then needs an option-byte step.
- **PCROP**: execute-only flash for a small routine that holds a device key (e.g. the package
  signing *public* key needs no secrecy, so PCROP matters only for secrets such as an
  encrypted-home key-wrapping key).
- **RDP level 1**: blocks SWD/flash readout by someone holding the device; DFU still works.
- **Flash ECC**: the WB flash has ECC; enable the ECCD NMI and ECCC interrupt, and invalidate a
  text-cache entry whose page reports errors instead of running corrupted code.

Caution: all of these live in option bytes, which PORTING.md says the project never touches,
and on the STM32WB they share the area with CPU2/FUS settings (`SFSA`, `SBRV`).

**Decision (2026-11-14, by the project owner):** enable flash ECC handling now; it needs no
option-byte change. Defer WRP, PCROP and RDP level 1 until Bluetooth (milestone 12) works,
then revisit with the CPU2/FUS layout known.

**A16. HSEM for flash and shared peripherals (Mod, 0 B RAM, required before BLE).**
Once CPU2 runs (milestone 12), ST requires CPU1 to take hardware semaphores before programming
or erasing flash and before using RNG, PKA or AES2, and to respect `PESD`. The new flash HAL already
notes this (`board.c` comment, RM0434 3.3.8). Wire it in before BLE, or the flash text cache will
collide with the radio stack's flash writes.

**A17. Independent watchdog (Sec/Mod, 0 B RAM).**
IWDG, refreshed from the clock task: a hung kernel (a stuck SPI wait, a lock held too long,
an interrupt storm) reboots instead of freezing the device. Pick the timeout above the
worst-case flash erase (~25 ms) plus a safety margin, e.g. 2 s. Once started it can't be stopped,
so `hal_halt` should keep refreshing it or deliberately let it reset.

### Summary of Part 3

| Item | Gives | RAM |
|------|-------|-----|
| A1 LDRT/STRT | Closes the S1 class for good | 0 |
| A2 MPU XN, NULL and heap guards, scoped flash window | W^X, catches NULL and stack overflow | 0 |
| A3 BASEPRI | Faults stay visible; shorter blackouts | 0 |
| A4 PendSV | Nestable IRQs; optional slimmer `proc` | 0 to -0.9 KB |
| A5 WFE / Stop2 + LPTIM | Battery | 0 |
| A6 to A8 CLZ/REV/bit-band/LDREX/SIMD | Speed, less masking | 0 |
| A9 FPU, lazy, opt-in | Fast `float`, smaller FP programs | 0 kernel, +136 B stack per FP process |
| A10 DWT watchpoint | Precise kernel stack overflow detection | 0 |
| A11 CRC unit | Faster `pkg get`, SD CRC | 0 |
| A12 RNG | Salts, temp names, canaries | 0 (+4 B per process with canaries) |
| A13 AES1 | Encrypted swap, keys outside SRAM | 0 |
| A14 PKA | Signed packages | ~0 SRAM |
| A15 WRP/PCROP/RDP/ECC | Tamper and corruption resistance | 0 (option bytes: your call) |
| A16 HSEM | Safe coexistence with CPU2/BLE | 0 |
| A17 IWDG | Recovery from hangs | 0 |

---

## Part 4: Swapping to the SD card (design proposal)

The repository has no swap code yet (`5e3d0f0`). PORTING.md reserves partition 3 (`/dev/hd3`,
64 MB) and lists swap as memory plan point 3. This is the design I recommend. The phased
requirements are in the specification document (`galtzagorriak-spec.md`, Phase 5).

### The constraint that shapes everything

Without an MMU, a process's data and stack hold absolute pointers: heap pointers, saved frame
pointers, return addresses, and the GOT that `exec` filled in. A swapped-out image can therefore
only come back **to exactly the same addresses**. Relocating on swap-in (what MINIX 2.0.4's PC
swapper could do with segment registers) is impossible here. So this is "swapping by address
range", like the existing shadowing (`shadow.c`), with the shadow kept on the SD card instead of
in RAM:

- **Swap-out:** write the image to the card and give its RAM back to MM's hole list.
- **Swap-in:** the original range must be free. If other processes now live there, they are
  swapped out first (if eligible), then the image is read back in place.

### Granularity: the whole process range, with the MPU in mind

- **Unit:** one process at a time, covering its full range `[D.start, S.end)`. That is exactly
  what `mpu_switch()` grants the process. Swapping only part of the range and giving the hole to
  someone else would leave two processes inside one MPU-covered range, which breaks isolation.
  So the range is freed and reclaimed as a whole.
- **I/O can skip the gap:** only `[D.start, brk)` and `[sp, S.end)` are written and read back.
  On swap-in, the gap is **zeroed** (it may hold another process's old data: an information leak otherwise).
- **Text is never swapped:** PIC text lives in the flash text cache (`5e3d0f0`), shared and
  read-only. `fcache.c` already counts a text entry as in use while any `mproc` slot points at
  it, and a swapped-out process keeps its `mp_seg[T]`, so its text stays pinned automatically.
  Processes whose text fell back to RAM (cache full, non-PIC) are not eligible in the first phases.
- **Not eligible:** MM, FS, INIT, traced processes, processes in a shadow (fork) group (until
  phase 5c), and any process with `p_physio` set.

### When a process may be swapped (the safe states)

Data reaches a user process only through (a) reply messages from MM or FS and (b) kernel or
driver copies into its buffers (TTY input, button events, disk reads). Writing either into a
swapped-out range would corrupt whoever lives there now. So eligibility starts with the states
where **MM controls the wake-up**:

- blocked in `wait()`/`waitpid()` (`WAITING`, `src/mm/forkexit.c:265`): every shell running a
  foreground command, and the **launcher while an app runs**;
- blocked in `pause()`/`sigsuspend()`/`sleep()` (`PAUSED`, `src/mm/signal.c:320`).

MM swaps the process back in **before** it sends the reply or delivers a signal (`sendsig`
writes to the stack). These cover the processes that sit idle holding RAM in practice.

A later phase (5c) adds processes suspended in FS (tty reads, pipes). That needs a kernel
"defer and ask MM" path in `umap`/`numap`/`cp_mess` and in the TTY and flipdev revive code.

**Kernel safety net:** a `P_SWAPPED` bit in `p_flags` keeps the process off the run queues, and
makes `umap`, `numap`, `proc_vir2phys` users and `cp_mess` refuse it (with a console warning)
instead of writing into memory that now belongs to someone else.

### Where and how the I/O goes

- **Device:** the raw partition `/dev/hd3`, set by a root-only `swapon` call from `/etc/rc`
  (an MM `svrctl`, as in later MINIX 2 versions). Use the partition, not a swap file in a file system.
- **Path:** MM sends `DEV_WRITE`/`DEV_READ` **directly to the WINCHESTER task** with
  `PROC_NR` = the process being swapped, so the driver's existing `numap()` and copy code apply
  unchanged. FS is not involved, so FS waiting on a swapped process can't deadlock the swapper.
- **Slots:** fixed, one per `mproc` index. A slot's size is the largest possible user image (≤
  192 KB), so 16 slots take 3 MB of the 64 MB partition. No allocation bitmap is needed.
  Sector 0 of each slot is a header: lengths, addresses, nonce counter, GCM tag.
- **Trigger:** on demand only, when `new_mem` (exec), `do_fork` or `do_brk` can't find a hole.
  Pick victims whose removal produces the largest contiguous hole; break ties by
  longest-blocked. Keep a minimum resident time (e.g. 1 s) against thrashing. No background
  swapping: it would wear the card and cost latency for nothing.
  *(Superseded 2026-11-14 by spec R12.5 (amended): a limited idle-priority reserve swap-out is
  needed for the launch budget L6.)*
- **Swap-in:** needs an exact-range allocation in MM (`alloc_mem_at(base, len)` in `alloc.c`).
  If the range is held by non-eligible processes, the wake stays pending and is retried
  whenever memory is freed. `free -v` shows such processes as "swapped, waiting for range".
- **Speed:** a shell is ~12 KB of data+stack, about 40 ms at today's single-block SD rate, and
  less with multi-block transfers (M9).

### Encryption with AES1 (and why it's not optional)

Swapped images contain whatever the process held, including passwords typed into `su` or
`passwd`, and the SD card is removable. So swap is encrypted **and authenticated**:

- **Algorithm:** AES-GCM in the AES1 peripheral (the STM32WB AES supports GCM). Authentication
  matters: with plain CTR, someone with the card could flip bits in a swapped stack (return
  addresses) and get code execution when it is swapped back in.
- **Key:** 128 or 256 bits from the hardware RNG at boot, written straight into AES1's key
  registers. Those are write-only, so the key never sits in SRAM, `/dev/mem` can't read it, and
  swap contents are useless after a reboot (swap isn't meant to persist).
- **Nonce:** 64 random bits per boot plus a 32-bit per-slot write counter kept **in MM's RAM**.
  Keeping the counter in RAM prevents rolling a slot back to an older image from the same boot.
- **Associated data:** slot, pid, addresses and lengths, so an image can't be moved to another
  slot or process.
- **Tag:** stored in the slot's header sector, written from MM's existing `mbuf`.
- **Where the crypto runs:** in the WINCHESTER task for the swap minor, on the existing 1 KB
  bounce buffer `w_buf`. If M5 removes `w_buf`, encrypt the image in place on swap-out (that RAM
  is being released anyway; decrypt again if the write fails) and decrypt in place on swap-in.
  Either way: 0 extra buffer RAM.
- **QEMU:** mps2-an386 has no AES1. Use a software AES-GCM in the QEMU build only (flash cost
  only), so tests run there.
- **Constraints:** AES1 is then dedicated to swap. Any other use (e.g. encrypted `/home`) must
  save and restore the key, which would put it in RAM: avoid it, or use AES2 later under HSEM rules.

### RAM cost

| Item | RAM |
|------|-----|
| Kernel: `P_SWAPPED` flag in `p_flags` | 0 |
| MM: per process, write counter (4 B) and saved stack length (4 B) | 16 × 8 = 128 B |
| MM: boot nonce prefix | 8 B |
| MM: swap device and state | ~8 B |
| Driver: crypto on `w_buf` or in place | 0 |
| Code (MM, driver, AES/GCM glue) | flash only, ~2–3 KB |
| **Total** | **~150 B**, paid back by the first process swapped out (≥ 8 KB) |

### Interaction with the other proposals

- **M1 (vfork)** removes most transient shadow copies. Swap then mainly targets idle parents
  (shells, the launcher), and later (5c) long-lived shadow copies from real forks.
- **A12 (RNG)** and **A13 (AES1)** are prerequisites. **A16 (HSEM)** applies to the RNG once CPU2 runs.
- **M9 (multi-block SD)** makes swap 2–4 times faster. If SD moves to DMA, the SPI2 owner flag must
  cover swap I/O too.
- **S12** must be fixed first: a swapped-in process trusts its cached text.

---

## Suggested order

1. S1, S2, S12, S6 (each a few lines; S1, S2 and S12 are local root). A1 is the durable fix behind S1.
2. S3, S5, S4 (fail-closed MPU, package directory, root password and shadow).
3. M3, M4, M5, M14 (about 5 KB of kernel/server RAM back with no behavioural change).
4. M1 (M2 is now done; fix S12 first).
5. M6/M7, M8, M9, M10, M12 as ongoing work.
6. From Part 3: A3, A2 and A17 early (cheap, protective); A12 and A11 with S9/S5; A16 before BLE;
   A13/A14 with swap and signed packages; A4 and A9 when there's time.

Estimated RAM recovered by M3 to M5 and M14 alone: **about 5 KB** of the ~55 KB that the kernel and
servers use, before vfork and the text cache, which save far more per process.

> **Note (2026-11-14):** `galtzagorriak-spec.md` Revision 2 supersedes the order below. It re-plans all items for feature parity with the stock firmware (20 phases, app porting, networking).
