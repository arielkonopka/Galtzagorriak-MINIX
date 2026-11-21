# 1. Architecture

How Galtzagorriak MINIX is put together, for people changing it. The engineering log
[`flipper/PORTING.md`](../../../flipper/PORTING.md) has the history and the measurements behind
each decision; this chapter is the map.

## 1.1 The source tree

| Path | What |
|---|---|
| `src/kernel/` | the MINIX 2.0 kernel: `proc.c` (IPC, scheduling), `system.c` (kernel calls), `clock.c`, `tty.c`, `memory.c` (`/dev/mem`, `/dev/rom`, `/dev/random`, ...), `table.c` (the task table). PC drivers are still there but not built. |
| `src/kernel/arm/` | everything ARM-specific: `mpx.S` (entry/exit, SVC, PendSV), `klib.c` (boot, interrupts, MPU, faults, flash, ms timer, Stop2), `shadow.c` + `flip.S` (fork shadows), `cons.c` (console), `disk.c` (SD card task), `flipdev.c` (the Flipper device task), `lat.c` (latency trace) |
| `src/mm/` | the memory manager: processes, `fork`/`exec`/`exit`, signals; plus `fcache.c` (run-time text cache) and `finst.c` (installed programs) |
| `src/fs/` | the file server: MINIX V2 file system, pipes, devices |
| `src/lib/` | libc; `src/lib/arm/rts/` has the ARM start-up and assembly (`crtso.S`, `_sendrec.S`, `memops.S`, `_vfork.S`) |
| `src/commands/` | the MINIX commands (`simple/`, `sh/`, `mined/`, ...) |
| `src/tools/init.c` | the real INIT |
| `include/` | headers; `include/minix/config.h` defines `MACHINE == FLIPPER`, `CHIP == ARM_CM` |
| `flipper/hal/` | the hardware layer: `hal.h` (the interface), `flipper/board.c` (STM32WB55), `qemu/board.c` (mps2-an386), `common/` (vectors, kprintf, framebuffer console, font, splash) |
| `flipper/ld/` | linker scripts: the kernel per board, `server.ld` for MM/FS/INIT, `user-pic.ld` for programs |
| `flipper/cmds/`, `flipper/gui/`, `flipper/apps/` | programs written for the port: commands, screen apps, packaged apps |
| `flipper/rootfs/` | the root file system prototype (`proto`), `/etc`, the ROM image's prototype, the base package's `postinst` |
| `flipper/tools/` | host tools in Python: image and package builders, `elf2mnx.py`, `flsend`, the checkers (`ramcheck.py`, `flashcheck.py`, `versioncheck.py`, `powercut.py`) |
| `flipper/m2/`, `flipper/tests/` | the self-test (`m2test`, test INIT) and the security tests |
| `flipper/vendor/` | CMSIS headers and `libusb_stm32` |
| `docs/spec/`, `docs/manual/`, `docs/devguide/` | design, user manual, this guide |

## 1.2 What runs

MINIX 2.0's structure is kept: a small kernel with **tasks**, two **servers**, and INIT, all
linked separately.

- **Kernel tasks** (privileged, in the kernel image, one stack each, `src/kernel/table.c`): TTY
  (USB CDC console, debug UART, the LCD console), WINCH (the SD card, `arm/disk.c`), SYN_AL,
  IDLE, PRINTER (renamed in effect: the Flipper device task `flipdev_task`), FLOPPY (`nodev`,
  only so the numbering holds), MEMORY, CLOCK (also the ms timer and power management), SYS
  (kernel calls).
- **MM and FS**, the servers, each a separate image. User processes talk to them with messages;
  they talk to tasks with messages.
- **INIT**, then `/etc/rc`, `getty`, `login`, `sh`, the launcher.

Messages are MINIX's: fixed-size `message` structs, `send`/`receive`/`sendrec`, entered through
`SVC`. The kernel never blocks on a user process.

## 1.3 Boot

1. `hal_early_boot()` (board code) runs first: Left held, or a DFU magic word left by
   `reboot -d`, jumps to ST's ROM DFU loader. It also resets once after a DFU jump so the flash
   controller starts clean.
2. `cstart()` (`arm/klib.c`) sets up the clock tree, the console, the vector table, copies the
   servers' initialised data from flash to RAM, and shows the splash.
3. `main()` (`src/kernel/main.c`) builds the process table; `arm_proc_layout()` gives the boot
   images their memory maps (virtual = physical); the first process is entered with `svc`.
4. MM and FS initialise. FS mounts the root file system from the SD card.
5. INIT runs `/etc/rc`: mounts `/rom` (default settings), runs `/etc/setup` on the first boot,
   mounts `/usr` and `/home`, binds the programs built into flash (`pkg bind /bin`), reads
   `rc.conf`, starts `update` and the launcher. Then INIT starts the logins of `/etc/ttytab`.

