# MINIX 2.0.0 on Flipper Zero — bare-metal port plan

Goal: replace the Flipper Zero firmware with a native MINIX 2.0.0 kernel
(kernel + MM + FS + INIT) running directly on the STM32WB55, using a USB serial
console when one is connected and an on-screen tty (LCD + d-pad keyboard)
otherwise.

## Hardware status (2026-11-13, real Flipper Zero)

MINIX boots on the device: kernel, MM, FS, init, login and the shell run
with the root file system on the microSD card (3.7 GB card detected), and
the console is the USB serial port (`/dev/ttyACM0` on the host).  Verified:
boot after DFU, 64 MHz clock, SysTick, LCD, backlight + RGB LED (LP5562 on
I2C1), buttons, USB CDC, SD card over SPI, file writes + sync.

## Distribution: Galtzagorriak MINIX (decided 2026-11-13)

A small but complete distribution with a package manager, to add apps:

- **Card layout**: the image written to the card holds the MBR and a small
  root partition only; a first-boot script creates the rest below 4 GiB:
  1 `/` 32 MB, 2 `/usr` ~2 GB (packages), 3 swap 64 MB (reserved for future
  process swapping), 4 `/home` the remainder.
- **Status (real Flipper, 2026-11-13)**: boot screen, first boot
  (partition, reboot, mkfs, mount: / 32 MB, /usr 2 GB, swap 64 MB, /home
  1.6 GB on a 3.7 GB card), and `pkg install` + running `hello` all work
  on the device.  Launcher (main screen, /etc/apps, apps as nobody in
  group gui), button navigation and the About app work on the device
  (2026-11-14).
  The art theme follows the Basque galtzagorriak (helpful gnome-like
  spirits); it belongs to the LCD/menu milestone.
- **Packages**: opkg-style, kept tiny: `pkg` is a C program (a shell script
  needed two ~35 KB shells at once and ran out of memory), (install,
  remove, list, info, files) on MINIX tar; a package is `name_version.tar`
  holding `CONTROL/control` (Package, Version, Depends, Description),
  optional `CONTROL/postinst`/`prerm`, and the files relative to `/`.
  Database in `/usr/var/pkg`.  Built on the PC by `flipper/tools/mkpkg.py`.
- **Verified on the Flipper (2026-11-14)**: `flsend hello_1.0.tar` ->
  installed and runs (~105 KB/s over USB).
- **Transport**: `tools/flsend file.tar` on the PC types `pkg get` on the
  USB console and sends each file as `FILE name size crc32` + raw bytes;
  pkg answers OK/BAD (resend).  ~26 KB/s in QEMU.  (ZMODEM was tried: the
  1990 MINIX rz NAKs today's lrzsz.)  Bluetooth serial later as another
  link.

## Licensing

- MINIX 2.0.0: `src/LICENSE` in this tree is the original 1997 Prentice-Hall
  agreement, but MINIX was relicensed under BSD-3-Clause in April 2000,
  retroactively for all earlier versions
  (<http://minix1.woodhull.com/current/LICENSE.html>).
- Decision (2026-11-13): code from the official Flipper Zero firmware
  (GPL-3.0) may be reused for the hardware features and the menu UI.  This
  is a private project that is not distributed, so licence obligations do
  not arise; should that ever change, the combination would be GPL-3.0
  (BSD-3 MINIX and the Apache-2.0 CMSIS headers are compatible).  Keep the
  original copyright notices on reused files.
- Order of work (same date): shell and commands first, then the Flipper
  hardware milestones.

## Target hardware (STM32WB55RG, verify pins against flipperzero-firmware
`targets/f7/furi_hal/furi_hal_resources.c` before relying on them)

| Resource      | Detail                                                        |
|---------------|---------------------------------------------------------------|
| CPU           | Cortex-M4F @ 64 MHz (HSE 32 MHz → PLL), no MMU, 8-region MPU  |
| Flash         | 1 MB @ 0x0800_0000. Top part holds CPU2 radio stack + FUS — never erase it. We use the bottom ~512 KB. |
| SRAM          | SRAM1 192 KB @ 0x2000_0000; SRAM2a/2b @ 0x2003_0000/0x2003_8000 — only the first 10 KB of each is usable, the rest is secure (CPU2/FUS option bytes) |
| Display       | ST7567 128×64 mono on SPI2 (CS PC11, RST PB0, A0 PB1)         |
| microSD       | SPI2 shared with display (CS PC12, detect PC10)               |
| Buttons       | Up PB10, Down PC6, Left PB11, Right PB12, OK PH3, Back PC13   |
| Debug UART    | USART1 TX PB6 / RX PB7 = GPIO header pins 13/14               |
| USB           | USB FS device (PA11/PA12) → CDC-ACM console                   |
| Backlight/LED | LP5562 on I2C1 (PA9 SCL / PA10 SDA)                           |

### Flashing and recovery

The chip always boots from flash (option bytes `nSWBOOT0=1`, `nBOOT0=1`); we
never change option bytes or touch the radio stack.

| Situation | How to reach ST's ROM USB DFU |
|-----------|-------------------------------|
| Stock firmware or any MINIX image | Hold **Left + Back** (hardware reset), release Back, keep holding Left. Stock firmware and our `hal_early_boot()` both check Left as their first action and jump to DFU. |
| Image broken before that check | Hardware DFU: hold **Left + Back + OK**, release Back and Left, keep holding OK. |

Without the buttons (2026-11-14):

- `reboot -d` (root) restarts into ST's ROM DFU loader (RBT_MONITOR: there
  is no boot monitor; hal_dfu_request() leaves a magic word at 0x20000000,
  the 8 bytes the RAM map skips, and resets; hal_early_boot() sees it).
- Independent watchdog, 2 s (R4a.4, Phase 4a, 2026-11-15), kicked by the
  clock *task*; the clock interrupt wakes it at least every 32 ticks
  (0.53 s).  Tasks are never preempted by tasks, so a kernel task that
  never gives the CPU back, an interrupt storm, or a panic (hal_halt()
  stops kicking, the message still gets out over USB) resets the board.
  A halt keeps kicking.  Before this the ISR kicked it: a task stuck in
  `for(;;)` with interrupts on never reset.
- RTC backup registers used by MINIX (battery domain; the stock firmware
  uses the low ones, MINIX takes them from the top, spec I.3):

  | Register | Use |
  |---|---|
  | BKP19R | "GZ" magic, dev bit, reset cause (watchdog, R4a.4) |
  | BKP18R | run-time cache erase budget (R2b.11): magic 0xA, budget (8 bits), pages erased today (8), the day (12: from the RTC date, or days of uptime when the RTC does not run) |
  | BKP17R | run-time cache pages erased in all |
  | BKP16R | installed-region pages erased in all |
  | BKP15R | backlight: magic 0xB, off by the idle timeout (bit 27), dimming step (bits 24-26), brightness (bits 16-23), idle timeout in seconds (bits 0-15; 0 never, default 15) |

- Where a watchdog reset goes: RTC backup register 19 ("GZ" magic) holds
  a dev bit and the reset flags of the last reset.  `reboot -d` sets the
  dev bit, a normal reboot or halt clears it.  Dev bit set: watchdog reset
  -> DFU (a broken new image comes back by itself).  Clear (a user's
  board): it boots normally, as the stock firmware does.  `ver` prints
  the cause (`reset: watchdog`, `software`, `power on`, ...; MEMINFO
  MI_RESET) and "dev mode"; the boot banner too, but USB is not up yet.
  After DFU + flashing the cause reads "software" (the loader's jump is
  followed by one reset, see hal_early_boot).
  Tested (AC4a.4) with a temporary `for(;;)` in the flipdev task: dev
  mode: DFU after 2.3 s; user mode: normal boot after ~1.5 s,
  `reset: watchdog`.  No false reset during a 700 KB base install, a
  650 KB SD read through a pipe and `pkg bind /bin/*`.
