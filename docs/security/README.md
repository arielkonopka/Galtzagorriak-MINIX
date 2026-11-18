# Security reviews

Recurring reviews of Galtzagorriak MINIX against what the Flipper Zero can
*receive*. Started 2026-11-16 at the user's request; first the kernel and
its drivers, then the servers (FS, MM) and the userland.

## Threat model

The Flipper is a pocket gadget, not a server. The question every review
answers is: **can anything the device receives from outside harm it?**
"Harm" means one of:

- **DoS**: a hang, a watchdog reset or reset loop, the UI frozen (the
  responsiveness rule 0.8 is a security property here), a battery drained
  by an input that keeps the CPU awake;
- **RCE**: outside data that ends up executed, or that gains a process
  rights it did not have;
- **corruption**: kernel or process memory overwritten, flash or SD
  contents damaged, settings changed.

Inputs, by how they reach the kernel:

| Input | Path | Status |
|---|---|---|
| USB host (frames, control requests, CDC data) | `flipper/hal/flipper/usb.c`, `vendor/libusb_stm32`, `src/kernel/arm/cons.c`, `tty.c` | reviewed |
| Header UART (pins 13/14) | `cons.c`, `tty.c`, `board.c` | reviewed |
| microSD card (bus responses, partition tables, file systems) | `board.c` (SPI), `arm/disk.c`, `drvlib.c`; FS server | reviewed (kernel 2026-11-16, FS server weekly run 1) |
| Infrared receiver | `arm/flipdev.c` (`ir_int`, `ir_drain`) | reviewed |
| iButton / 1-Wire | `flipdev.c` (`ow_*`) | reviewed |
| GPIO, ADC, buttons | `flipdev.c`, `board.c` | reviewed (no interrupts or parsing from GPIO) |
| BLE / CPU2 mailbox and shared RAM | not implemented yet | review when it lands |
| Sub-GHz (CC1101), NFC (ST25R3916), 125 kHz RFID | parked at boot, no receive path yet | review when it lands |
| Wi-Fi module (ESP32-S2 on USART1) | Phase 22 | review when it lands |

**What is not a weakness by itself.** Canary apps (spec section M) and
other privileged tools legitimately need rights a normal app would not
(raw radio access, logging what a host does, pairing as bait). A review
does not flag such a right; it flags a path where *outside data* can use
it, or where the right leaks to something that should not have it.
Physical access with the case open (SWD, DFU) is out of scope (report S11).
The SD card holds the root file system, so a card *crafted* by an attacker
is already game over; a card that is damaged, worn out or from another
device must still not hang, reset-loop or corrupt the system.

## What every driver must do (review checklist)

1. **Lengths from outside are bounded** before any copy or index: a USB
   `wLength`, a frame length byte, a partition table entry, a CPU2 mailbox
   length. Clip, never trust.
2. **No unbounded loop on outside data in a task.** Kernel tasks are not
   preempted by tasks: a loop that the sender can keep going (a ring refilled
   while it is drained, a linked list read from the device, a retry loop)
   needs a bound or a time slice, and long waits give the CPU back
   (`task_yield`, or sleep on an interrupt). The watchdog resets after 2 s;
   rule 0.8 wants far less.
3. **No interrupt storm.** An interrupt source the outside drives (an edge,
   a receive register, an error flag) is cleared in its handler, and is
   switched off while nobody listens when that costs nothing.
4. **Rings have an overflow policy**: drop, or flow control; never overwrite
   unread data silently or index past the ring.
5. **Errors are counted, not looped on**: CRC and timeout retries are
   limited, and logging them cannot itself block (USB console).
6. **Nothing from outside reaches flash** at run time without an explicit
   user action (also the flash-write rule).
7. **Shared memory with another core** (CPU2 SRAM2, mailboxes) is read
   once into private memory, then checked (no double fetch).

## How a review runs

- **Cadence: weekly**, Mondays, by a scheduled Claude session (user's
  request 2026-11-16). Each run reviews what changed since the previous
  report: every new or changed driver, interrupt handler, kernel task and
  spec chapter about an input, against the checklist above. New drivers
  are added to the input table. When nothing relevant changed, the run says
  so and writes no report.
- **Order**: kernel and drivers (2026-11-16) -> FS server against damaged
  file systems -> MM (exec, signals) -> the rest of the userland (setuid
  programs, `pkg`, login, the GUI protocol) -> each new radio driver as it
  lands.
- **Output**: one report per run, `docs/security/YYYY-MM-DD-<scope>.md`,
  findings numbered (K = kernel, F = FS, U = userland, ...) with severity,
  the input, the failure, the fix or recommendation, and its status. Clear
  bugs are fixed in a PR with a test; design questions go to the user.
- **Severity**: Critical (remote/plug-in RCE, privilege, or a brick), High
  (any outside input can reset or freeze the device), Medium (needs a
  damaged medium, a long exposure, or a local condition), Low (battery,
  cosmetic, defence in depth), Info.
- Earlier report: `docs/spec/modernization-security-report.md` (S1-S12,
  2026-11-14): local privilege problems; its findings are not repeated.

## Tests on a PC

Where the kernel code can run on a PC, a review adds a test that feeds it
hostile input:

- `make -C flipper/tests/host` (any host C compiler; ASan + UBSan): the
  kernel sources are compiled unchanged with small stubs.
  - `parttest`: partition tables from damaged cards (loops, wrap-around,
    devices outside their container), fixed cases and 20000 random cards.
  - `usbtest`: the USB device side (`usb.c` + `usbd_core.c`) against a
    random host: any SETUP packet, data, halts, resets; checks memory
    safety, packet sizes, `wLength`, and that every byte sent arrives once
    and in order.
- `python3 -I flipper/tools/fsdamage.py` (QEMU, `make minix disk` first):
  damaged file systems on `/dev/hd3` (junk indirect block, wiped bit map,
  inconsistent super block) must not stop the system.
- `python3 -I flipper/tools/conflood.py` (QEMU, `make minix disk` first):
  floods the console and checks that processes keep running and the shell
  answers. QEMU's UART delivers slower than the Flipper's USB, so it
  cannot reproduce K1 itself; it guards against regressions in the slice.
- The M2 self-test and `flipper/tests/*.c` (sigret, badreloc, mpufrag, ...)
  cover the local attacks of the 2026-11-14 report.

Code that only the hardware can exercise (SPI card responses, timings) is
reviewed statically; the report says which findings have no test.
