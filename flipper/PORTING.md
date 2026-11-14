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
- Independent watchdog, 10 s, kicked from the clock tick: a kernel that
  hangs - or panics (hal_halt() stops kicking after a panic, so the
  message still gets out over USB) - resets, and a watchdog reset goes to
  DFU.  Tested with a kernel spinning at boot: DFU after 11 s.
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