- ^G on the console beeps (speaker PB8, TIM16 CH1), e.g.
  `echo 7 | tr 7 '\007'` - to call someone to the device.

So a new image is flashed with `reboot -d` and dfu-util, and a broken one
comes back to DFU by itself; the buttons are needed only if both fail.

Then `make BOARD=flipper flash` (= `dfu-util -a 0 -s 0x08000000:leave -D
build/flipper/m0.bin`). Back to stock firmware: qFlipper → *Repair* from
DFU mode. Iterating on MINIX never needs Repair.

### SD card layout

One MBR partition (type 0x81, start 1 MiB, 4000 MiB) holding a MINIX V2
filesystem with 14-char names:

    sudo sfdisk /dev/sdX            # then type: label: dos
                                    #            start=2048, size=4000M, type=81
    sudo mkfs.minix -2 -n 14 /dev/sdX1

Plain `mkfs.minix -2` writes 30-char names (magic 0x2478), which MINIX 2.0
rejects; `-n 14` gives 0x2468.

Limits of MINIX 2.0 (include/sys/types.h), verified with a 4000 MiB image:
- `off_t`, `zone_t`, `block_t` are unsigned 32-bit, and `dv_base`/`dv_size`
  (kernel/driver.h) are 32-bit byte counts: the partition must be < 4 GiB and
  must **end** below 4 GiB on the card. 1 MiB + 4000 MiB fits.
- `ino_t` is 16-bit: at most 65535 files+directories (mkfs.minix caps it).
- Files are limited to 2 GiB (`s_max_size` written by mkfs.minix).
- The message field carrying the position (`POSITION` = `m2_l1`) is a
  *signed* long: drivers must treat it as unsigned or offsets past 2 GiB
  break. To fix in the SD driver/drvlib port, which will address in 512-byte
  sectors internally.

## Key design decisions

1. **No MMU → shadowing.** MINIX 2.0 still contains the `SHADOWING` code paths
   from the Atari ST port (kernel/proc.c, system.c, clock.c, mm/forkexit.c,
   mm/exec.c). After `fork()` the child shares the parent's address; the
   kernel swaps memory images on context switch. We reuse those paths and
   write the missing machine-dependent half (shadow copy/swap) for ARM.
2. **Relocatable executables.** User programs are Thumb-2 ELF linked with
   `--emit-relocs`; a host tool converts them to a compact MINIX a.out-style
   format with a relocation table. `exec()` relocates into wherever MM
   allocated memory. (Later optimisation: text of common commands XIP from
   flash, only data in RAM.)
3. **Kernel executes from flash (XIP).** All of RAM except kernel/MM/FS data
   and the FS buffer cache goes to user processes (~150 KB target).
4. **Traps.** `SVC` = MINIX `s_call` (send/receive/sendrec); PendSV does
   context switches; SysTick at HZ=60 drives clock task; NVIC replaces the
   8259. Tasks/servers run privileged on MSP-switched stacks, user processes
   unprivileged on PSP. MPU guards kernel memory (milestone 6).
5. **New CHIP/MACHINE.** `CHIP == ARM_CM`, `MACHINE == FLIPPER`,
   `_WORD_SIZE == 4`, `SHADOWING == 1`. x86-only drivers are compiled out.
6. **Root filesystem.** MINIX V2 FS (Linux `mkfs.minix -2` compatible) —
   first as a read-only image linked into flash (`/dev/ram`), later on a
   microSD partition.
7. **Two build targets** sharing all code except a small HAL:
   `flipper` (real hardware) and `qemu` (`qemu-system-arm -M mps2-an386`,
   Cortex-M4, CMSDK UART) so the kernel can be tested without a device.

## Milestones

| # | Deliverable | Done when |
|---|-------------|-----------|
| 0 | Toolchain, Makefile, startup, linker scripts, clock tree, USART1 + LCD "hello", buttons, QEMU target | Banner on serial + LCD |
| 1 | Kernel core on ARM: config, `arm/mpx.S`, `arm/klib.c`, proc/system/clock/table/tty, serial console | Kernel boots, IPC + clock + TTY + privilege checks pass |
| 2 | MINIX libc for ARM, exec format + host converter, real MM + FS, SPI SD-card driver, root on SD (QEMU: same image via semihosting), tiny flash rescue root | `init` runs `/bin/sh` from the SD card |
| 3 | Shadowing fork, core userland (sh, ls, cat, cp, ed/mined, ...) | Interactive shell, pipes |
| 4 | Sector-based disk addressing (fix 32-bit byte offsets), image builder for the SD card | Files persist across reboot |
| 5 | LCD tty (4×6 font, 32×10), d-pad on-screen keyboard, USB CDC-ACM tty, console auto-select | Usable without PC |
| 6 | MPU protection, RTC time, backlight/power-off, battery gauge, docs | Daily-usable build |

### Flipper hardware (goal: the original device's feature set)

Each peripheral becomes a MINIX driver task with device files, used by
ordinary user programs; a menu UI on the LCD then mirrors the stock
firmware's structure (Sub-GHz, RFID, NFC, Infrared, GPIO, iButton, USB,
Bluetooth, Settings).

| # | Deliverable | Device files |
|---|-------------|--------------|
| 7 | USB device stack: CDC-ACM console, later HID (keyboard) | `/dev/ttyusb`, `/dev/usbhid` |
| 8 | GPIO header pins, 5 V/3.3 V power, vibro, speaker (PWM), RGB LED + backlight (LP5562, I2C), buttons as input device | `/dev/gpio`, `/dev/vibro`, `/dev/speaker`, `/dev/led`, `/dev/input` |
| 9 | Infrared receive/transmit (timer capture / carrier PWM) | `/dev/ir` |
| 10 | Sub-GHz CC1101 on SPI1, with region-based transmit frequency limits as in the stock firmware | `/dev/subghz` |
| 11 | NFC (ST25R3916, SPI1), 125 kHz RFID, iButton (1-Wire) | `/dev/nfc`, `/dev/rfid`, `/dev/ibutton` |
| 12 | Bluetooth LE: start CPU2 with ST's wireless stack already in flash, IPCC mailbox + HCI/ACI transport | `/dev/ble` |
| 13 | Menu UI on the LCD mirroring the stock firmware's layout, apps per peripheral | — |

Status of #8 (2026-11-14): `/dev/vibro` (PA8), `/dev/speaker` (TIM16 on
PB8: notes of frequency + duration, played from the clock tick; ^G on the
console beeps too), `/dev/gpio` (the eight free header pins PA7 PA6 PA4
PB3 PB2 PC3 PC1 PC0 = pins 2-7, 15, 16; modes 0/1/in/up/down/off; PB6/PB7
stay the debug UART), commands `gpio` and `beep` (`beep -c` calls for
attention).  Minors 4-6 of major 6 (kernel/arm/flipdev.c).  Checked on
the device: GPIO levels with pull-up/down and as outputs; tone timing.
Not yet: the 5 V header supply (charger/OTG over I2C), a buttons
"input" device beyond /dev/buttons.

Minor 8, `/dev/power` (T15a): the BQ27220 fuel gauge on I2C1 (0x55),
read only, one text line per open (`soc= mv= ma= avg= sby= temp= flags=`),
read on demand in the task (7 two-byte reads, no static RAM); EIO on a bus
error, ENXIO without a gauge (QEMU).  Nothing is ever written to the gauge
(its data memory is flash; the stock firmware configured it).  The
`power` app shows it (docs/manual/power.md).

Notes: Bluetooth needs SRAM2 shared with CPU2 and the IPCC mailbox, so it
changes the memory plan: the 10 KB SRAM2a/2b areas currently hold the FS
buffer cache (fs/buf.h, ld/server.ld) and part of it must go to the radio. Sub-GHz
transmit must stay inside the legal bands for the user's region.

## Memory plan (agreed 2026-11-14)

