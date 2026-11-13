# MINIX 2.0.0 on Flipper Zero — bare-metal port plan

Goal: replace the Flipper Zero firmware with a native MINIX 2.0.0 kernel
(kernel + MM + FS + INIT) running directly on the STM32WB55, using a USB serial
console when one is connected and an on-screen tty (LCD + d-pad keyboard)
otherwise.

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

## Status

- **Milestone 0: done in QEMU, built for Flipper (hardware test pending).**
  `make run` boots the QEMU target, prints the banner, checks SysTick, and
  renders the LCD framebuffer as text (fbcon scroll/wrap test).
  `make BOARD=flipper` gives `build/flipper/m0.bin` (~4.6 KB). On the device it
  should print the banner on USART1 (115200 8N1, pins 13 TX / 14 RX, 3.3 V),
  show text on the LCD, echo button presses and serial input; hold BACK 2 s to
  halt.
- **Milestone 1: done in QEMU, built for Flipper (hardware test pending).**
  `make minix-run` boots the MINIX kernel (TTY, CLOCK, SYS, MEMORY, IDLE,
  stub PRINTER/FLOPPY/WINCHESTER tasks) with test stand-ins for MM, FS and
  INIT (`m1/`).  Checked: user -> server IPC, user -> task refused
  (E_BAD_DEST, as in MINIX), clock interrupts preempt a spinning user,
  console input by interrupt with REVIVE replies, and an unprivileged write
  to SysTick faults and becomes a signal while the kernel survives.
  `make minix BOARD=flipper` gives `build/flipper/minix/minix.bin` (~17 KB),
  kernel RAM: ~17 KB.

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
- Known, to fix in milestone 4: disk positions are 32-bit byte offsets
  (`dv_base`, signed `POSITION`), see the SD card layout section.

## Layout

```
flipper/Makefile   m0: BOARD=qemu|flipper; all/run/flash
                   MINIX: minix, minix-run (QEMU), minix-flash (Flipper)
flipper/ld/        linker scripts (flipper.ld, qemu.ld, shared sections.ld,
                   server.ld for MM/FS/INIT images)
flipper/hal/       hal.h; common/ (vectors, kprintf, fbcon + 4x6 font, string)
                   flipper/board.c (STM32WB55), qemu/board.c (mps2-an386)
flipper/m0/        milestone-0 bring-up test program
flipper/m1/        milestone-1 stand-ins for MM, FS, INIT
flipper/tools/     mkfont.py, mkflash.py (pack images), genaliases.py
flipper/vendor/    CMSIS headers (ST cmsis_device_wb, ARM CMSIS_5), Apache-2.0
src/kernel/arm/    mpx.S (entry/exit), klib.c (boot, irq, faults), cons.c
                   (serial console), shadow.c, nodev.c, armcpu.h
src/lib/arm/rts/   _sendrec.S, crtso.S, bootimg.S
include/           config.h: MACHINE FLIPPER / CHIP ARM_CM; ARM sigcontext,
                   jmp_buf, vir_bytes
```