Budget L10 (boot to launcher menu) is 3 s.

## 1.4 Processes on a Cortex-M4

- All processes run in Thread mode on the process stack (PSP). Tasks run privileged; MM, FS and
  user processes unprivileged. The kernel itself runs in Handler mode on the main stack (MSP).
- SysTick and device interrupts share one priority; SVC is one level above. So kernel entries
  never nest (`k_reenter` is only ever -1 or 0). Radio and pulse-engine interrupts that must not
  be delayed are to sit above the kernel's lock level (R0.8a, R4a.1, with the radio drivers).
- On entry, the whole register set goes into `proc_ptr->p_reg`; `restart` builds the hardware
  frame on the process stack again.
- **FPU** (R5.7): lazy, and nothing in the process table. A process that used the FPU gets an
  extended frame (S0-S15, FPSCR) from the CPU, written only when needed (lazy stacking);
  `p_reg.control` gets `CONTROL_FPCA` and `p_reg.sp` points 64 bytes below the frame. Its
  registers stay in the FPU (`CONTROL_FPLIVE`, only ever on `mpu_proc`) while the kernel runs and when it returns to the same
  process. Only when `restart` enters another process does `fp_flush` store S16-S31 there (which
  also makes the CPU do the lazy S0-S15 save) and clear the FPU, so nobody sees another
  process's registers; `restart` loads S16-S31 back when the process runs again. All stores
  and loads use the process's own rights (`strt`/`ldrt`, the lazy save with its MPU regions);
  a bad stack gives it SIGSEGV (`fp_frame_recover`). The kernel, servers and libc are
  soft-float and never use the FPU. Signal handlers start without an FP context; the
  sigcontext keeps `CONTROL_FPCA`, so `sigreturn` finds the interrupted one again. Tests:
  `/bin/fptest` (`m2/fptest.c`, run by `m2test`).
- **MPU**: on every switch, `mpu_switch()` gives the process its own memory and read-only
  flash. Small or oddly aligned blocks are covered with several regions (`<minix/mpucover.h>`).
  There is no unprotected mode.