1. **Tuning** (in progress): smaller FS/MM tables, stdio buffers, exec
   top-down / fork shadows bottom-up; kernel task and exception stacks
   sized from measured high-water marks (`free -k`, 9.7 KB → 5.2 KB);
   USB transmit ring 2048 → 512 B (keeps the newest output while no
   terminal is open).  Result on the device after login: 77 KB free,
   largest hole 57 KB.  chmem of every program checked with `free -t on`
   (MM paints the gap at exec and reports the untouched part at exit):
   small one-shot tools 4 KB, `sh` 12 KB (`echo /bin/*`: ~8 KB for the
   glob plus the exec frame), `ed` 10 KB, `tail` 8 KB.  Known limit: `ls -l /bin | sort |
   wc` still does not fit; point 2 is the cure (20 KB of it is code).
   Updating the system programs: `make BOARD=flipper base` and
   `tools/flsend build/flipper/minix/pkgs/base_<ver>.tar`.
2. **Program text from flash** (done, on the device 2026-11-14): user programs
   are position-independent (`-fpic -msingle-pic-base`, r9 = GOT = start
   of data; ld/user-pic.ld, A_SEP images from tools/elf2mnx.py).  MM keeps
   a text cache in internal flash (src/mm/fcache.c, 376 KB after the
   128 KB system image): the first exec copies the text there, later ones
   find it by CRC-32 + size and need RAM only for data and stack.  Full:
   records of programs nobody runs are dropped, oldest first; otherwise the
   text goes to RAM as before.  Kernel call SYS_FLASH (MM only, region
   only, MPU off and interrupts masked while the controller works).  QEMU
   after login: 106 KB free in one piece (was 73 KB, largest 52 KB); `ls -l
   /bin | sort | wc` fits now.  `free -v` shows the cache.
   Flipper: 105 KB free in one piece after boot (was 73 KB, largest 48 KB),
   the cache survives reboots.  STM32WB flash notes (hal/flipper/board.c):
   erase/program need HSI16 running; CFGBSY clears only after the error
   flags are cleared; FLASH_CR must not be written while busy (bus error);
   and ST's DFU loader jumps to the image without a reset, leaving its
   error flags set and CFGBSY on until a reset, so hal_early_boot() resets
   once in that case (minix.bin is also padded to whole double words, the
   programming unit).  A failing flash operation
   only turns the cache off (texts go to RAM) and logs the registers.
3. **Swap to the SD card** (partition 3): **postponed** (2026-11-14).
   Without an MMU a process can only come back to the addresses it left
   (its data holds absolute pointers), and whatever lies there by then
   must leave first - in the general case a deadlock, in the simple one
   (MINIX 2.0.4 style: swap out only processes blocked in wait/pause)
   the gain is a waiting shell, ~16 KB.  Fork shadows are short-lived
   (fork + exec frees them at once); long-lived ones (subshells) flip on
   every switch and could not afford the card.  With 105 KB free after
   boot it is not worth the risk now; revisit if RAM runs short.

## CPU-specific optimizations (work list, requested 2026-11-14)

Where they are measurably better than gcc -Os and cost no RAM, with the C
version kept as fallback.  Timings on the Flipper (host wall clock):

| | before | memops | + SPI |
|---|---|---|---|
| 10 x 512 KB from /dev/mem (with 10 fork+exec) | 1.27 s | 0.57 s | 0.53 s |
| 2 MB from the SD card | 4.98 s | 4.56 s | 2.72 s |
| `cat /bin/* \| wc -c` | 3.34 s | 3.08 s | 2.16 s |

Done:
- `memcpy`/`memmove`/`memset` in assembly (src/lib/arm/rts/memops.S):
  32-byte LDM/STM blocks, word loops for unaligned sources (the M4 allows
  unaligned LDR/STR on normal memory).  In libc for the programs and the
  servers, and in the kernel (phys_copy, shadow copies), which now links
  them from libc.a instead of the HAL's byte loops.  m2test checks every
  alignment, length 0..80 and overlap both ways.
- SD card / LCD SPI: block transfers keep the TX FIFO fed (two bytes in
  flight), SD data at 16 MHz (was 8), multi-block reads (CMD18).  CRCs
  are on (CMD59): CRC7 on commands, CRC16 on every data block, so a
  garbled transfer is refused by the card or noticed on reading; it is
  retried, and after repeated errors the bus drops to 8 MHz for good
  (the kernel logs "sd: N transfer(s) retried").

- Multi-block writes (CMD25): 512 KB written in 2.35 s (was 3.11 s).
- Hardware RNG (STM32WB TRNG, 48 MHz clock or LSI): `/dev/random`
  (minor 4) and `/dev/zero` (minor 5, was missing) in the memory driver;
  `passwd` takes its `$5$` salt from /dev/random.
- Shadow exchange (`flipclicks`) with LDM/STM (arm/flip.S).  The real
  cost was elsewhere: a ready shadowed process waited for the clock tick
  to be swapped in (16.7 ms per switch).  The idle task now raises
  PendSV, which wakes the clock task at once (a kick, not counted as a
  tick); idle checks and sleeps with interrupts masked so it cannot come
  back into `wfi` with a shadow waiting.  m2test's 200 round trips
  between a forked pair (400 switches): 6.66 s -> 0.25 s.

Dropped: the CRC peripheral for CRC-32 (pkg is limited by USB, ~100 KB/s,
where the table CRC costs ~1 % CPU; a kernel call per block would cost
more than it saves).

Still to do:
- SPI by DMA (the CPU could do other work during SD transfers).
- AES/PKA units if something needs them; CLZ / bit tricks where the old
  sources loop over bits.

## Userspace drivers (planned, not implemented)

Goal: drivers that are not needed all the time, such as Bluetooth, Sub-GHz,
NFC/RFID, IR, iButton, GPIO and extra USB classes, run as ordinary
processes. They are started when a device file is opened and exit when they
have been idle for a while, so their RAM is free the rest of the time.

**What it saves.** The kernel runs from flash, so a driver built into the
kernel costs RAM only for its data, its buffers and its share of the task
stacks. Its code costs no RAM. A user process's code is copied into RAM
today, so moving a driver out of the kernel *costs* RAM until memory plan
point 2 is done. After that, a stopped driver costs nothing and a running
one costs only its data and stack. That pays off for drivers with large
buffers or protocol state, such as the BLE transport, the NFC protocol
stack and IR capture buffers. Point 2 must come before userspace drivers.

**USB.** USB RAM today is about 2.5 KB: `tx_ring` 2048, `rx_ring` 256,
`udev`+`ubuf` about 230 B. It has no task stack of its own because it runs
inside TTY. The CDC-ACM console is how the device is logged into and how
packages are sent (`flsend`), so it is needed almost all the time. If its
driver crashed or were unloaded, the device could only be reached through the
LCD. Plan:
- The CDC console stays in the kernel. `tx_ring` is now 512 B (done,
  1.5 KB saved; flow control makes it safe). Optionally, power down the
  USB peripheral when VBUS is absent.
- Other USB functions, such as HID keyboard, mass storage (SD card as a USB
  drive) and later CDC-ECM, run as a userspace driver `usbd`. When it
  starts it takes the USB peripheral from the kernel; the console moves to
  the LCD/UART, and the kernel CDC comes back when `usbd` exits or dies.

**Design (light and deadlock-free):**
- A kernel task `udrv` routes requests to the drivers. A request for
  `/dev/X` is forwarded to driver X, and the caller is answered with
  SUSPEND. The driver answers later and `udrv` REVIVEs the caller. The kernel
  never waits on a user process (no blocking `sendrec` from a task to a user
  process), so a driver that hangs blocks only its own callers.
- `devd` (a userspace manager run as root) is started by `udrv` when a
  device file is opened and no driver is running. It runs the driver from
  `/etc/drivers` with its own uid. A driver that has been idle for N seconds
  exits.
- IRQs reach the driver as messages (`HARD_INT` notify). The kernel masks the
  IRQ until the driver acknowledges it. MMIO is reached through an extra MPU
  window over the peripheral's registers, limited to that driver. DMA is
  allowed only into the driver's own memory.
- When a driver dies, `udrv` fails every waiting request with EIO, releases
  the IRQ and MMIO window, and `devd` can restart it on the next open.
- Cost: one kernel task with a small table of drivers and requests
  (< 1 KB), plus `devd` (stack + data, about 2–3 KB once point 2 is done).

## Phase 1 measurements (spec rule 0.3, R0.8e), 2026-11-14

Static RAM (make BOARD=flipper ramcheck), bytes .data+.bss: kernel 17556,
MM 10684, FS 27856, INIT 1748 on main 76fd717; Phase 1 adds 0 B.

Latency baseline on the Flipper (make latcheck kernel, lat -w 60, launcher
in use by hand, background "cksum" of a 1.2 MB SD file in a loop):

| ID | Path | Measured | Budget |
|---|---|---|---|
| L1 | button -> next LCD frame | min 19 ms, max 967 ms (86 presses; pairs every press, also ones that change nothing on screen) | 30 ms p99 / 50 ms |
| L2 | button -> /dev/buttons answered | 7-8 us | 2 ms p99 |
| L4 | interrupts masked, normal work (SD, ls -lR, cp 1.2 MB) | 0-2 us | 20 us |
| L4 | interrupts masked, flash text cache write (first run of a program) | 40 ms (also ~44 ms at boot: cache directory) | not allowed (R0.8c) |
| L5 | REVIVE -> process runs | avg 47 us, max 905 us (171) | 100 us |
| L6 | exec -> its first frame | 2.3 ms .. 1.26 s (first runs write the flash cache) | 300 ms |
| L7 | app exit -> menu redrawn | 3.0-6.8 ms | 150 ms |
| L10 | clock start -> launcher's first frame | 300 ms (plus the boot before the clock) | 3 s |

After T2 (flash writes as idle work, 2026-11-14): L4 2 us max also while
the cache is written (was 40 ms); L6 6-35 ms over 8 launches of "info",
5 of them first runs from RAM (was up to 1.26 s); L9 one page erase
21.95 ms, programming 256 B per step 2.75 ms, at most one erase per
~270 ms; a 220-character line pasted during the copy arrived complete.

Flash writes at run time (owner rule, 2026-11-14): a program is copied
to the text cache only after its third run from RAM (MM counts runs of up
to 8 uncached programs, nothing written for that), from its file, which
exec leaves open for the copy; the directory is written only for a new
record, a compaction when it is full, or a format when there is none.  So
a day of normal use writes 0 pages once the programs in use are cached;
each newly installed program costs its own pages once (e.g. sum: 1 page
erase + 1 KB programmed + one 32-byte record) after its third run, and
nothing if it is never run three times.  Exception: a first run that has
no RAM for its text is cached at once (fc_sync), the old way.
T3 (2026-11-14, by hand on the Flipper, 238 presses in the launcher,
About and Console, background cksum of a 1.2 MB SD file; T2 kernel): keys
now come on press (gui.c).  L1 median 136 ms -> 5 ms, min 52 ms ->
0.17 ms; the tail (p99 0.75 s, max 0.92 s) is OK presses that start an
app (the next frame is the app's, after fork + exec) and first runs that
had to be cached synchronously for lack of RAM (L6 max 2 s).  L2 max
2 ms, L5 avg 46 us max 2 ms, L7 median 3.6 ms.

Phase 2b part 2 (2026-11-15, Flipper, normal kernel): `pkg flash
/bin/ed` (14 KB file, 4 pages) takes about 1 s; MM is busy for that time
(other MM calls such as fork and exec wait), while interrupts, the clock
and the GUI's /dev reads keep running (flash writes no longer mask
interrupts).  exec of an installed program writes nothing and copies no
text.  The flash is written only by pkg: per program and version, its
pages once plus one 64-byte record; another 8 bytes when the version is
replaced or removed; one directory page erase per 63 installs.  Static
RAM: 0 B (MM 10172).

R2b.6 (same day): `make BOARD=flipper base-flash` writes
build/flipper/minix/minix-base.bin, minix.bin plus the programs of
BASE_FLASH (sh, launcher, getty, login, ls, cat, cp, pkg, update, mount,
printroot, test: 128 KB) in the installed region, with records that are
not bound yet.  Flash it with DFU instead of minix.bin; /etc/rc runs
`pkg bind /bin/*`, and the first boot afterwards binds each record to
the file with the same SHA-256 text (on the Flipper: 12 texts, 18 files
with the aliases [, clone, cpdir, ln, mv, rm).  Later boots: one MEMINFO
call, nothing to do.  A base upgrade keeps them in flash: an unchanged
text gets only a new record, no page is written.  Older cards need the
`pkg bind /bin/*` line in /etc/rc added by hand (the base package does not
carry /etc).  Now the base package's postinst adds it (rootfs/base.postinst).

Phase 2b follow-ups (2026-11-15, Flipper): the first `pkg bind /bin/*`
after flashing minix-base.bin (18 files, 12 texts, the directory moved
once) takes 1.86 s; with nothing left to bind it is one MEMINFO call,
59 ms for the whole command.  `free -k` now also shows the servers'
stack high-water marks (the kernel marks their stacks at load): MM 428
of 1024 bytes after boot and login, 588 after the bind with its
compaction; FS 516 of 2048.

Installed-region compaction (2026-11-15, Flipper): after a few base
upgrades, `pkg flash` of any program said "No space left on device"
with 188 KB free.  The directory (63 records) was full, and compaction
freed nothing.  It kept every dead record whose pages a process still
ran from.  But an upgrade that leaves a text unchanged gives it a new
live record at the same pages, so every upgrade left a dead duplicate
of each running program (sh, getty, login, launcher, update...), kept
for good.  Now compaction drops a dead record when another record
(live, or an earlier dead one) has the same text address: the first
install after that worked again.  RAM 0 B; flash: one compaction (two
directory pages) as before.

AC2b.R1/R2 (2026-11-15, Flipper, latcheck kernel):
- R1, programs from the installed region: the launcher's L6 (exec to
  its first frame) is 9.7 ms.  `ls /` and `sh -c :` run 50 times in
  at most 1-2 s, so at most 40 ms per run, fork included.
- R2, before: `pkg flash` was one MM call per program (erase and program
  the whole text, ~0.4-1 s), and every fork, exec and wait queued
  behind it.  40 execs took 3 s during a 49 KB install and 1 s
  without it.
- R2, now: an install that writes runs as MM's flash job, one step per
  call (`MI_INSTALL` op 4: a page erase or 256 bytes, each compared with
  the file as it is programmed).  `pkg` calls again until the job is
  done, and MM serves everybody else in between.  The record is written
  last, after a check that the file's ctime has not changed and a
  SHA-256 of the flash copy.  During a 105 KB install (14 programs,
  3 s) the launcher's L6 was at most 10.9 ms (4 samples), and L9 at
  most 21.95 ms (one page erase).
- RAM: 0 B.  The install borrows the cache's job (struct mmjob); a cache
  copy in progress is dropped and redone on a later run.  An install
  whose `pkg` died is dropped by the next install or cache copy.
- Flash writes: unchanged (the same pages and record per program).
- L1 during an install still needs the owner's hands (buttons).

AC2b.5, power cuts during installs (2026-11-15, QEMU only, no Flipper
flash written): `tools/powercut.py`.  The QEMU board's flash now behaves
like the STM32WB's: a double word can be programmed only when erased
(or with zeros), otherwise PROGERR.  The test cuts the power at the Nth
operation on the region, either before it or half done (half a page
erased, one word of a double word written).  Then it resets the
machine; the "flash" survives the reset, like the real one.  After the
reboot the test checks four things: /tmp/p runs the text of the file it
now is (od or cksum); it installs again; a new program installs too;
and nothing printed "not erased", "damaged" or a panic.