- **Shadowing** (from the Atari ST port): after `fork` the child shares the parent's addresses;
  the kernel exchanges the two data images on a switch (`shadow.c`, `flip.S`). The idle task
  raises PendSV so a ready shadowed process is swapped in without waiting for the next tick.
  Otherwise the clock swaps a group member in only at the end of a 100 ms quantum, because
  each swap copies the images (#331). The swapped-in member goes to the tail of the user
  queue, so processes already waiting there (the launcher with a key to draw) run first.
  On the shadow queue a woken member (`ready()`) goes to the head and a preempted resident
  goes to the tail. Before a flip, `unshadow()` saves the FP registers that are still live
  in the FPU (`fp_save`), so that `fp_flush` cannot later write them into the other image.
- **Scheduling**: MINIX's queues (tasks, servers, users, plus a shadow queue). Tasks are never
  preempted by tasks. The interactive queue for the foreground app is planned (R0.8a).
- **Programs** are position-independent (`r9` = data base). MM places `exec` images at the top
  of RAM and fork shadows at the bottom.

## 1.5 Memory

| Range | Use |
|---|---|
| `0x20000000`, 8 bytes | DFU request word (survives a reset) |
| SRAM1 up to `0x20030000` | kernel data and task stacks, MM, FS, INIT data, then all user processes |
| SRAM2a / SRAM2b, 10 KB each usable | the FS buffer cache (the rest of SRAM2 is the radio CPU's) |

Static RAM of each image is budgeted in `flipper/ram-budget.txt` and checked by `make ramcheck`.
At the time of writing: kernel about 14.8 KB, MM 10.2 KB, FS 27.2 KB (most of it the buffer
cache in SRAM2), INIT 0.8 KB. About 105 KB is free for user processes after boot.

Bluetooth (Phase 15) needs part of SRAM2 and the IPCC mailbox; the buffer cache will have to
share.

## 1.6 Flash

| Region | Written by | Contents |
|---|---|---|
| system image, from `0x08000000`, ≤ 192 KB | DFU only | kernel, MM, FS, INIT (`minix.bin`); the build fails above 192 KB |
| shared libc, from `0x08030000`, ≤ 48 KB | DFU only | the C library, once, for every program (`minix.bin`; spec R2b.9, see below) |
| shared libfuri + libapp, from `0x0803C000`, ≤ 48 KB | DFU only | the GUI and app library, once, for the apps that use it (`minix.bin`; R2b.9, see below) |
| shared Berry interpreter, from `0x08048000`, ≤ 96 KB | DFU only | the Berry VM and bindings with their constant tables, once, for every running script (`minix.bin`; R7c.1, issue #431) |
| installed region, from `0x08060000` | `make base-flash` (DFU), `pkg` at install | program texts with their directory (two pages); the ROM image (32 KB) right after the directory |
| run-time text cache, 128 KB | MM in the background | texts of programs run often, within a daily erase budget |
| guard pages, then the radio stack and FUS | never | the top of flash, from `SFSA` (read from `FLASH->SFR` at boot) |

`hal_flash_layout()` computes the regions at boot. The kernel refuses writes outside the two
writable regions and inside the ROM image (`EROFS`). Every erase is counted in RTC backup
registers (`free -v`). Erases run with interrupts enabled; the CPU simply stalls on fetches for
about 22 ms per page.

**The shared libc** (`<minix/shlibc.h>`, `tools/mkshlibc.py`, `ld/shlibc.ld`). The libc members
are linked once at a fixed address, `SHLIBC_BASE`; the image starts with a header (magic, ABI,
build id, sizes) and the *entry table*, one word per exported function. `flipper/libc.exports`
fixes the order of that table for the ABI: append new functions at the end, never reorder or
remove one without a new `GZ_ABI` (the build tells you what to append). A program calls slot *n*
through a 12-byte stub in its own text (`ldr ip, =slot; ldr pc, [ip]`), so imports cost no RAM.

The library's writable data (its GOT, `errno`, stdio, `malloc` and `atexit` state; about 0.6 KB)
is per process: the first bytes of every data segment, the *libc area*, at `r9` like the
program's own GOT, which `ld/user-shlibc.ld` places after the area. `elf2mnx.py --libc` fills the
area with the library's initial contents, so a program fits only the library build it was
linked with: the a.out header is 48 bytes, flag `A_LIBC`, `a_tbase` = the build id. MM's exec
refuses any other (`ENOEXEC`); `mkpkg.py` writes `Libc: <id>` and `pkg` refuses a mismatch. The
kernel accepts the library's flash as the *source* of a copy (its strings passed to system
calls), never as a destination.

Members with big per-process data stay static, linked into the programs that use them
(`SHLIBC_STATIC` in `minix.mk`: password and group files, `mtab`, `crypt`, `scanf`, the time
zone code, the float tables, `regexp`, `getopt`, `termcap`, math, ...). `SHLIBC=0` builds every
program static, as before.

**The shared libfuri + libapp** (the "furi" library; `ld/shfuri.ld`, `flipper/furi.exports`,
`tools/mkshlibc.py image --dep`). `libfuri` (core, GUI, u8g2, fonts) and `libapp` are linked once
at `SHFURI_BASE`, right after the libc's slot, with the same header (magic `GZLF`) and a frozen,
append-only entry table. Only the functions the apps call are exported, and only what they reach
is in the library (`--gc-sections`): a ported app that needs another function appends it to
`furi.exports`. The library calls the libc through the libc's stubs, so its per-process data
(its GOT, data and bss, the *furi area*, about 2.4 KB) follows the libc area at `r9`, and its
build id covers the libc's. Only the programs in `FURIPROGS` (`minix.mk`; `ld/user-shfuri.ld`,
`elf2mnx.py --furi`) carry that area; `a_dbase` of their header holds the library's id, which
exec checks like `a_tbase` (`ENOEXEC` on a mismatch, `0` = not used), and `mkpkg.py` writes
`Furi: <id>`, which `pkg` checks. The static libc members the library needs (`localtime`,
`mktime`) are inside it and exported, so an app does not get a second copy.

## 1.7 RTC backup registers

The battery-backed RTC registers hold state that must survive a reset without costing RAM or a
flash write. MINIX uses them from the top; the stock firmware uses the low ones (spec I.3).

| Register | Use |
|---|---|
| BKP19R | "GZ" magic, dev-mode bit, cause of the last reset |
| BKP18R | run-time cache erase budget: budget, pages erased today, the day |
| BKP17R | run-time cache pages erased in all |
| BKP16R | installed-region pages erased in all |
| BKP15R | backlight: brightness, idle timeout, dimming state |

A new user of a backup register adds a row here and in `PORTING.md`.

## 1.8 What is not MINIX 2.0 any more

A quick list for people who know the original:

- the kernel and most program text execute from flash; programs are PIC and get relocated by MM;
- `vfork`, a flash text cache, an installed-program region, a ROM settings image (`/dev/rom`,
  `cfopen`);
- SHA-256 password hashes, a hardware `/dev/random`, many security fixes (spec Phase 1, the
  modernisation report);
- a millisecond timer, tickless idle with Stop2 sleep, an independent watchdog;
- foreground ownership of the screen and buttons;
- version `Galtzagorriak 0.x` with its own ABI number (spec J); `uname` says so.