| Scenario | main | now |
|---|---|---|
| install od, then upgrade to cksum: every erase, the ends of every text, every directory write (52 runs) | 16 failed | 0 |
| the same, every operation (`--all`, 2624 runs) | | 0 (6 logins timed out with 8 QEMUs at once; all 6 passed again) |
| directory compaction, every operation (`--compact`, 22 runs) | 9 lost the installed record | 0 |

- Safety held everywhere: no run ever executed a half-written or
  wrong text.  The text is written first, the record after it.
- Bug 1: a cut inside a directory record, before its address was
  written, left a slot that looked free but was not erased.  Every
  later install programmed over it and failed (PROGERR on the
  Flipper), for good.  Now only a slot erased throughout is free, and
  a record counts only when its address and seq are written.  A torn
  record is skipped, the seq count ignores it, and the next compaction
  drops it.
- Bug 2: compaction wrote the new directory's header first, then
  copied the records.  A cut in between made a directory that was
  valid but short, and the installs it had not copied yet were lost
  (their programs fell back to RAM).  Now the header is written last,
  so until then the old directory is the valid one.
- The run-time cache's records were already safe: one is found only
  when its last double word (the owner) matches.
- Also: the installed region is capped at 256 pages (the job's page
  index is a byte; the WB55 has about 160).
- RAM 0 B.  Flash writes: unchanged (the header is one double word
  written later, not an extra one).

R2b.11, the run-time cache erase budget (2026-11-15): the kernel counts
every flash erase (`hal_fc_erase_ok`, before the erase).  An erase of
the run-time cache is refused (ENOSPC) once today's budget is spent
(default 32 pages: one cycle a day of the 128 KB cache).  MM then drops
the copy, and the program keeps running with its text in RAM.  The
counts live in RTC backup registers (table above): 0 B of RAM, and a
reboot loop cannot reset today's count.  `pkg budget [pages]` shows or
sets the budget (root; 0 turns run-time caching off).  `/etc/rc` sets it
from `fcache_budget=` in `/etc/rc.conf` (sh variables, optional).
`free -v` prints `flash: cache N pages erased (M today, budget B),
installed K`.  This MINIX has no `svrctl`, so the setting goes through
MEMINFO (`MI_FCBUDGET`) like the other MM controls.
QEMU, AC2b.9: five uncached programs, run ten times each with pauses.
With `pkg budget 2`: 2 pages erased (the directory and one program),
the rest ran from RAM.  With the default: 6 pages, 4 programs.  That the
count survives a reboot is checked on the Flipper (QEMU has no backup
registers).

T12 / L.2, the default settings in ROM (2026-11-15, QEMU): `make
base-flash` builds `rom.img` (a read-only MINIX file system, 32 KB, from
`rootfs/rom.proto`: `/etc` as built, plus the default `rc.conf`, which is
not put on the card) and puts it into `minix-base.bin` at a fixed place:
right after the installed region's two directory pages (0x08032000 on
the Flipper).  It is written only by DFU:
- installs start after it;
- the kernel refuses MM's erases and writes there (EROFS);
- `/dev/rom` (memory driver, block 1/6) serves it and cannot be opened
  or mounted for writing.

`/etc/rc` mounts it read-only on `/rom`, and reads `rc.conf` from the
card first, else from `/rom`.  Without the image (a plain `minix.bin`)
the mount fails quietly.  The base package's postinst adds `/rom`,
`/dev/rom` and the mount line on older cards.

RAM: kernel -8 B.  `/dev/random` and `/dev/zero` need no geometry entry
(they have no end), and `/dev/rom` takes one of their two slots.  The
random generator's 64-byte buffer moved into a function of its own:
mounting `/dev/rom` is the first block-device use of the MEMORY task
(do_vrdwt + m_schedule), and with the buffer in `m_schedule` its
256-byte stack overflowed (panic in QEMU).  On a card the image takes
one of the 8 super blocks and one mount.  Flash: 32 KB of the installed
region, written at DFU time only.

T12 / L.3-L.4, settings read with a default (2026-11-15, QEMU):
- `cfopen(path, mode)` in libc (`<stdio.h>`, `_MINIX`).  Reading opens
  the card's file, else `/rom` + path.  Writing always goes to the card,
  making the directories; `a`/`+` start from a copy of the default.
  `cfcat path...` does the same for scripts.
- The launcher reads `/etc/apps` with it, and `/etc/rc` prints the motd
  with `cfcat`.  `rc.conf` is not on the card at all (ACL.1); `cfcat
  /etc/rc.conf` prints the default.
- `settings diff` lists the files under `/etc` that differ from
  `/rom/etc`.  `settings reset [path...]` brings defaults back: the
  card's copy is removed, except for files read directly at boot or
  login (rc, ttytab, profile, fstab, setup, passwd, group), which get the
  default copied over them.  A plain `reset` leaves passwd and group
  alone.
- QEMU: motd, rc.conf and rc changed, then `settings diff` showed the
  three; `settings reset` removed motd and rc.conf, copied rc back, and
  left passwd alone until it was named.  The flash counters stayed at 0
  (ACL.2; the launcher's Settings menu waits for the Settings app).
- L.5 is not done: the card image still carries its `/etc`.  With
  `/rom` missing (a board flashed with plain `minix.bin`) a card without
  `/etc/apps` would show no apps, so taking the files off the card
  waits until `/rom` is always there.

Minimal I.1, the wall clock from the RTC (2026-11-15, #58): at boot the
clock task sets `boot_time` from the RTC (`hal_rtc_time()`).  The STM32's
BCD calendar is read once (years 2000-2099, UTC); it keeps running on the
battery with the stock firmware's settings.  QEMU takes the host's time
(semihosting).  Without a running RTC the result is 0, so the clock
starts at 1970 as before, and every boot would repeat the same ctimes.
An installed program's binding is (dev, ino, ctime), so a new file could
then get an old program's binding.  MM therefore makes no new bindings
while the clock is before the build's commit (`GZ_BUILD_EPOCH`):
`pkg flash`/`install --flash`/`bind` say "the clock is not set (RTC)",
before any page is written, and the program runs from RAM.  QEMU: `date`
shows the host's time and installs work; after `date 0101700000` an
install is refused and the region is unchanged.  The full model (time()
from the RTC, the century register) is still Phase 2d.  RAM 0 B.

R2b.12, rebinding by content, and the exec guard (2026-11-15, QEMU; the
#58 decision):
- `pkg bind` now goes through every file it is given (there was a shortcut
  when no built-in record was unbound).  A file whose binding matches
  costs one MM call, no hash; `pkg bind /bin/*` in QEMU still finishes
  within the same second.  A file copied back with a new inode is
  hashed, gets a new record for the text already in flash, and no page
  is written.
- The old record becomes stale.  FS answers a new MM-only call,
  `INOCHK` (dev, ino, ctime): live, gone, or unknown when the device is
  not mounted.  It only looks at the inode: a free inode is let go
  without `put_inode`, which would free it again on the disk.
  Compaction drops a live record only on a definite "gone", and only if
  nothing runs from its text or a current record holds the same text.
  No extra flash writes: the record is simply not copied.
- QEMU, `ls`, `cat`, `cp` copied back under new inodes, 145/147/148:
  - `pkg bind` → 21 live records (3 stale), no text page written;
  - `ls` ran from flash;
  - after one compaction → 19 records (18 + one test file).
  When the inode was reused (same number, new ctime), the old record
  was already killed by `in_old`.
- Exec guard: the record's free `flags` word (the double word of `seq`,
  so no extra write) holds the CRC-32 of the text's first 256 bytes,
  taken from flash by `in_record` and by `mkbaseflash.py`.  Exec checks
  it against the file, 4 x 64 bytes through the FS cache (the block exec
  already read), and only then trusts the binding.  Records without it
  (flags all ones) work as before.
- QEMU, a ctime repeat forced with `date`: `fold` installed as /tmp/x
  (inode 143, 2027-01-01 00:00), removed, `chown` (same text size,
  5896 B) copied to /tmp/x, again inode 143 at 00:00.  It printed
  chown's usage: the guard refused fold's text.
- powercut `--compact` now checks a file that is never changed again
  (/tmp/k).  /tmp/p's records may go: each install follows a `cp` (new
  ctime), so the old record is stale.  Now 0 of 36; with compaction's
  old header-first order it still catches 13 of 22.  RAM 0 B (kernel,
  MM, FS).

T4b step 1, the SD card's busy wait (2026-11-15, Flipper): after a write
the card programs for up to ~9 ms, and `sd_wait_ready` polled it all
that time in the disk task: the last cause of infrared underruns (R0.8b
above).  Now every 64 polled bytes (~40 us) the card is deselected (it
goes on programming; SD spec, SPI mode), the other kernel tasks run
(`hal_disk_yield` → `task_yield`), and the speed is set again before the
card is selected (the LCD shares SPI2).
- Infrared, 1000 durations of 100 us, while `cat /bin/* > /home/root/b2;
  sync` ran in a loop: 0 of 80 frames failed (before: 13 of 130 with
  reads alone).
- 671,643 bytes written to `/home` and read back with the same
  checksum, 9 times.
- A 650 KB write plus `sync` takes about 5 s.
- What is left: user processes still wait while the disk task polls a
  transfer or yields to other tasks only.  L5 during the writes: max
  2.2 s, 1.8 s of it in tasks, the disk task's longest slice 16.7 ms.
  Step 2: SPI2 DMA with the task blocked on the transfer-complete
  interrupt, and the busy wait on the clock.

T4b step 2, SD data by DMA (2026-11-15, Flipper):
- Each 512-byte data block of a read or write goes by DMA1, channel 1 =
  SPI2 RX and channel 2 = SPI2 TX (DMAMUX requests 8 and 9, from ST's
  `stm32wbxx_ll_dmamux.h`).  Bytes not wanted come from and go to the
  HAL function's own stack.
- The disk task sleeps meanwhile (`hal_disk_wait` = `receive(HARDWARE)`),
  so user processes get the CPU.  The interrupt only switches itself
  off; the HAL then checks the completion flag in the hardware, so a
  stale wakeup just ends in polling.
- The LCD shares SPI2: `hal_lcd_flush` returns -1 while the card is
  selected (the SD chip-select pin is the flag, 0 B), and flipdev
  draws that frame at the next tick (`lc_dirty`).
- First tested on the unused swap partition only (DMA for its sectors
  alone): 1.3 MB written and read back identical, twice.  Then on the
  whole card: `/bin/*` checksum unchanged, 650 KB written to `/home` and
  equal.
- 4 MB read: 5 s by DMA, 6 s polled.  A CPU-bound `cksum` (2 s alone)
  next to a 4 MB card read: 15 s with DMA, 24 s polled.
- Infrared at 100 us durations during writes and syncs: 0 of 40 frames
  failed.  L5 during them: max 77 ms, average 21 ms; with step 1 alone,
  2.2 s and 48 ms.
- RAM 0 B.

Backlight idle timeout (owner request 2026-11-15): without a key or
button (USB console keys count too) the LCD backlight dims by 10 % a
second in the last 5 s of the idle timeout, then fades out; it comes
back at the next key.  Default 15 s, so dimming from 10 s on.
- The clock tick only compares `k_input` with the timeout and wakes the
  device task on a change; the task does the LP5562's I2C (`bl_apply`).
- Brightness, timeout and state live in RTC backup register 15, so
  they are kept over resets: 0 B of RAM.
- `backlight [LEVEL [SECONDS]]` shows or sets them through
  `/dev/backlight`: a 3-byte write sets the timeout, and a read gives
  the state.  A settings app can use the same device later.
- Off softly: the LP5562's engine 1 fades the white channel from the
  current level to 0 in about 0.5 s on its own (no CPU, no I2C while it
  runs; the program and register sequence of the stock firmware's
  `lp5562_execute_ramp`).  On at once: `hal_backlight` gives the channel
  back to direct control, stops the engine and sets the level.  Owner,
  on the Flipper: "naprawdę super".
- With the light off, a press only turns it on (owner): neither that
  press nor anything else reaches the apps until every button is up.
  The flag is the top bit of flipdev's `last_buttons`, so no RAM.
- Flipper: `backlight 255 10; sleep 15; backlight` → "now off (idle)";
  `sleep 1; backlight` after a key → "now on".

Phase 2c, vfork (2026-11-15, QEMU and Flipper):
- MM's `VFORK` (call 57): the child runs in the parent's memory and on
  its stack, and the parent waits in MM until the child's exec or exit
  (`vfork_done`).  There is no shadow buffer and no image copy.
  - The kernel's `mkshadow` copies nothing for it (c2 = 0).
  - At the child's exec or exit, MM keeps the region `rmshadow` hands
    back: it is the parent's.
- With shadowing, vfork happens only when no other process shares the
  parent's addresses (a background subshell); otherwise it is a plain
  fork, because the group's shadow swaps would move the image from
  under the child.
- Signals to the waiting parent are delivered when it goes on.  A fork
  or vfork from the child fails with EAGAIN.
- libc `vfork` is assembly: the message lives below the stack pointer
  across the trap only, and the return address stays in `lr`.  The
  error path tail-calls `__vfork_err`.
- Users (R2c.2):
  - `sh`, for simple external commands only: no assignments, no
    here-documents, no substitutions in redirections.  The globals the
    child changes are put back, and every error path in the child ends
    in `_exit`.
  - `init` for `/etc/rc`.  Its getty start stays a fork, because that
    child forks again.
  - the launcher, with `getgrnam` moved before the call;
  - `pkg`'s `run()` and its `tar tf`.
  - `getty` does not fork.
  - Man page `vfork(2)` lists what the child may do (R2c.3).
- Tests:
  - m2test: vfork + exec + wait, vfork + _exit + wait, 20 vforks in a
    row (AC2c.3).
  - QEMU, the same load of background shells: on main `free` and `cat`
    failed with "not found" (no room for the shadow copy); with vfork
    they ran with 16 KB free.
  - Flipper: `ls -l /bin | sort | wc` (AC2c.1) works.
  - 50 `ls /` take about 1 s with no image swaps; each fork + exec
    used to cost two shadow copies of the shell (~0.4 ms each for
    17 KB, AC2c.R).
- RAM 0 B.

T14a, power fixes at boot (2026-11-16, Flipper, #103):
- SMPS step-down for the core instead of the LDO.  Its clock is set as
  the stock firmware does: HSI16 (kept on), SMPSDIV 01.
- The radios are parked in `hal_init` over SPI1, which is switched off
  again afterwards with its pins analog:
  - CC1101: SRES, then SPWD with CS high;
  - ST25R3916: SET_DEFAULT, then op-control 0.
- DBGMCU's Stop/Standby debug clocks are cleared unless in dev mode.
- Dev-mode boot line on the Flipper:
  `power: smps on; cc1101 asleep (122 us to wake); st25r3916 op 0 id
  2a (off)`.
  - A sleeping CC1101's crystal has to start once CS goes low, so it
    takes 122 us before CHIP_RDYn; reading MARCSTATE would wake it, so
    this is the check.  It goes back to sleep afterwards.
  - Chip id 0x2a = type 5, the ST25R3916.
- Regression: the SD card, IR send, the backlight and the USB console
  work; `lat` L4 0 us.
- Idle current: not measured, no meter at hand.  Expected about -1.7 mA
  for the CC1101 and roughly half the CPU current from the SMPS
  (power-plan.md 1.1).
- RAM 0 B.

T9 (2026-11-15): pipe data did reach the SD card.  Pipes are inodes on
the root device with their data in ordinary cache blocks; counted in
QEMU (every SD write logged), 10 times `cat /etc/rc /etc/profile
/etc/passwd`, then `sync`:

| | to /dev/null | through `\| wc` |
|---|---|---|
| before | 7 writes (atime of the inodes) | 76 writes: the pipe's data blocks, its inode, the bitmaps |
| after | 7 | 9 (the two bitmap blocks once) |

Why: (1) evicting any dirty block calls flushall(), which writes every
dirty block of the device, unread pipe data included; (2) a pipe's
blocks stayed dirty after they were read, and after the pipe was closed
and its zones freed; (3) the pipe's inode was written at creation and
at close.  Now: (1) get_block evicts the oldest clean block and a dirty
one only when every free buffer is dirty; (2) forget_zone() marks a
zone's cached blocks clean when a pipe has been read empty and when any
zone is freed (also saves the writes of a file deleted before the
next sync); (3) an unnamed pipe's inode is never written.  Pipe data
still goes to the card if it is evicted unread with every buffer dirty,
or if `update` syncs while a pipe holds data.  RAM: 0 B.
T4 (2026-11-15, Flipper, latcheck kernel; lat now splits L5 into "to FS
reply" and "reply -> run", and says who used the time in between):

| Load | L5 before | L5 after |
|---|---|---|
| cksum of a 1.2 MB file in a loop, typing on the console | avg 37 us, max 37 us (27) | same |
| launcher by hand (126 presses), cksum loop | avg 87 us, max 3.1 ms (248); the 3 ms all "reply -> run" | (needs the owner's hands again) |
| copies of 8 programs run 3 times each (the cache copies them in the background), typing | max 22 ms; all 4 slow ones with a flash operation in between | flash: 0 of the slow ones; max 47 ms, of which 43 ms in kernel tasks, longest slice 12.5 ms in the SD task (WINCHESTER) |

- The flash stalls: during an erase (22 ms) or a double word (85 us)
  the CPU waits for the flash, handlers included.  The cache's
  background steps now ask first (`FL_BG`): the system task answers
  EAGAIN while a user process is ready or a key or button came in the
  last second, and the step is tried a tick later.  Background writes
  go one double word per call, so a waiting task gets the CPU after at
  most ~85 us.  A page erase still cannot be split: it only starts when
  nothing waits and nothing was typed for a second.
- What remains is the SD driver: it polls SPI and the data token in the
  disk task, and a kernel task keeps the CPU until it blocks - a
  multi-block read for an exec held it for 12.5 ms.  Splitting the
  transfer does not help while the task polls.  Fix (T4b, proposed):
  SPI2 RX/TX by DMA with the transfer-complete interrupt, the token
  wait on the clock, the task blocked in between.
- Image swaps (unshadow, clock task) take up to 0.41 ms for 17 KB; no
  revived process was shadowed in these runs.

Other kernel tasks behind the SD task (R0.8b, 2026-11-15): tasks never
preempt tasks, so a woken task (flipdev refilling the infrared ring,
answering a button) waited for a whole SD request or scatter.  Test: an
infrared frame of 1000 durations sent in a loop during
`cat /bin/* | cksum` (652 KB from the card).  The ring holds 64
durations and asked for a refill when half empty.

| Durations | main | now |
|---|---|---|
| 600 us (refill margin 19 ms, now 29 ms) | 1 of 5 frames failed (EIO, ring underrun) | 0 of 74 |
| 100 us (margin 3.2 ms, now 4.8 ms), a stress test | 15 of 15 failed | 13 of 130 failed |

- The disk task calls `task_yield()` after every 1 KB piece, with the
  card deselected (the LCD shares SPI2 and sets its own speed): PendSV
  saves its context and puts it behind the other ready tasks.
- flipdev asks for a refill when a quarter of the ring is gone, not
  half: the task may come 48 durations late.
- `do_vrdwt` works on FS's vector one request at a time instead of a
  static copy, which another driver task could overwrite during the
  yield: kernel -288 B.
- What still fails at 100 us, measured: a 1 KB read takes at most
  1.34 ms, but a 1 KB write up to 8.9 ms, almost all of it
  `sd_wait_ready` polling while the card programs (inodes written by
  `sync` during the reads).  Every remaining underrun had such a write.
  That busy-wait belongs to T4b: release the card's select while it is
  busy and wait on the clock, the task blocked.
- User processes still wait for the polled transfer (T4b).

To redo: make BOARD=flipper latcheck, flash build/flipper-lat/minix/minix.bin
and install its base package, mknod /dev/lat c 6 15, then lat -c and
lat -w SECONDS while testing.  lat prints min/median/p99/max.

## Status

- **Milestone 0: done in QEMU, built for Flipper (hardware test pending).**
  `make run` boots the QEMU target, prints the banner, checks SysTick, and
  renders the LCD framebuffer as text (fbcon scroll/wrap test).
  `make BOARD=flipper` gives `build/flipper/m0.bin` (~4.6 KB). On the device it
  should print the banner on USART1 (115200 8N1, pins 13 TX / 14 RX, 3.3 V),
  show text on the LCD, echo button presses and serial input; hold BACK 2 s to
  halt.
- **Milestone 1: done** (kernel core; its test stand-ins were replaced by
  the real servers in milestone 2).
- **Milestone 2: done in QEMU, built for Flipper (hardware test pending).**
  Real MM + FS, root file system on the SD card (QEMU: disk image via
  semihosting), exec of relocated programs.  `make INIT=m2 minix minix-run`
  runs the self-test (file I/O, argv/environ, malloc, signals, fork, pipes,
  exec, MPU isolation) and exits QEMU with its status.
- **Milestone 3: largely done in QEMU.**  Real `init` (src/tools/init.c),
  getty, login, `/bin/sh` and ~80 commands; `/etc/rc`, ttytab, passwd.
  Memory (QEMU and Flipper alike): kernel 18 KB, MM 11 KB, FS 13 KB in
  SRAM1 plus the buffer cache in SRAM2; INIT 8 KB; **135 KB for user
  processes**.  Limits: several concurrently forked shells each hold a
  ~15 KB shadow copy, so long pipelines with loop subshells can run out of
  memory ("not found").  Ideas: run program text in place from flash,
  swapping to the SD card.
- **Memory protection (from milestone 6) done:** the MPU confines each user
  process to its own memory (`mpu_switch()`); flash is read-only.  MM and FS
  run privileged.

### Building the SD card

    make -C flipper BOARD=flipper card CARD_BLOCKS=<partition size in KB>
    sudo dd if=flipper/build/flipper/minix/card.img of=/dev/sdX1 bs=1M conv=fsync

`card.img` is a file system as large as the whole partition, trimmed to the
part in use (~5 MB): unused zones are free in the bitmaps, so what is on the
card there does not matter.  Partition size: `blockdev --getsize64
/dev/sdX1` / 1024; the default is 3871232 (the 4000 MiB partition).  `make
disk` builds the small 8 MB `fs.img` and the QEMU `disk.img`.

### User programs

Linked at 0 with relocations (`ld/user.ld`, `-Wl,-q`), converted by
`tools/elf2mnx.py` to MINIX a.out (cpu `A_ARMCM`) with a GEMDOS-style
relocation table, which exec relocates in MM (the Atari ST code path).
Stack + heap per program like `chmem`: 16 KB default, `CHMEM_<prog>` in
`minix.mk`.

### How the ARM kernel works

- All processes run in Thread mode on the PSP; tasks privileged, MM/FS/users
  unprivileged (`p_reg.control`).  The kernel runs in Handler mode on its own
  MSP stack (`arm/mpx.S`).
- SysTick and device IRQs share one priority, SVC is one level above, so
  kernel entries never nest (`k_reenter` is only ever -1 or 0).
- On entry the whole register set moves into `proc_ptr->p_reg`; `restart`
  rebuilds the hardware frame on the process stack.  `sigreturn` cannot
  change `control` or `psw`.
- The first process is entered with `svc` from `main()`; BASEPRI keeps
  interrupts off until then.
- MM, FS and INIT are separate images in flash after the kernel
  (`ld/server.ld`, header `src/lib/arm/rts/bootimg.S`), linked one after the
  other in flash and RAM; `cstart()` copies their data, `arm_proc_layout()`
  sets their maps (virtual == physical).

### Bugs found and fixed in the original sources

- `include/minix/config.h`: `#if (ATARI_TYPE == TT)` is true on every
  non-Atari machine (both undefined = 0) and silently forced `SHADOWING` to 0.
- `include/minix/jmp_buf.h`: 68000 `JB_D6` offset was 20, should be 30.
- `include/stdarg.h`: gcc >= 2 branch used `__builtin_next_arg` pointer
  arithmetic, wrong whenever arguments are passed in registers (ARM, and any
  modern gcc); now uses the `__builtin_va_*` builtins.
- `include/stdio.h`: `v*printf`/`v*scanf` declared with `char *` instead
  of `va_list` (breaks with gcc's register-based va_list).
- `src/lib/posix/_exec.c`, `src/lib/other/execlp.c`: `execl()`, `execle()`,
  `execlp()` passed their `va_list` on as the argv array; rewritten to copy
  the arguments.
- `src/mm/exec.c`: the shadowing exec path used an undeclared `rp`.
- `src/kernel/clock.c`: with shadowing, a runnable shadowed process could
  wait forever on an idle system (the clock task was only woken at the end
  of a quantum).
- Boot servers had no stack segment, so MM did no checking and INIT's heap
  could grow into its stack (`arm_proc_layout()` now gives them one).
- `src/commands/sh/sh.h`: `iofn` declared as taking no arguments but called
  with two.
- `src/commands/simple/cp.c`: a 32 KB buffer on the stack (fine on a PC);
  ed's 8 KB line buffers and sort's 20 KB work area likewise sized down
  for ARM.
- `src/commands/simple/mkfs.c`: on 32-bit machines the "already
  initialised" bit map (128 KB of bss) was indexed past its end for file
  systems over 1 GB, and the block limit was 1 GB; now 16 K tracked blocks
  and < 4 GB.
- `init` sends `/etc/rc` output to `/dev/log`, which was missing from the
  root file system (rc output vanished).
- `src/kernel/tty.c`: LNEXT (^V) and REPRINT (^R) were also processed in
  raw mode (IEXTEN only), eating those bytes from binary transfers.
- Console flow control end to end (USB endpoint NAK, console ring, raw TTY
  input kept, UART interrupt held); the CR inserted before every LF in the
  console driver corrupted raw output and was moved to kernel printf only.
- Memory tuning for 16 processes: FS 48 filps / 40 inodes / 4 supers,
  MM 32 holes, stdio BUFSIZ 512 (Flipper after login: 72 KB free, largest
  hole 51 KB; was 27 KB).
- Memory without an MMU: exec now places program images at the top of
  RAM (MM `alloc_mem_high`), fork shadow copies at the bottom; long-lived
  processes pile up at the top and the free space stays in one piece
  (largest hole after login 29 KB -> 48 KB).  `free -v` shows the holes.
- `src/fs/read.c`: block devices were given a "size" of `LONG_MAX`, so every
  read past 2 GB returned end-of-file; now `ULONG_MAX`.
- The disk driver no longer uses 32-bit byte offsets for the sector number
  (`arm/disk.c`); `dv_base`/`dv_size` are still 32-bit byte counts, so
  partitions must stay below 4 GB.
- `sed` needed 48 KB of static tables (more than the largest free block,
  so it never started); on ARM 1 KB lines, 100 commands, 3 KB pool.
- `tail` on a pipe mallocked `LINE_MAX * 10` = 20 KB and always failed;
  on ARM a 4 KB ring.
- `uname()` left `machine` and `arch` unset on non-Intel chips (garbage
  from the stack); now "flipper" / "armv7e-m".
- `tr` only knew System V ranges `[a-z]`; POSIX `a-z` now works too.
- Known: `sh` crashes (signal 11) instead of just failing when it runs
  out of heap ("out of string space"), e.g. on a glob too big for it.
- **Security:** `getty` ran `/usr/bin/login` (ours is `/bin/login`) and,
  when that failed, a root shell without a password, for any name typed
  at the `login:` prompt.  Now it tries `/bin/login` and `/usr/bin/login`
  and otherwise gives up (init starts a new getty).
- **Security:** `passwd` (set-uid root) copied passwords of up to 32
  characters (`getpass`) into 9-byte buffers.
- Passwords could not be checked at all: crypt(3) needs the helper
  `pwdauth`, which was not installed (now `/lib/pwdauth`, on the root file
  system, set-uid root; `passwd` and `su` are set-uid root too, in the
  image and in the `base` package).
- Modernised: password hashes are SHA-256-crypt (`$5$`, 5000 rounds, 16
  character salt), the glibc format, so `openssl passwd -5` makes the same
  hashes; old DES hashes are still accepted.  About 1 s per check on the
  Flipper (QEMU takes minutes: it emulates the small MPU regions slowly).
- `pkg` failed with "cannot make /usr/tmp/pkg.*" when a run that had died
  left its work directory and a later boot reused the pid; it now
  removes the stale directory.
- `find` and `du` could not read any directory ("can't read directory"):
  opendir() mallocs a 1 KB DIR and the 6 KB chmem left too little gap;
  `cpdir`, `chmod -R` had about 1.5 KB to spare.  chmem raised (minix.mk).
- `tr` knew only octal escapes: `tr '\n' ' '` replaced the letter n.  The
  POSIX escapes (\n \t \r \b \f \v \a) are handled now.
- tty: a line that filled the 256-byte input queue in canonical mode lost
  its newline as well and could never be read; the console hung (only
  kill, ^U, got out - interrupt is DEL in MINIX, not ^C).  The last place
  is now kept for the line break: too long lines are cut short.
- `cksum /bin/*` (and any exec after expanding /bin/*) gave "argument
  list too long": sh builds the exec frame with sbrk() and its 12 KB had
  no room left after the glob; sh chmem is 16 KB now.
- `cut` took option arguments only as separate words (`-d : -f 2`);
  POSIX `-d: -f2` gave "syntax error".
- After `dfu-util ... :leave` the ROM loader jumps to the image without a
  reset and the flash controller stays busy (CFGBSY) - not caused by an
  odd image size (padding did not help).  hal_early_boot() resets once
  when it sees that; a stale SFTRSTF from an earlier `reboot` blocked
  this on the first try, so the reset flags are cleared at every boot.

## Layout

```
flipper/Makefile   m0: BOARD=qemu|flipper; all/run/flash
                   MINIX: minix, minix-run (QEMU), minix-flash (Flipper)
flipper/ld/        linker scripts (flipper.ld, qemu.ld, shared sections.ld,
                   server.ld for MM/FS/INIT images)
flipper/hal/       hal.h; common/ (vectors, kprintf, fbcon + 4x6 font, string)
                   flipper/board.c (STM32WB55), qemu/board.c (mps2-an386)
flipper/m0/        milestone-0 bring-up test program
flipper/m2/        test INIT and the m2test user program
flipper/rootfs/    root file system prototype (proto) and files
flipper/minix.mk   libc, kernel, servers, user programs, disk image
flipper/tools/     mkfont.py, mkflash.py (pack images), genaliases.py
flipper/vendor/    CMSIS headers (ST cmsis_device_wb, ARM CMSIS_5), Apache-2.0
src/kernel/arm/    mpx.S (entry/exit), klib.c (boot, irq, faults), cons.c
                   (serial console), shadow.c, nodev.c, armcpu.h
src/lib/arm/rts/   _sendrec.S, crtso.S, bootimg.S
include/           config.h: MACHINE FLIPPER / CHIP ARM_CM; ARM sigcontext,
                   jmp_buf, vir_bytes
```
