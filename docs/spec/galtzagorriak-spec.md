# Galtzagorriak MINIX: specification for the next phases

Status: Revision 2 (2026-11-14): re-evaluated for feature parity with the stock Flipper firmware.
Revision 1 (same day) covered security, RAM, swap and flash-installed programs; its phases are
renumbered here (section C and the Roadmap list the mapping).
Addition (2026-11-14, same revision): section I (time in UTC, Year 2038, location, first-boot
wizard), Phase 2d and Phase 18b.
Addition (2026-11-14): Phase 12d–12f (background-app tiers, park/hibernate API, two-ended
placement, swap without a card, manual notes; owner request).
Addition (2026-11-14): section J (own versioning: Galtzagorriak 0.x on a MINIX 2.0.0 base, ABI
number, tags and CHANGELOG; owner decision).
Addition (2026-11-15): section L (no file data in internal flash; base-system default settings in
a read-only ROM image with the user's changes on the SD card), R2b.11–R2b.12 (run-time cache
write budget, rebinding by SHA-256) and BLE bonds kept on the SD card (R15.4, R16.1.2); owner
request to make flash writes as rare as possible. Background: the project report `hybrid-fs-overlay-analysis.md` (Polish, not in the repo)
(a Linux-style overlay file system was considered and rejected).
Addition (2026-11-15): 11.2 (USB canary app, owner request), R19.9 (IP over an external IrDA
module with PPP, optional SD package) and an external USB host chip in Deferred. Then section M
(canary family) with 9.2, 14.2 and 16.5 (owner request: "various canaries, Bluetooth too").
Addition (2026-11-15): Phase 19 rewritten (lwIP as a low-priority `udrv` driver, `/dev/tcp` +
BSD sockets in libc, USB ECM/NCM with DHCP and mDNS, Wi-Fi module, PPP with frequency hopping on
CC1101, BLE Ethernet bridge; no TLS on the Flipper); owner decision. Background: the project
report `tcpip-stack-analysis.md` (Polish, not in the repo).
Baseline: `main` at `5e3d0f0` (flash text cache).
Background, evidence and line references: `modernization-security-report.md` (same folder).
Item IDs in brackets (S1, M3, A2 …) point into that report.

**One goal (owner, 2026-11-14): the functionality of the stock firmware, with responsiveness as
important as features.** A phase that adds a feature but breaks a latency budget in rule 0.8 is
not done.

This document is the work order for the implementation agent. Each phase has a goal,
numbered requirements (MUST = required, SHOULD = do unless there is a measured reason not to),
acceptance criteria that can be checked in QEMU and on the Flipper, and a RAM budget.

---

## 0. Rules for every phase

**0.1 RAM rule.** RAM is the scarcest resource. A phase may not increase the static RAM
(`.data` + `.bss`, SRAM1 and SRAM2) of kernel, MM, FS or INIT beyond its stated budget. It may
not increase per-process RAM beyond its stated budget either. Code and `const` data in the
kernel, the servers and cached user text are in flash and are free, within the flash limits
below.

**0.2 Flash limits.**
- The system image (kernel, MM, FS, INIT) must stay under **192 KB** (raised from 128 KB in
  Phase 2b to fit the drivers of Phases 4b–15; the build fails above it).
- Programs live in the installed region and the run-time cache (Phase 2b gives the layout).
- Nothing may be written outside those two regions.
- The CPU2 secure start (`SFSA`) is read from `FLASH->SFR` at boot, as the stock firmware does.
  Nothing may be written at or above `SFSA` minus the margin defined in R2b.1.

**0.3 Measurement.**
- A `make BOARD=flipper ramcheck` target MUST be added in Phase 1. It prints `.data`/`.bss` per
  image with `arm-none-eabi-size`.
- It compares the numbers with `flipper/ram-budget.txt` and fails when an image exceeds its
  line. Each phase updates that file in the same commit that changes the numbers.
- Baseline at `62ea5c1`, measured (bytes):

| Image | .data | .bss | Static RAM |
|-------|------:|-----:|-----------:|
| kernel | 652 | 16872 | 17524 |
| MM | 372 | 10260 | 10632 |
| FS | 476 | 27380 | 27856 (18.8 KB of it is the SRAM2 buffer cache) |
| INIT | 372 | 1376 | 1748 |

  Re-measure at `5e3d0f0` before Phase 1 and use that as the real baseline.
- Device check after each phase: `free` and `free -v` after login on the Flipper. Record free
  memory and the largest hole in PORTING.md.

**0.4 Tests.**
- Security fixes come with a test program under `flipper/tests/` (new). Each test tries the
  attack and reports PASS when the system refuses it and keeps running.
- `make INIT=m2 minix minix-run` (the existing self-test) MUST still pass after every phase.

**0.5 Option bytes.** No phase touches option bytes (WRP, PCROP, RDP, boot options).
Decided 2026-11-14: revisit after Bluetooth works; that is now R17.4, after Phase 15.
Flash ECC handling is not an option byte and is done now (R4a).

**0.6 Security invariants.** Once Phase 1 is done, these must hold forever and are re-checked by
every later phase:
- I1: Kernel code never writes to or reads from an address taken from a user process unless the
  address is checked against that process's map or the access is unprivileged (`LDRT`/`STRT`).
- I2: No user-controlled value (file header, message, sigcontext) decides privileged code's
  write addresses without a bounds check.
- I3: Every user process runs with MPU isolation. There is no "whole RAM" fallback.
- I4: Code that runs with a set-uid identity comes from that file and nowhere else.
- I5: No DMA transfer into or out of a process's memory outlives the process's ownership of that
  memory (exit, exec, kill, shadow removal and swap-out stop it first; R4b.5).
- I6 (responsiveness): budgets L1–L10 of rule 0.8 hold after every phase.

**0.7 Hardware first for RAM (owner rule, 2026-11-14).** Whenever a hardware feature of the
STM32WB55 or of a peripheral chip can hold data or do work that would otherwise need RAM, we use
it, provided it is safe (no new way to break invariants I1–I5). Section A.6 lists the known
cases. A requirement that buffers data in RAM must say why the hardware option doesn't apply.

**0.8 Responsiveness (owner rule, 2026-11-14; as important as the RAM rule).** The device must feel
at least as quick as the stock firmware. These budgets hold on the Flipper at 64 MHz, under the
load stated, in every phase from the one that introduces the measured path. Numbers are worst
case (max over the test run) unless marked p99.

| ID | Path | Budget | Load during the test |
|---|---|---|---|
| L1 | Button press → first changed pixel on the LCD (foreground app) | ≤ 30 ms p99, ≤ 50 ms max | background `cksum` of a 1 MB SD file + USB console output |
| L2 | Button press → `/dev/buttons` read returns in the foreground process | ≤ 2 ms p99 | same |
| L3 | Interrupt latency for radio/pulse IRQs (timer capture, CC1101 GDO, ST25R3916 IRQ) | ≤ 5 µs | any |
| L4 | Longest interval with interrupts masked (PRIMASK or BASEPRI at or above the radio level) | ≤ 20 µs, except the measured iButton slot (R6.x, ≤ 70 µs) | any |
| L5 | REVIVE → process running (R4b.3) | ≤ 100 µs | idle system |
| L6 | App launch from the launcher (text in the installed region) → first frame | ≤ 300 ms; ≤ 600 ms if the app was swapped out | none |
| L7 | Back to the launcher after an app exits → menu redrawn | ≤ 150 ms; ≤ 400 ms if the launcher was swapped | none |
| L8 | Longest CPU1 flash stall (erase or program) while a foreground app is in a radio stream, BLE connection or USB HID session | 0: not allowed (R0.8c) | |
| L9 | Longest UI freeze caused by a flash write at any other time | ≤ 25 ms (one page erase), and not more than one page per 100 ms | `pkg install --flash` of a 50 KB program |
| L10 | Boot to launcher menu | ≤ 3 s (stock is about 2 s) | normal `/etc/rc` |

Mechanisms every phase must respect:
- **R0.8a Priorities.** NVIC levels, from highest: faults; pulse-engine and radio timer/DMA
  (never masked by the kernel's `lock()`, R4a.1); SysTick and other kernel IRQs; PendSV.
  Scheduler queues, from highest: kernel tasks; MM/FS; the **foreground app and the protocol
  daemon serving it** (new "interactive" user queue, set by the launcher through R4b.8 foreground
  ownership); other user processes; idle work (cache writes, swap-out, compaction).
- **R0.8b No long kernel paths.** No kernel call or driver path may run longer than 1 ms with
  the scheduler held. Long copies (exec load, swap I/O, SD multi-block) are split into chunks of at
  most 4 KB with a preemption point between chunks.
- **R0.8c Flash writes are idle work.** CPU1 stalls on any flash fetch while flash is erased or
  programmed (≈ 22 ms per 4 KB page erase, ≈ 85 µs per double-word), and with CPU2 running BLE the
  erase must also go through CPU2's flash-activity protocol (HSEM, R15.1). Therefore:
  - flash is written only by a low-priority worker, one page (erase + program) per step, with the
    scheduler free in between;
  - no flash write while L8 applies (foreground radio stream, BLE connection event pending, USB
    HID active); the worker waits;
  - exec never waits for a cache write: the first run of an uncached program loads into RAM, and
    the worker copies the text to the cache afterwards (amends R1.4 and the `5e3d0f0` cache);
  - DMA streams keep running during a stall (DMA reads SRAM, not flash), so a pulse-engine ring
    MUST hold at least 25 ms of data whenever a flash write is allowed at all.
- **R0.8d Swap never stalls the foreground.** The foreground app and the daemon serving it are never
  swap-out candidates. Swap I/O runs at idle priority, in 4 KB chunks, and yields the SD bus to any
  foreground FS request between chunks. Swap-out is decided ahead of need: MM keeps a free-RAM
  reserve (default 16 KB) by swapping idle processes in the background, so a launch does not wait
  for a swap-out. Only the launcher is pre-swapped-in when an app exits (L7).
  Limits and order: R12.5 (amended), R12.23. Swap goes to the SD card only (R12.1).
- **R0.8e Measurement tool.** Phase 1 adds `make BOARD=flipper latcheck` and a kernel trace using
  the DWT cycle counter: timestamps for button IRQ, `/dev/buttons` reply, LCD DMA start, IRQ entry,
  mask/unmask (longest masked interval), REVIVE→run and flash stalls, kept in a 32-entry ring
  (256 B) that exists only in a `LATTRACE=1` measurement build; the release build has none, so
  Phase 1's 0 B budget holds. `lat` (user command)
  prints min/p99/max. Each phase records L1–L10 in PORTING.md beside the RAM numbers.

---

## A. Goal and architecture

**Goal:** reach the feature set of the stock Flipper Zero firmware, using MINIX for what the stock
firmware can't easily do:
- several isolated processes at once (MPU), with crashes contained;
- Unix tools, scripting and pipes;
- per-app users and groups as the permission model;
- apps installed and removed at run time as packages.

The stock features are listed in section B, from Sub-GHz to the mobile app's RPC and the app
ecosystem.

**A.1 Split every radio/IO feature into a thin kernel PHY and a user-space protocol layer.**
- **Kernel (driver task or interrupt handler):** only the parts with microsecond deadlines or
  direct hardware access:
  - SPI register access with bus arbitration;
  - timer capture and playback of pulse trains by DMA;
  - 1-Wire slot timing;
  - card-emulation anticollision (done by the ST25R3916 itself);
  - legally required limits (Sub-GHz transmit bands).
  Kernel code is in flash and costs RAM only for its state.
- **User space (installed programs and packages):** decoders, encoders, file formats, UI,
  databases, attacks, scripting. Code in the flash-installed region; RAM only while running.

This supersedes the "whole driver as a user process" plan in PORTING.md for timing-critical
devices. That plan still fits protocol daemons (`bled`, `rpcd`) that start on demand and exit when idle.

**A.2 Device model.**
- Each feature is a character device (`/dev/ir`, `/dev/subghz`, `/dev/nfc`, `/dev/lfrfid`,
  `/dev/ibutton`, `/dev/gpio`, …) with `read`/`write` for pulse or frame streams and `ioctl` for
  configuration.
- Mutually exclusive hardware gets an exclusive open (`EBUSY`): NFC and LF RFID share the
  NFC-IRQ/RFID-pull pin, and Sub-GHz and NFC share SPI1 bus time.
- Access is by Unix permissions: one group per device class (`subghz`, `nfc`, `rfid`, `ir`,
  `ibutton`, `gpio`, `usb`, `ble`, `gui`).

**A.3 Apps run as their own users.**
- The launcher starts each app as its own uid (Android-style), with the groups it declares in
  `/etc/apps`, or in its package's `CONTROL`.
- MINIX 2 has one gid per process, so add supplementary groups with a small fixed limit (R4b.4).

**A.4 Real-time behaviour for user processes.**
- A process woken by a device reply (REVIVE) goes to the **head** of the user queue (priority
  boost), so a protocol layer answers within tens of microseconds, not after the 16 ms tick.
- Kernel drivers get a one-shot high-resolution timer instead of the 60 Hz clock.
- The foreground app and its protocol daemon run in an "interactive" queue above other user
  processes; flash writes, swap and cache fills are idle work (rule 0.8).

**A.5 Memory tiers.**
- **Code:** installed region (hot: system and device apps) → run-time cache (warm: other
  commands, packages from SD) → RAM (cold fallback).
- **Data:** RAM, plus swap for idle processes. Background apps use the cheapest of four tiers
  (event, hibernate, park, stream; Phase 12d), so most of them hold no RAM while waiting.

**A.6 Hardware-first RAM saving (standing rule from the project owner, 2026-11-14).**
Whenever a hardware-specific approach can save RAM **safely**, use it. Concretely:
- DMA straight between a peripheral and the **user's buffer** (rule 0.8), not via kernel bounce buffers;
- peripheral FIFOs and on-chip memories as the buffer:
  - the CC1101 64-byte FIFO;
  - the ST25R3916 512-byte FIFO;
  - the USB peripheral's 1 KB packet memory (PMA) for endpoint buffers;
  - the PKA's own RAM for big-number operands;
  - the AES1 key registers for keys;
- run in place from flash (installed region, run-time cache), and `const` everything that never changes;
- bit-banding instead of byte flags plus locks;
- DMA in circular mode with half/full interrupts instead of software rings;
- SRAM2 for long-lived kernel and server buffers, freeing SRAM1 for processes. SRAM2a/2b keep their
  content in Stop2 and Standby with the retention bit set, so state there survives low-power modes without saving.

The requirement for each phase states which of these it uses.

---

## B. Gap analysis against the stock firmware

Status of the MINIX port at `5e3d0f0`: LCD, buttons, backlight, RGB LED, USB CDC console, SD
card, launcher with two apps. Bus map (from the stock firmware's `furi_hal_spi_config.c`):
- SPI1 ("r" bus): CC1101 (Sub-GHz) and ST25R3916 (NFC);
- SPI2 ("d" bus): display and SD card;
- I2C1: LP5562 LED driver, BQ27220 fuel gauge, BQ25896 charger.

RAM figures are estimates for the proposed design (A.1, A.6). "Kernel RAM" is static and
permanent. "App RAM" is per running process, only while it runs. Flash is code. Kernel flash
goes into the system image; user flash goes into the installed region.

| Feature (stock) | MINIX today | Missing in the kernel (PHY) | Missing in user space | Kernel RAM | App RAM | Flash kernel / user |
|---|---|---|---|---:|---:|---:|
| Display, buttons, GUI | `/dev/lcd`, `/dev/buttons`, launcher, gui lib | Foreground ownership of LCD and buttons; long-press Back to leave an app; `/dev/lcd` read for screen mirroring (from the last writer's buffer, 0 B) | Widgets (menus, dialogs, text input, file browser), status bar, notifications | ±0 (−1 KB with `fbuf` gone) | 1 KB fb + UI state per app | 1 / 10 KB |
| Power: battery, charging, power-off | — | BQ27220 + BQ25896 on I2C1 in `flipdev`; ship mode for power-off; low-battery event | Battery in the status bar, power menu | ~32 B | — | 2 / 2 KB |
| RTC (clock kept while off) | uptime-based clock | LSE RTC read at boot, written by `date -s` | — | ~8 B | — | 0.5 KB / — |
| Vibro, speaker, LED sequences | LED, backlight | Vibro GPIO; speaker PWM (TIM16, PB8); timed sequences in the device task | Music player, notification sounds | ~32 B | 2 KB | 1 / 3 KB |
| GPIO, 5 V out, USB-UART bridge | — | `/dev/gpio` (header pins only, allow-list); 5 V OTG via charger; UART bridge by circular DMA between USART/LPUART and the USB PMA | GPIO app, UART bridge app, i2c/spi tools | ~64 B | 2 KB | 3 / 6 KB |
| Infrared send and learn | — | **Pulse engine** (shared): timer capture → DMA → user buffer; DMA → timer playback with carrier | Protocol codecs (NEC, Samsung, RC5/6, SIRC, Kaseikyo, …), universal remotes, `.ir` files | 0 extra (pulse engine ~64 B) | 3–6 KB | 3 / 15 KB |
| iButton (1-Wire, Cyfral, Metakom) | — | 1-Wire master and slave timing; comparator for Cyfral/Metakom | Read, write, emulate apps, `.ibtn` files | ~64 B | 2 KB | 3 / 8 KB |
| 125 kHz RFID | — | 125 kHz carrier PWM; comparator + pulse engine capture; emulation by pull-pin DMA; exclusive with NFC (shared pin) | EM4100, HID Prox, Indala, AWID, FDX-B, … codecs; T5577 writing; `.rfid` files | 0 extra | 4–6 KB | 3 / 25 KB |
| Sub-GHz | — | CC1101 on SPI1: register `ioctl` with **transmit band check in the kernel**; async RX/TX through the pulse engine on GDO0; packet mode through the 64 B chip FIFO; RF switch; external CC1101 on the GPIO SPI | ~40 protocol decoders, frequency analyzer, read raw, `.sub` files, Keeloq keystore | ~96 B | 10–20 KB | 6 / 60 KB |
| NFC (A/B/F/V, MIFARE Classic, NTAG, DESFire, EMV read; emulation) | — | ST25R3916 on SPI1 + IRQ: field, poll, transceive straight between the 512 B chip FIFO and the user buffer; listener mode using the chip's automatic anticollision; RT wake (A.4) | Protocol stack, MIFARE Classic crypto1 and key recovery, dictionaries streamed from SD, `.nfc` files | ~96 B | 15–35 KB | 8 / 100 KB |
| BadUSB | CDC only | Composite USB CDC + HID keyboard/mouse; endpoint buffers in the USB PMA | DuckyScript interpreter, keyboard layouts | ~64 B | 4 KB | 4 / 10 KB |
| USB mass storage (SD as a drive) | — | Mass-storage class (BOT/SCSI), sector by sector from the SD driver; requires `/home` unmounted | Mode switch app | 0 extra (shares the disk buffer) | — | 4 KB / — |
| U2F / FIDO | — | HID FIDO interface; PKA ECDSA P-256 sign; counter in a flash page; key wrapping in the CPU2 secure enclave (as the stock firmware does) | CTAP1 logic | ~64 B | 3 KB | 6 / 8 KB |
| Bluetooth LE (mobile app, serial, HID, remote) | — | CPU2 start (FUS + BLE light stack, as stock), IPCC mailbox + HSEM, transport task; mailbox and event buffers in SRAM2 | `bled` (GATT: serial, battery, device info, HID), pairing UI | ~1 KB SRAM1 (task stack) + 6–8 KB **SRAM2** | `bled` 6 KB | 8 / 20 KB |
| qFlipper / mobile app RPC | — | — | `rpcd`: protobuf (nanopb) RPC over the USB CDC data interface and BLE serial: storage, app start, screen mirror, input, system info | 0 | 6–10 KB | — / 25 KB |
| Firmware update from SD | DFU only | Updater that rewrites the system region from a signed image (runs from the installed region with interrupts masked; ROM DFU is the recovery) | `update` tool | 0 | 2 KB | 3 / 4 KB |
| Apps ecosystem (FAP) | packages (`pkg`), launcher | — | App manifests with groups and icons in `CONTROL`; package catalog over RPC; optional Furi-compatibility library for porting stock apps (later) | 0 | — | — / later |
| Archive, settings, desktop lock | shell tools | — | Archive/file browser, settings app, optional PIN lock | 0 | 4 KB | — / 15 KB |

**Projected totals for full parity:**
- **Kernel static RAM:** about +1.5 KB in SRAM1 (drivers and the BLE task) and +6–8 KB in SRAM2
  (BLE). The SRAM2 part is paid by shrinking the FS buffer cache from 18 to about 11 blocks.
- **Recovered by Phase 2:** about −5 KB, plus −1 KB (`fbuf`), plus the per-command buffer diet.
  **Net SRAM1:** about −4 KB versus today.
- **Kernel flash:** about +60 KB. The system image grows from 62 KB to about 125 KB: raise the
  system region from 128 KB to **192 KB** (R2b.1).
- **User flash:** about 310 KB for the device apps, plus the base system. The 77 base commands are
  436 KB of text *(measured)*, because each one links its own libc. Shared library: R2b.9.
- **Typical run-time load:** launcher (swapped while an app runs) + one device app (≤ 35 KB) +
  `rpcd` or `bled` (≤ 10 KB) + a USB shell (≤ 12 KB). This fits in the ~106 KB free after login
  measured in QEMU at `5e3d0f0`, with headroom from Phase 2.

---

## C. Earlier proposals revisited for this goal

| Earlier proposal | Conflict with feature parity | Change |
|---|---|---|
| PORTING.md "userspace drivers" (`udrv`/`devd`, whole driver as a process) | Sub-GHz, IR, LF RFID, iButton and NFC listening need µs timing a process can't give | Kernel PHY + user protocol layer (A.1). `udrv`/`devd` is kept for protocol daemons (`bled`, `rpcd`), started on open and stopped when idle |
| M5: drop `/dev/lcd` read with `fbuf` | The mobile app and qFlipper mirror the screen | Keep the read, served from the **last writer's own buffer** (the kernel remembers the process and address of the last write and copies from there): still −1 KB |
| M5: remove the disk bounce buffer `w_buf` | USB mass storage and swap encryption want a sector buffer | Keep **one** 512 B to 1 KB sector buffer shared by SD, USB mass storage and swap crypto; remove `tmp_buf` and the other duplicates |
| Swap reserves AES1 for itself | U2F needs key wrapping | No conflict if U2F does as stock does and wraps keys in the CPU2 secure enclave (AES2 slots, available once BLE runs). Swap keeps AES1 |
| Swap eligibility | A process with armed DMA must never move | Exclude processes holding a device with an armed DMA or capture stream (R12.3 amended) |
| FS buffer cache fills SRAM2 (18 blocks) | BLE needs 6–8 KB of SRAM2 for the CPU2 mailbox | Cache shrinks to about 11 blocks when BLE is enabled (R15.3). Multi-block SD transfers and cache tuning keep FS speed up |
| Option bytes deferred until BLE | U2F needs protected keys | Consistent: U2F comes after BLE and uses the CPU2 enclave. WRP/RDP reconsidered there |
| Installed region 256 KB | Base userland alone is 436 KB *(measured)*, plus about 310 KB of device apps | System region 192 KB; installed region grows to the CPU2 limit read from `SFSA`; a shared libc in flash (R2b.9) |
| Phase order "security → hardening → RAM → swap → …" | Features came last | Security stays first; memory foundations next; then the device platform; then features in order of value and dependency; app ports follow their drivers; code modernization is continuous |
| Single gid per process (MINIX 2) | Apps need several device groups | Supplementary groups, fixed at 4 per process (R4b.4) |
| No `select`/`poll` in MINIX 2 | Apps wait on buttons, a device and a timer at once | `poll()` in FS (R4b.6) |
| 60 Hz clock as the only timer | Protocol timeouts are in ms; RT wake needed | One-shot high-resolution timer for drivers; `REVIVE` priority boost (R4b.2, R4b.3) |
| FPU opt-in (A9) | Some stock apps and games use `float` | Kept, moved earlier into the app-porting framework phase (R5.7) |

---

## D. Porting the stock apps: model, shim layer, licensing

### D.1 How stock concepts map onto MINIX

| Stock (Furi / FAP) | MINIX equivalent |
|---|---|
| App = FAP (relocatable ELF loaded into the shared heap) or a built-in app | App = MINIX PIC program, text in the installed region or run-time cache, own process, own MPU-isolated memory, own uid |
| `application.fam` manifest | `CONTROL/control` plus an `app:` section (label, icon, groups, chmem, category) read by the launcher; generated from the `.fam` by a host tool |
| `FuriThread` (workers) | (a) the worker's timing-critical part moves into the kernel PHY (A.1); (b) the rest runs as **cooperative threads inside the process** (own small stacks, switched with `setjmp`/`longjmp` or a 20-line context switch), multiplexed by `poll()`. No `fork`: without an MMU it would copy the image |
| `FuriMessageQueue`, `FuriEventFlag`, `FuriMutex`, `FuriSemaphore` | In-process queue and flags (the scheduler is cooperative, so mutexes become cheap assertions); across processes: pipes |
| `FuriTimer` | `poll()` timeout plus the millisecond alarm (R4b.2) |
| `furi_record_open("gui")`, ViewPort, Canvas | `libfuri` GUI: draws into the app's 1 KB framebuffer and writes `/dev/lcd`; input from `/dev/buttons`, with short/long/repeat as stock |
| ViewDispatcher, SceneManager, Submenu, Popup, DialogEx, TextInput, ByteInput, VariableItemList, Widget, FileBrowser | Same APIs in `libfuri` (ported code, see D.3) |
| `storage` record (`/ext`, `/int`, `/any`) | POSIX files: `/ext` → `/home/flipper` on the SD card, `/int` → `/etc/flipper` (settings) |
| `FlipperFormat` | The stock library compiled for MINIX on top of stdio |
| `notification` record (LED, vibro, sound sequences) | `/dev/led`, `/dev/vibro`, `/dev/speaker`, with sequences played by the device task (Phase 6) |
| `furi_hal_subghz`, `furi_hal_nfc`, `furi_hal_infrared`, `furi_hal_rfid`, `furi_hal_ibutton`, `furi_hal_gpio` | `ioctl`/`read`/`write` on `/dev/subghz`, `/dev/nfc`, `/dev/ir`, `/dev/lfrfid`, `/dev/ibutton`, `/dev/gpio`, wrapped by `libfuri` HAL stubs where the protocol code calls them |
| Protocol libraries (`lib/subghz`, `lib/nfc`, `lib/lfrfid`, `lib/infrared`, `lib/ibutton`, `lib/one_wire`, `lib/toolbox`) | Ported as user-space libraries; their HAL calls go to the stubs above |
| `cli` commands, RPC | Unix commands (`subghz rx`, `ir tx`, …) and `rpcd` |
| `furi_check`/crash → whole device reboots | The process dies and the launcher shows "app crashed"; the rest of the system keeps running |

### D.2 `libfuri`: source compatibility, not binary compatibility

- **What:** a user-space library giving the subset of the Furi and app APIs that ported apps use:
  - core: threads (cooperative), queues, timers, records, `furi_string`, logging to `/dev/log`;
  - GUI and modules;
  - storage, dialogs, notification;
  - FlipperFormat;
  - HAL stubs (device `ioctl`s).
- **Not:** a FAP ELF loader. FAP binaries depend on the stock firmware's exact API table and
  shared heap; a loader would cost RAM and protect nothing. Apps are **rebuilt from source** against
  `libfuri` with a `fam2pkg` host tool producing a MINIX package.
- **Why it's worth it:** the stock apps and their protocol libraries are tens of thousands of lines;
  porting them against a shim is far cheaper than rewriting them, and community apps can follow the
  same path.
- **Size:**
  - core + GUI + modules ≈ 25–35 KB of code;
  - fonts (u8g2, `const`) ≈ 10 KB;
  - FlipperFormat ≈ 6 KB.
  All flash. Without a shared library (R2b.9) each app carries its own copy, which is why R2b.9
  is a prerequisite for porting more than a handful of apps.
- **RAM per app:** `libfuri` state ≈ 1.5 KB, the framebuffer 1 KB, and 1–2 KB per cooperative thread stack.

### D.3 Licensing (decided 2026-11-14)

- **Decision:** the project may become **GPL-3.0 as a whole**. Stock firmware code (GPL-3.0) may
  be reused freely, kernel included, and mixed with MINIX code where that's simpler.
- **Compatibility:** MINIX 2.0.0 is BSD-3-Clause (relicensed in 2000), which is GPL-3.0-compatible:
  the combined system is GPL-3.0, and the original BSD notices stay in the files that carry them.
- **Housekeeping (cheap, keeps provenance clear):**
  - keep upstream copyright notices;
  - put an SPDX line in new and ported files;
  - note the stock source file in a comment when code is copied;
  - record `License:` in each package's `CONTROL`.
- **Third-party parts inside the stock tree keep their own licenses:** u8g2 (BSD-2), nanopb (zlib),
  mbedTLS (Apache-2.0), ST CMSIS/HAL (BSD-3/Apache-2.0). All are GPL-3.0-compatible. Any ST
  middleware is under ST's own terms; check it before reuse.
- **Never included:** Keeloq manufacturer keys and similar secrets; the stock firmware doesn't
  ship them either. The Sub-GHz app loads a user-supplied keystore.

---

## E. Radio chips: what can be "reprogrammed"

| Chip | Firmware? | What we can change | Use |
|---|---|---|---|
| CC1101 (Sub-GHz) | None: a register-configured transceiver | Everything in its registers: 300–348, 387–464 and 779–928 MHz; 2-FSK, GFSK, 4-FSK, MSK, ASK/OOK; 0.6–600 kbps; packet handler, FEC, whitening. Presets are just register tables (as stock's custom presets) | All Sub-GHz features; also a **low-rate IP link** between two Flippers (R19.11, with frequency hopping), within the kernel's band and duty-cycle limits (R8.2) |
| ST25R3916 (NFC) | None: register-configured | Modes NFC-A/B/F/V reader, card emulation, NFC-DEP (P2P), transparent mode | All NFC features. IP over NFC is not worth it |
| CPU2 (Cortex-M0+ wireless co-processor) | Yes, but **only ST-signed, encrypted images**, installed by ST's FUS. Our own code can't run there | Pick among ST's stacks: BLE full, light (stock), extended, HCI-only; 802.15.4 MAC; Thread FTD/MTD; Zigbee; concurrent BLE + Thread or BLE + Zigbee; BLE LLD (proprietary 2.4 GHz) | One "radio personality" chosen at install time. Switching stacks is a FUS operation: slow, wears flash, and a failure leaves the radio unusable until repaired with qFlipper. Not something to switch at run time |

Flash cost of a CPU2 stack comes out of the same 1 MB. The BLE light stack is the smallest; the
Thread, Zigbee and concurrent stacks are much larger (check ST's release notes for the exact size
of the chosen version). `SFSA` moves accordingly, and R2b.1 reads it at boot, so the installed
region shrinks automatically. Choosing a bigger stack is an owner decision (R16.1.0, decided 2026-11-14: the full BLE stack,
for central and GATT client apps; R19.13 for Thread or Zigbee).

---

## F. Native MINIX apps: a framework for apps that live in time

Stock apps are ported (section D). Native apps are new apps written for MINIX first. The first two
are an interval reminder and an alarm clock (owner request, 2026-11-14), and more will follow.
They need three things the stock design keeps inside one always-running firmware: timed wake-ups
when no app is open, alerts that win over whatever is on screen, and per-app settings. MINIX
provides them as small shared services, so each new app is only its UI plus a few files.

### F.1 Design principles

- **Nothing resident while waiting.** No daemon sits in RAM between events. The next due time
  is held by the RTC alarm hardware itself (A.6, rule 0.7); the process that handles an event is
  started when the alarm fires and exits when it is done.
- **Alerts preempt the screen, not the app.** An alert takes the LCD and buttons for a moment;
  the foreground app keeps running (its radio stream or USB session is not interrupted) and gets a
  redraw signal when the alert closes.
- **Data, not code, describes an app's timers.** A timer is a small text file owned by the app's
  uid. A new app adds timers by writing files; it does not extend a daemon.
- **Settings are plain files** in the stock FlipperFormat style (`Key: value`), readable by
  `libfuri` (D.2) and by shell scripts.

### F.2 Shared services

- **Timers (`timed`).** Spool directory `/var/timers/`, one file per timer, owned by the app's
  uid (mode 0600; the directory is sticky, R3 sticky bit). Fields:
  - `Kind`: `once` (date and time), `daily` (HH:MM plus a weekday mask), `interval` (every N
    minutes inside a window HH:MM–HH:MM on chosen weekdays, aligned to the window start);
  - `Category`: `alarm`, `reminder` or `info` (decides how it may break through do-not-disturb);
  - `Action`: `notify` (title, text, sound, vibro, LED pattern, snooze minutes) or `run`
    (a program of the same package, started as the owner with `--timer <id>`);
  - `Missed`: `fire-once` or `skip`, for events that fell in a power-off or a clock change.
  `timed` is a short-lived program: it reads the spool, fires what is due, computes the earliest
  next time over all timers, arms RTC Alarm A with it (`ioctl` on `/dev/rtc`), and exits.
- **RTC alarm path (kernel + INIT).** When Alarm A fires (from Run, Stop2 or Standby), the RTC
  driver sends `SIGALRM`-class signal `SIGRTC` (new) to INIT; INIT, which is resident anyway, forks
  `timed`. The same signal is sent after `stime()` and after a wake from power-off, so `timed`
  recomputes after any clock change. Alarm B stays free for the system (e.g. the updater).
- **Alerts (`alert`).** The program that shows an alert, started by `timed` (or by any app in
  group `notify` through `notify_post()`):
  - takes the screen and buttons with the new `ioctl(FG_PREEMPT)` (R7b.2); shows title, text and
    the time; Back = dismiss, OK = done (counts for reminders), Up/Down = snooze;
  - sound through `/dev/speaker` sequences played by the device task from a `const` table or a
    user table (R6.3), so `alert` doesn't wake for every note; vibro patterns likewise;
  - LED patterns run in the **LP5562's own program engines** (three engines, 96 instructions):
    the MCU loads the pattern once and sleeps; 0 B RAM, no CPU while it blinks;
  - repeats sound for up to the configured ring time (alarm default 10 min, rising volume), then
    marks the event missed and exits.
- **Notification policy** in `/etc/notify.conf` (root) and `~/.config/notify` (user): volume,
  vibro on/off, LED on/off, do-not-disturb window. Category `alarm` always rings (sound or vibro
  as the alarm itself says), even in do-not-disturb and even with the global sound switch off,
  unless the alarm's own setting is vibro-only. `reminder` and `info` follow do-not-disturb.
- **Per-app settings and data** in `/ext/apps_data/<app>/` as stock does, owned by the app's uid.
- **`libapp`** (shared library in flash, R2b.9; ≈ 4 KB): `timer_add(spec)`, `timer_remove(id)`,
  `timer_list()`, `notify_post(category, title, text, opts)`, `settings_get/set()`,
  `on_redraw()` hook. Works with `libfuri` GUIs and with plain `/dev/lcd` programs.
- **Manifest fields** in the package `CONTROL` / `/etc/apps`: `Timers: yes` (adds group `timers`,
  which may write `/var/timers`), `Notify: alarm|reminder|info` (highest category the app may
  post), `DeskMode: yes` (may keep the backlight on while charging).

### F.3 Power and accuracy

- In Run and Stop2 the RTC alarm wakes the CPU directly (R6.8). The ST7567 LCD keeps its image with
  the CPU stopped, so a desk clock updates once a minute from the RTC wake-up timer and sleeps in
  between.
- **Off with alarms armed.** Power-off is the BQ25896 ship mode (BATFET off); the RTC stays powered
  from VBAT (its own LDO from the battery). Before ship mode the RTC alarm output (Alarm A,
  open-drain) is enabled on PC13, which is also the charger's QON input: at the alarm the charger
  turns power back on, the device boots (L10) and `timed` rings. INIT reads the wake reason before
  the boot key combos (Back looks pressed until the flag is cleared). Because a start from off
  takes QON + L10, `timed` arms the alarm early by the measured boot lead and rings at the real time
  (owner, 2026-11-15). Details in next-tasks T14c.
- **Wake, do the job, off again (owner, 2026-11-17).** A timer file may say `After: off` (and
  `timed --then-off` does the same for an app). It is for jobs like a rescue beacon or a weather
  log: the app adds the timer and powers the Flipper off (`halt`); the RTC wakes it, the job runs,
  and the Flipper goes back to ship mode. The rules:
  - **Never off if the Flipper was on before the job.** `timed` powers off only when **this** boot
    was caused by the RTC alarm (wake reason read at boot, F.3 above, kept in an RTC backup
    register bit, 0 B RAM). If the Flipper was already running when the timer fired, the job runs
    and the Flipper stays on, exactly as for a timer without `After: off`;
  - not if a button was pressed since the boot, or a user session (USB console, launcher app in
    front) is active: the user took it in hand, so it stays on;
  - several jobs due close together: off only after the last; if the next timer of any kind is
    due in less than ≈ 4 min, sleep (Stop2) instead of off (a boot costs more than that much sleep;
    `reports/hibernation-analysis.md`);
  - a job that hangs is ended after its `Timeout:` (default 60 s; SIGTERM, then SIGKILL after
    3 s) and the Flipper still goes off, so a stuck beacon cannot drain the battery;
  - with USB attached ship mode cannot cut power: sleep instead;
  - the boot after an RTC wake with an `After: off` job due is **quiet**: no splash, backlight
    off, no CPU2/Bluetooth, no launcher; a button press during it turns it into a normal boot
    (and cancels the power-off, rule above);
  - power-off is the normal orderly halt (sync, N.2 clean flags, then ship mode).
- **Timer type `emergency` (owner, 2026-11-17).** `Type: emergency` is for a rescue signal
  (beacon, screen flash, Morse). It means `After: off` with the rules above, and in addition:
  - it ignores do-not-disturb and the notification policy (like an alarm);
  - it is never skipped: `Missed: fire-once` is forced, and a timer due during a normal session
    still runs (the signal goes out, the Flipper stays on);
  - low battery does not cancel it; `timed` **thins it out by halving** (owner, 2026-11-17):
    below the low-battery threshold (default 20 %) it skips every second occurrence; each time
    the remaining charge halves again (10 %, 5 %, 2.5 %, ...) it skips every second one of those
    that are left, so the signal runs at 1/2, 1/4, 1/8 ... of its rate (at most 1/64). Each level
    then lasts about as long as the one before (half the charge at half the rate), so the beacon
    keeps going for as long as possible instead of dying early. The level and the occurrence
    counter live in RTC backup register bits (0 B RAM); the app gets the level in `TIMED_LOWBAT`
    and may also shorten each signal. Only the charger's own cut-off stops it;
  - only a package with `Timers: emergency` in its manifest (F.2, checked by `pkg`) may create one,
    so an ordinary app can't keep waking a switched-off Flipper.
  Other wake-and-off jobs (weather log, sensor sample) use plain `After: off`.
- **Timer type `wakeup` (owner, 2026-11-17: "wake_up_on_time").** The alarm clock's timers
  (`alarm`, 7b.2). Normal battery: as today (rings on time, from off too, with the boot lead).
  **Below 10 %** (configurable) the alarm itself is **not run**: `timed` does not start the app,
  and instead gives one minimal notice that the alarm was skipped, whichever costs less: a short
  LED blink pattern (LP5562, a few mA for well under a second) by default, or a short beep
  (`notify.conf`: `lowbat_notice=led|beep`). The skip is logged (`/var/log/timed`, batched,
  N.2) and the alarm app shows "Alarm skipped: battery low" when next opened. The notice runs
  from Stop2 or from a quiet boot (no screen). Note for the manual: an LED blink will not wake
  a sleeper; the alarm app warns when an alarm is armed and the battery is under 15 %.
- LSE accuracy is about ±20 ppm (≈ 2 s a day). Settings expose RTC smooth calibration (`RTC_CALR`),
  and the clock is synced from the mobile app's date/time RPC (R15.5) or NTP over Phase 19.

### F.4 Adding the next native app

A new native app is: a package with a manifest (F.2), a UI (`libfuri` or plain), optional
timers and alerts through `libapp`, settings in `/ext/apps_data/<app>/`. It gets its own uid
(A.3). It needs no kernel change and no daemon change. Each new app gets a subsection with
requirements, acceptance and its RAM figure, in the phase whose drivers it needs: time-based apps
in Phase 7b ("7b.N"), Bluetooth apps in Phase 16 ("16.N", e.g. the Bluetooth adapter 16.1), and so on.

### F.5 Gadget apps (owner request, 2026-11-14)

The Flipper should also be a useful everyday gadget: the water reminder (7b.1) is one, a hidden
camera finder (`camfind`, 9.1 and 16.3) is another. Gadget apps follow F.1–F.4, and share one
engine for "find something by radio": scan → score → pick one → hot/cold locate.

**Hidden camera finder: what physics allows.** No single sensor finds every camera, so `camfind`
combines signals and is honest about each one's strength in its UI:

| Signal | Hardware | What it can and can't do | Strength |
|---|---|---|---|
| Wi-Fi scan for camera vendors and setup SSIDs | Optional ESP32 Wi-Fi devboard on the GPIO header, over the UART (R6.5) | Most hidden cameras sold today are Wi-Fi IP cameras. A passive scan lists access points and stations; MAC vendor prefixes (OUI) and SSID patterns of camera makers and cheap camera modules mark suspects. Cameras with a randomised MAC, or recording to SD without Wi-Fi, are not seen | **Strong** (best option) |
| BLE scan | CPU2 in observer (scanning) role | Some cameras advertise over BLE during setup, or use BLE-capable modules. Scanning likely needs the full BLE stack (light = peripheral only; confirm with R2b.10 and the stack notes) | Medium |
| Sub-GHz RSSI sweep | CC1101 | Finds transmitters at 300–348, 387–464, 779–928 MHz. Analog spy cameras mostly use 1.2, 2.4 or 5.8 GHz, which the CC1101 can't receive. It does find other hidden transmitters (bugs, trackers on 433/868 MHz) and the **cellular uplink of 4G/GSM cameras** in LTE band 20 (832–862 MHz) and band 8 / GSM-900 (880–915 MHz), with the user's own phones in airplane mode (R9.1.3) | Weak for Wi-Fi/analog cameras; **medium** for SIM (4G/GSM) cameras |
| Infrared receiver (built in) | 38 kHz demodulating IR receiver with automatic gain | Its output is digital (a 38 kHz burst or not) and its gain adapts, so it can't measure light intensity. Night-vision LEDs (850/940 nm) shine continuously, so it can't see them. At most it flags modulated IR nearby | Very weak; hint only |
| **IR intensity meter** (R9.1.7) | External IR photodiode on a header ADC pin (a part costing about 1 €) | Measures IR level directly: finds night-vision illuminators in a dark room, and in active glint mode (the Flipper's IR LED + synchronous detection) finds camera lenses by their reflection | **Strong** for night-vision cameras and lenses at close range |
| Wi-Fi motion correlation (R9.1.8) | ESP32 devboard in monitor mode | A camera's compressed video stream grows when something moves in front of it. The devboard counts bytes per station (frame lengths are visible even in encrypted networks the user is not part of); the user stands still, then waves, and a station whose uplink follows the movement is watching the room | **Strongest** for Wi-Fi cameras |
| Analog 2.4/5.8 GHz receiver (R9.1.9) | Optional module: RX5808 (5.8 GHz, analog RSSI output to an ADC pin) or nRF24L01+ (2.4 GHz, RPD bit) | Finds analog video transmitters the CC1101 can't hear. The nRF24 sees all 2.4 GHz energy (Wi-Fi, BLE too), so it is an energy meter, not a camera detector | Strong for analog cameras |
| Lens glint helper | RGB LED and the user's eyes, or the user's phone camera | Strobes the LED while the user looks along it for a lens reflection; the help screen also explains the phone-camera trick (many phone cameras show night-vision IR LEDs as purple light). Guidance, not detection | Guide |

**The Flipper's IR LEDs can't be used as a light sensor** (checked on the schematic, 2026-11-15, see
`reports/camfind-detection-analysis.md` in the project files): the three VSMY14940 LEDs run from `SYS`
through 39 Ω each to the drain of an N-MOSFET (DMN2058UW); the MCU (PB9, no ADC) drives only the
gate. Neither LED terminal reaches an MCU pin, so their photovoltage can't be read without a
hardware change. The built-in receiver's output lands on PA0, which has an ADC input, but the
TSOP75338 output is digital, so that doesn't help either.

Scope rule: `camfind` only listens. It never connects, deauthenticates or jams; the ESP32
firmware for it implements passive scanning only.

**Other gadget ideas (suggestions only, not scheduled):**
- **Tracker detector:** (now scheduled as part of the BLE canary, R16.5.4) BLE scan for Find My, SmartTag and Tile-style trackers that stay near you
  over time, with the same hot/cold locate (needs the BLE scan of 16.3).
- **Weather and sensor display:** decode your own 433/868 MHz weather stations and sensors
  (temperature, humidity, rain) on the Sub-GHz decoders, shown as a small dashboard.
- **Kitchen and Pomodoro timers, medication reminder:** `timed` timers with alerts (7b), logging
  doses like the water count.
- **NFC business card:** write a vCard or a link to a blank NTAG sticker or card (Phase 14).
- **Lost BLE device finder:** hot/cold to your own headphones or tag by RSSI.
- **Meeting badge:** "do not disturb" or name on the screen, desk-clock style, with RTC-timed
  changes.

RAM principle for gadgets: tables (OUIs, SSID patterns, tracker signatures, sensor formats) are
`const` in the app's flash; the only RAM is the current scan list, bounded in size.

---

## G. The Flipper as a USB peripheral module

Owner request (2026-11-14): the Flipper plugged into a PC works as a USB module for its radios
and readers: a Bluetooth adapter, an NFC reader, a 125 kHz RFID reader, an iButton (1-Wire)
reader, and likewise Sub-GHz and infrared. Each mode is a **USB profile** chosen in the `usbmode`
app (or `usbmode <name>` from the shell), and runs in the background with the screen free.

### G.1 Principles

- **Standard classes first,** so the PC needs no driver: HID keyboard ("keyboard wedge": the ID
  is typed followed by Enter), CCID (smart-card readers, PC/SC), CDC-ACM (serial), Bluetooth
  (class 0xE0). A mode that needs a protocol PC tools already speak emulates that protocol on a
  serial port, not a vendor's USB IDs.
- **Our own USB IDs.** The project uses its own vendor/product IDs (e.g. from pid.codes for open
  hardware projects), never another vendor's, so we don't impersonate a product. Where a host
  driver binds only by ID (Linux `ir_toy`, `btusb` quirks), the docs give the one-line `new_id`
  or `btattach` command instead.
- **One profile at a time, console always.** The STM32WB55 USB device has 8 endpoint numbers and
  1 KB of packet memory (PMA). The CDC console keeps its 3 endpoints in every profile; each profile
  adds one function. Endpoint buffers live in the PMA only (A.6), so a profile costs no SRAM for
  USB buffering.
- **Kernel fast path, no resident daemon.** A mode runs for hours, so its logic lives in the
  device task with a few dozen bytes of state, and data moves between the chip FIFOs and the USB
  packet memory without SRAM copies. G.3 compares the options and gives the numbers.
- **Same ownership rules as on the device.** A mode holds the radio it uses; an app that needs the
  same radio gets `EBUSY` with the mode's name. The mode runs as its own uid in the radio's group.

### G.2 Modes

| Mode | USB function seen by the PC | PC side | Needs | RAM while active (G.3) | Phase |
|---|---|---|---|---|---|
| RFID reader 125 kHz | HID keyboard wedge (ID in hex or decimal, format setting) **or** CDC serial with a simple ASCII line protocol (`EM4100 0102030405\r\n`, also reads HID Prox, Indala and the other stock protocols) | any text field; serial scripts | 8 (LF RFID), 10 | kernel ≤ 32 B | 11.1 |
| iButton / 1-Wire ("1key") | HID wedge (ROM code) **or** CDC serial emulating the DS2480B serial 1-Wire master protocol | text field; OWFS, `digitemp` and other DS9097U tools work unchanged | 6 (iButton), 10 | kernel ≤ 32 B | 11.1 |
| Infrared | CDC serial with the IR Toy "sampling mode" protocol | LIRC / Linux `ir_toy` driver (bind with `new_id`), `irrecord`, `ir-ctl` | 6 (IR), 10 | kernel ≤ 48 B + 512 B lent by FS | 11.1 |
| Sub-GHz | CDC serial: raw pulse stream (receive and transmit, stock `.sub` RAW timing format) and a register-preset command set | `rtl_433`-style decoders reading pulse files, URH, scripts | 8 (Sub-GHz), 10 | kernel ≤ 48 B + 512 B lent by FS | 11.1 |
| NFC reader | **CCID** contactless reader (PC/SC: ISO 14443-4 cards via APDUs, MIFARE Classic/Ultralight and other memory cards via the PC/SC Part 3 pseudo-APDUs, e.g. `FF CA 00 00 00` = get UID) **or** HID wedge (UID) | `pcscd`, Windows smart-card service, `pcsc_scan`, OpenSC, libnfc via its PC/SC driver | 13 (NFC), 10 | kernel ≤ 64 B; helper 4 KB only for MIFARE Classic | 14.1 |
| Bluetooth adapter (dongle) | Bluetooth HCI controller, USB class 0xE0 (events on interrupt IN, ACL on bulk); fallback CDC serial with H4 framing | Linux BlueZ (`btusb`, or `btattach -P h4` on the serial fallback); others see a BLE controller where their stack accepts an LE-only one | 15 (CPU2), 10 | kernel ≤ 32 B | 16.2 |

The Bluetooth adapter of 16.1 (Flipper as central, the mouse appears as a USB mouse) and the
dongle mode of 16.2 (the PC's own Bluetooth stack drives the Flipper's radio) are two different
modes; both need more than the light stack (see 16.1 and 16.2).

### G.3 Options explored for RAM (owner request: "explore the options, remember the RAM")

**Question 1: all functions at once (one big composite device) or one mode at a time?**

| Option | RAM | Other limits | Verdict |
|---|---|---|---|
| Composite with every function enumerated at once | Each function's logic must be live: 6 daemons ≈ 15–20 KB if in user space | 8 endpoint numbers and 1 KB PMA are not enough for console + HID + CCID + BT + 3 CDC ports | No |
| **One mode at a time + console** (re-enumerate on change, R10.1) | Only one mode's state; with a `union` of mode states in the kernel the cost is the largest one, not the sum | PMA plan below fits every mode | **Recommended** |
| Mode without the console (frees 3 endpoints) | same | Only needed if a future mode doesn't fit; none does now | Kept as a fallback |

PMA plan (1024 B, A.6): buffer table 64 B; EP0 2 × 64; console CDC 64 + 64 + 16 (notify).
That leaves 688 B for the mode: HID wedge 16 B; CCID bulk 2 × 64 + interrupt 8; Bluetooth
interrupt 64 + double-buffered bulk 2 × 2 × 64; serial (IR, Sub-GHz, 1-Wire, RFID) double-buffered
bulk 2 × 2 × 64 + 16. Every mode fits with the double buffering that gives full throughput, and
none of it is SRAM.

**Question 2: where does each mode's logic run?**

| Option | Resident RAM while the mode runs | Can it be swapped? | Cost |
|---|---|---|---|
| A. A user daemon per mode | 2–4 KB (stack, data, libc data) | No: a process waiting on a radio device is not swappable (R12.3), and modes run for hours | Simple to write |
| **B. Kernel fast path** (state machine in the device task, formats in `const` tables, data straight between the chip FIFO / pulse ring and the PMA) | 16–64 B of state, in one `union` for all modes | nothing to swap | Kernel flash ≈ 3–5 KB per mode (free, rule 0.1); USB input parsed in the kernel must follow I2 and be fuzzed (AC below) |
| C. Hybrid: B for the data path, a user helper **started on demand** for rare complex commands, exiting after 10 s idle | as B, plus the helper only while used | yes, and it exits anyway | Best where a mode has a long tail of rarely used commands |

Recommendation per mode:

| Mode | Choice | Resident RAM | How the RAM is avoided |
|---|---|---|---|
| RFID and iButton keyboard wedge | B | ≤ 32 B | ID → keystrokes from a `const` layout table straight into the HID IN buffer in the PMA |
| RFID ASCII serial, 1-Wire DS2480B serial | B | ≤ 32 B | line formatting and the DS2480B command state machine in the device task; 1-Wire slot timing is there already (R6.7) |
| IR and Sub-GHz pulse streams | B | ≤ 48 B + a 512 B pulse ring **lent by FS** | while the mode runs, FS lends one buffer-cache block to the kernel (`svrctl` loan, returned when the mode ends): the cache is one block smaller, net new RAM 0 |
| NFC reader (CCID) | C | ≤ 64 B; helper ≈ 4 KB only during MIFARE Classic pseudo-APDUs | CCID framing and ISO 14443-4 APDUs in the kernel: bytes go PMA → ST25R3916's 512 B FIFO by SPI DMA and back, no SRAM copy of the APDU; MIFARE Classic authentication (Crypto-1, in software as stock does) runs in the helper |
| Bluetooth dongle (HCI) | B | ≤ 32 B | HCI packets copied between the IPCC buffers CPU2 already uses in SRAM2 and the PMA |

Total for any one mode: **kernel ≤ +64 B static, user 0 B**, against 2–4 KB per mode with daemons.
The `usbmode` app only configures the mode and exits. Kernel flash for all modes ≈ +15 KB, which
must fit the 192 KB system region (re-checked by `ramcheck`'s flash line at Phase 11).

**Question 3: nRF24 dongle mice (optional, not confirmed).**

Many "wireless" (not Bluetooth) mice use nRF24-family chips: 2.4 GHz GFSK at 250 kbps–2 Mbps with
the Enhanced ShockBurst packet format. Options:

| Option | Extra hardware | RAM | Notes | Verdict |
|---|---|---|---|---|
| nRF24L01+ module on the GPIO header (SPI on pins 2, 3, 5; CSN 4; CE 6; IRQ on 7) | ≈ 1–2 € module | kernel ≤ 48 B; packets wait in the chip's own 3 × 32 B RX FIFO and go straight into the HID IN buffer in the PMA (rule 0.7) | Works for mice whose protocol is unencrypted and documented. To replace the mouse's own receiver, the Flipper needs the mouse's address: learned once by a pairing or scan step for the owner's own device, then stored in `/ext/apps_data/nrfmouse/` | **Recommended if wanted** |
| The WB55's own radio through ST's BLE LLD stack (raw 2.4 GHz packets) | none | similar | The PHY is close (GFSK 1/2 Mbps) but the packet framing differs (preamble, address length, CRC). Unverified, and it means replacing the BLE stack on CPU2, losing Bluetooth | Research item only |
| Not supporting them | none | 0 | The 16.1 bridge covers BLE mice | Default until the owner confirms |

Scope rule: only devices the owner owns, and only unencrypted mouse traffic; encrypted keyboards
(e.g. AES-protected keystrokes) are out of scope.

---

## H. GPIO sensor modules (owner request, 2026-11-14)

The owner has hobby sensor modules to reuse on the GPIO header: temperature and humidity sensors and an
ultrasonic sonar. They get one small, extensible driver layer and two gadget apps (7b.4).

### H.1 Electrical notes for the Flipper header

- The header logic is **3.3 V**. Power 3.3 V modules from pin 9 (3V3), ground on pin 8, 11 or 18.
- 5 V modules (e.g. HC-SR04) take power from pin 1 (5 V, switched on by the charger's OTG boost,
  R6.1; the app turns it on and off). Their **outputs are 5 V**: put a divider (e.g. 1 kΩ in
  series, 2 kΩ to ground) on every line going into the Flipper. STM32WB pins marked FT in the
  datasheet tolerate 5 V as digital inputs, but the divider costs nothing and protects pins that
  aren't FT and inputs while the Flipper is off. Inputs from 3.3 V parts (HC-SR04P, RCWL-1601) need
  nothing.
- A 3.3 V output drives a 5 V module's TTL input (HC-SR04 trigger) fine.
- Each configuration is checked against the pin's alternate functions in the datasheet (timer
  channel, I2C3, LPUART1, ADC); `sensors.conf` refuses a pin that lacks the function a module
  needs.

### H.2 Modules and how each is read without masking interrupts (rule 0.8)

| Module | Bus | How it is read | Pins (default) |
|---|---|---|---|
| DHT11, DHT22/AM2302 (temperature, humidity) | Single wire, timed pulses | Start pulse (1 ms for DHT22, 18 ms for DHT11) by a timer in one-pulse mode, not a busy wait; the 40-bit answer captured by the **pulse engine** (R4b.7: timer input capture, both edges, DMA of ≈ 84 timestamps); the kernel decodes widths (> 50 µs = 1) and checks the checksum. No interrupt masking at all | a pin with a timer input-capture channel (e.g. pin 2, 3 or 5; checked per H.1) |
| DS18B20 (temperature) | 1-Wire | 1-Wire over **LPUART1 in half-duplex mode with DMA** (reset at 9600 baud, bits at 115200 baud, the standard UART-1-Wire method): the UART makes the slot timing, so no masked slots (unlike R6.7's bit-banged iButton master) | pin 15 (LPUART1 TX, open drain) with a 4.7 kΩ pull-up to 3V3 |
| SHT3x/SHT4x, AHT20, HTU21D, BME280/BMP280 (temperature, humidity, pressure) | I2C | Hardware **I2C3 with DMA**, 100 or 400 kHz; conversions waited for with the ms timer (R4b.2) | pins 15 (SCL) and 16 (SDA), pull-ups on most modules already |
| HC-SR04, HC-SR04P, RCWL-1601 (ultrasonic distance) | Trigger + echo pulse | 10 µs trigger by a timer in one-pulse mode; echo width (150 µs–25 ms) by **timer input capture**; distance = width × speed of sound / 2, with speed of sound from a temperature sensor if one is configured (331.3 + 0.606 × °C m/s) | trigger on any pin, echo on a timer-capture pin (divider if 5 V) |

I2C3 and LPUART1 share pins 15 and 16, so one configuration uses either I2C sensors or the
UART 1-Wire bus there, not both; a DS18B20 can instead use the iButton 1-Wire master (R6.7) on
another pin if both are needed.

### H.3 Driver structure (extensible)

- **One kernel device task part, `sensor`,** with a `const` table of module types
  (`{name, bus, start, decode}`); adding a module type = one table entry plus its decode
  function in flash. No resident user daemon.
- **Configuration** in `/etc/sensors.conf` (root): one line per sensor: name, type, pins or I2C
  address, options. `sensorctl reload` applies it. Names become device nodes:
  `/dev/sensor/<name>` (e.g. `/dev/sensor/room`, `/dev/sensor/sonar`), group `sensors`.
- **Reading:** `read()` starts a measurement and returns one line of text
  (`t=22.4 h=41.2 p=1012.8\n`, `d=0.734\n`; SI units, fixed format) so `cat`, shell scripts and
  apps all work; `ioctl(SENSOR_RAW)` gives a binary struct. Minimum interval per module type
  is enforced (DHT22 2 s, DHT11 1 s, sonar 60 ms) by returning the cached last value.
- **Buffers:** the DHT edge capture (≈ 84 × 2 B) and I2C transfers use DMA into the caller's
  buffer (R4b.5; `libapp` passes a 192 B scratch area), so the kernel holds only state.
- More module types later (suggestions): PIR motion (HC-SR501, edge), light (BH1750, I2C),
  CO₂ (SCD40 on I2C, MH-Z19 on UART), time-of-flight distance (VL53L0X/VL53L1X, I2C, a more
  precise rangefinder than ultrasound), soil moisture and analog gas sensors (ADC, R6.4;
  MQ-series heaters draw about 150 mA from the 5 V pin, so they are battery-hungry).

---

## I. Time, location and the first-boot wizard (owner request, 2026-11-14)

The owner asked for three things that belong together: the system clock is kept in **UTC**, it
must not break in **2038**, and the first start for the user runs a **wizard** (hostname, region,
date and time, location, today's sunrise and sunset). Location plus an accurate clock is also the
groundwork for a future **celestial ("old-style") navigation** app (I.6). The time model (I.1–I.3)
changes the ABI, so it is done early, in Phase 2d. The wizard needs almost every other part, so it
is done last, in Phase 18b.

### I.1 Time model

- **One time scale inside the system: UTC**, as POSIX seconds since 1970-01-01 00:00:00 UTC
  (leap seconds not counted, as POSIX). The RTC calendar holds UTC. Files, timers that name an
  instant, logs and the RPC/NTP sync all use UTC.
- **Local time exists only for people.** The time zone is a POSIX `TZ` string (for example
  `CET-1CEST,M3.5.0,M10.5.0/3`), which the existing ACK libc already parses (`src/lib/ansi/misc.c`,
  with `M` rules for daylight saving). No tz database on the device: a `const` table of about 80
  common zones (label → `TZ` string, ≈ 3 KB) lives in the installed region and is used only by the
  wizard and the settings app.
- **Time from the phone is local time.** The stock mobile app sends the phone's local wall time
  (stock keeps local time in its RTC); `rpcd` converts it to UTC with `/etc/TZ` before `stime()`,
  and does not step the clock when the phone's zone differs from ours (R15.7).
- **Wall-clock timers** (`daily` in F.2) are local times and follow the `TZ` rules; `once` timers
  are stored as a UTC instant. On a spring-forward day a timer that falls into the missing hour
  fires at the first instant after it; on a fall-back day it fires once, at the first occurrence.
- **The RTC is the wall clock.** `time()` reads the RTC hardware (BCD calendar, with the shadow
  registers bypassed and read twice for consistency) and converts it with a branch-free
  days-from-civil routine. The kernel no longer keeps `boot_time` (−4 B) and the wall clock stays
  right across Stop2 and tickless idle, whatever the tick count did (rule 0.7).

### I.2 Year 2038: the options compared

Today `time_t` is a signed 32-bit `long` (`include/sys/types.h`, `include/time.h`), so the clock
breaks on 2038-01-19 03:14:08 UTC. The on-disk MINIX V2 inode (64 B) has three 32-bit times and no
spare bytes. The owner suggested shifting the epoch or widening the type. Five ways were compared:

| Option | Range | RAM | ABI / formats | Verdict |
|---|---|---|---|---|
| A. Keep signed 32-bit | to 2038 | 0 | none | breaks in 12 years |
| B. Shift the epoch (signed 32-bit from e.g. 2000) | 1932–2068 | 0 | every boundary converts: tar, `mkfsimg.py`, host tools, RPC, NTP, stock `uint32_t` timestamps; one missed conversion is a silent 30-year error | buys only 30 years, high bug risk |
| C. Unsigned 32-bit `time_t` | 1970–2106 | 0 | same size, but the type changes sign: negative differences, `t < 0` checks and ported libraries (lwIP, mbedTLS, Berry) that assume a signed `time_t` | workable, but the semantic change leaks into ported code |
| D. 64-bit everywhere, disk included | ±292·10⁹ years | FS +480 B (40 in-core inodes × 12 B), larger inode on disk | new FS version, `mkfs`, `fsck`, `mkfsimg.py` | too expensive in RAM |
| **E. Hybrid (chosen)** | programs: ±292·10⁹ years; stored times: 1970–2106, extensible later | **kernel −4 B, FS 0, MM 0**; `struct stat` +16 B on the stack only | one ABI break now (all binaries are rebuilt from source anyway); no message-size change | **recommended** |

**Decision (recommended, Phase 2d): option E.**
- **Every interface is 64-bit:** `time_t` is a signed 64-bit integer in libc, in `struct stat`,
  `struct utimbuf`, `time()`, `stime()`, and later `struct timespec`/`timeval`, Berry integers and
  `libfuri`. Programs compiled now never need another ABI change.
- **Storage stays 32-bit where RAM or disk layout is at stake,** read as **unsigned**: MFS
  inode times on disk and in the in-core inode table, MM's `mp_ctime` (only an identity tag), the
  `(dev, ino, ctime)` records of the installed region (R2b). Valid from 1970 to 2106-02-07.
  Conversion happens at the boundary (`stat`, `utime`) and nowhere else.
- **Messages keep their size.** A 64-bit value travels as two 32-bit halves (`m2_l1` low,
  `m2_l2` high). A 64-bit member in the message union is forbidden: it would make `message`
  8-byte aligned and could grow every message buffer in the kernel and in every process.
- **The owner's epoch-shift idea is kept for later, on disk only:** if 2106 ever matters, the
  superblock gets a `time base` field (the on-disk superblock has unused bytes) and inode times are
  read as `base + uint32`. That is an FS-internal change; no program sees it. Not scheduled.
- Cost on the Cortex-M4: 64-bit add, subtract and compare are two instructions; division (only in
  `gmtime`/`mktime`, and when converting the RTC calendar) uses the 64-bit helper of libgcc, about
  0.5–1 KB once in the shared libc, run rarely. Responsiveness is unaffected.

### I.3 Other clocks that wrap

- **Uptime ticks.** `realtime` (`src/kernel/clock.c`) is a signed 32-bit `clock_t` at `HZ`=60: it
  overflows (undefined behaviour) after 414 days and wraps after 828. A desk clock on a charger
  reaches that. Fix at 0 B: make it unsigned and compare deadlines wrap-safely
  (`(long)(deadline − now) <= 0`) in the clock task, `p_alarm` and `next_alarm`.
- **RTC calendar.** The STM32WB55 RTC keeps a two-digit BCD year (00–99) and treats every year
  divisible by 4 as a leap year (RM0434). The century goes into an **RTC backup register**
  (`RTC_BKPxR`, battery domain, survives power-off and reset; 0 B RAM), together with the last
  year seen, so a century rollover is detected even if it happened while the device was off. On
  2100-02-29 (a date the hardware invents) the driver rewrites the date to 2100-03-01. If RM0434
  shows a binary counter mode (`BIN` bits of newer STM32 RTCs) on this part, the driver SHOULD use
  it instead of the BCD calendar.
- **Backup registers are shared with the stock firmware.** Stock `furi_hal_rtc` uses the low
  registers (header, flags, fault data, PIN data). MINIX takes its register from the top
  (`RTC_BKP19R` first) and keeps a register map table in PORTING.md.
- **NTP** (Phase 19) counts seconds from 1900 in 32 bits and wraps in 2036 (era 1); the SNTP
  client picks the era closest to the current RTC time (or the build date, if the RTC is unset).
- **Stock timestamps** are `uint32_t` (`furi_hal_rtc_get_timestamp`); `libfuri` keeps that type for
  source compatibility, which matches the 1970–2106 storage range.

### I.4 Location, region and hostname data

| File | Content | Owner, mode | Read by |
|---|---|---|---|
| `/etc/hostname` | one line, `[a-z0-9-]`, 1–8 characters (the stock name limit; the BLE name becomes `Flipper <name>`, as stock) | root, 0644 | shell prompt, `bled` advertising name, USB product string, RPC device info |
| `/etc/flipper/region` | radio region code (R8.2) | root, 0644 | the Sub-GHz transmit check (R8.2), NFC/RFID power limits |
| `/etc/TZ` | POSIX `TZ` string, and the zone label in a `#` comment | root, 0644 | INIT puts it into the environment of everything it starts; re-read on `SIGHUP` and `SIGRTC` |
| `/etc/location` | FlipperFormat: `Latitude`, `Longitude` (decimal degrees, 4 decimals), `Elevation` (m), `Source` (`manual`, `city`, `sun`, `gps`), `Accuracy_km`, `Set` (UTC time) | root, **0640, group `location`** | apps whose manifest says `Location: yes` (granted group `location`) |

- **Location is private data.** It is not readable by `nobody`, not sent over RPC or BLE unless the
  caller is root, and not written to logs.
- **Region** default: the hardware region in OTP (as stock `furi_hal_version_get_hw_region`).
  Changing it requires root and is logged to `/var/log/messages`.
- An optional GPS module on the header UART (section H) can later fill both the location
  (`Source: gps`) and the time.

### I.5 Sun computation (`libastro`)

A small shared library in the installed region (≈ 4 KB, 0 B resident), used by the wizard, the
clock apps, and the future navigation app (I.6):
- sun position for a UTC instant: declination, equation of time, Greenwich hour angle;
- **sunrise and sunset** for a date and position, plus civil and nautical twilight (nautical
  twilight is when a navigator takes star sights), and day length; polar day and polar night are
  reported as such, not as an error;
- standard refraction at the horizon (−0.833°), and the dip correction from `Elevation`;
- **the inverse, position from the sun:** from an observed sunrise and/or sunset (UTC):
  - both times: longitude from local apparent noon (the midpoint, corrected by the equation of
    time), latitude from the day length (solved by bisection);
  - one time and a known latitude: longitude; one time and a known longitude: latitude;
  - accuracy: 1 min of timing error ≈ 0.25° (15 arcmin) of longitude; the latitude from day length
    is ill-conditioned within about 3 weeks of an equinox (the day is ≈ 12 h everywhere), where
    the library reports "latitude undetermined" and the wizard asks for it instead;
- arithmetic in `double` (soft-float; the M4 FPU is single precision). Times are split into
  integer days since J2000 plus a fraction, so no precision is lost (a Julian date in `float` is
  only good to ≈ 6 h). One sunrise/sunset computation must take ≤ 20 ms on the device.
- Formulas are reimplemented from published astronomy (Meeus, *Astronomical Algorithms*; NOAA
  solar calculator); no third-party code is copied.

### I.6 Groundwork for celestial navigation (future app, not scheduled)

The owner may later add an app for navigation "the old way": a sextant sight plus an accurate
clock gives a position line. What it will need, and where this spec already provides it:
- **Accurate UTC.** 4 s of clock error is 1 arcminute of longitude, which is 1 nautical mile at the
  equator. The LSE drifts about ±20 ppm (≈ 2 s a day), so:
  - every clock sync (RPC from the phone, NTP, or a manual check against a time signal in the
    clock settings) appends the measured offset to `/var/db/chrono` (UTC time, offset in ms);
  - from that log the system computes the **daily rate**, as navigators do with a marine
    chronometer, and corrects it in hardware with RTC smooth calibration (`RTC_CALR`, about
    0.95 ppm steps). 0 B RAM; the log is a few lines on the SD card;
  - the clock app shows the estimated current error ("±n s since the last sync").
- **A dead-reckoning start:** `/etc/location` (I.4).
- **An almanac in flash:** `libastro` grows from the sun (I.5) to the moon, the four navigational
  planets and the 57 navigational stars plus Polaris (a `const` table, ≈ 1 KB); Greenwich hour
  angle and declination for any UTC second.
- **Sight reduction:** computed altitude and azimuth (intercept method), altitude corrections
  (dip, refraction, semi-diameter, parallax), running fixes.
- **UT1 versus UTC** (DUT1, under 0.9 s, ≤ 0.23 arcmin) is ignored and documented.

### I.7 Notes for the system manual

The final deliverable includes a complete manual. These facts go into it:
- The clock is kept in UTC; local time comes from `/etc/TZ`. How to change the zone (settings
  app, `setup tz`) and what happens to alarms on daylight-saving days (I.1).
- Times are 64-bit for programs; files store times up to 2106-02-07 (I.2).
- After switching from the stock firmware, the RTC still holds the stock firmware's **local**
  time; the wizard asks to confirm it (Phase 18b). After switching back to stock, the stock
  firmware shows UTC until it is set again.
- What the region means (which bands may transmit) and that changing it needs root.
- Location: what it is used for, who can read it, and the three ways to set it, including the
  sunrise/sunset method and its limits near the equinoxes.
- Re-running the wizard or a single step: `setup`, `setup <step>`, Settings → System → Setup.
- Clock accuracy, the daily rate and calibration (I.6).
- Syncing the clock from the Flipper mobile app (R15.7): the app sends local time, MINIX stores
  UTC; offsets of 2 s or less are only logged; a phone in another zone gives a notice instead of a
  clock change; what the app can and cannot do with MINIX.

---

## J. Versioning and releases (owner decision, 2026-11-14)

The system has changed enough to be its own system, so it gets its own name and version. MINIX
2.0.0 stays visible as the base.

**J.1 Name and version number.**
- The system is **Galtzagorriak**. Versions follow semantic versioning, `MAJOR.MINOR.PATCH`.
- **0.x until feature parity.** The minor number goes up by one each time a phase or a lettered
  sub-phase (2a, 2b, 7b, …) is complete, that is, all its acceptance criteria pass and its PRs
  are merged. The minor number is a counter, not the phase number: phases may finish out of
  order (e.g. 2a, 2c, 2d before 2b), and each one adds one.
- The patch number goes up for a release with fixes and tasks only (e.g. T2–T5).
- **1.0.0** is released when Phase 18 is complete: feature parity with the stock firmware, with
  the rule 0.8 budgets met.
- After 1.0: major for an ABI break (J.3), minor for new features, patch for fixes.
- **Starting point: 0.1.0 = Phase 1 complete** (R1.1–R1.7, AC1.R; tagged on main after this
  section was merged). Earlier history (milestones 0–3, flash text cache, PIC programs, the
  hardware PRs) is summed up in `CHANGELOG.md` under 0.1.0; no older tags are added.

**J.2 Where the version shows.**
- `include/minix/gzversion.h` (new, hand-edited) is the **only** place the numbers are written:
  `GZ_NAME "Galtzagorriak"`, `GZ_MAJOR`, `GZ_MINOR`, `GZ_PATCH`, `GZ_RELEASE "0.1.0"`, `GZ_ABI 1`,
  `GZ_BASE "MINIX 2.0.0"`. `OS_RELEASE "2.0"` / `OS_VERSION "0"` in `<minix/config.h>` keep their
  meaning (the MINIX base) so old code that reads them still builds.
- **Build id.** The build writes `gzbuild.h` (in the build tree, not in git) with `GZ_BUILD`:
  empty for a build exactly at a release tag, otherwise `+N.gHASH` from `git describe`
  (`-dirty` added for uncommitted changes), e.g. `0.1.0+7.g1a2b3c4`. The file is rewritten only
  when its contents change, so a build does not recompile everything.
- `uname()` (libc) returns:

  | Field | Value | Note |
  |---|---|---|
  | sysname | `Galtzagorriak` | 13 of 15 chars |
  | release | `0.1.0` | `GZ_RELEASE`, ≤ 11 chars |
  | version | `M2.0.0` | the MINIX base, ≤ 7 chars |
  | machine / arch | `flipper` / `armv7e-m` | unchanged |

  So `uname -a` shows both the system and its base: `Galtzagorriak flipper 0.1.0 M2.0.0 flipper armv7e-m`.
  `src/test/test3.c` (now `#if 0`) accepts `Galtzagorriak` too.
- **Boot banner** (tty task, USB console and LCD console):
  `Galtzagorriak 0.1.0+7.g1a2b3c4 (based on MINIX 2.0.0), ABI 1`, then the original
  Prentice-Hall copyright line for the MINIX code.
- **`ver`** (new, base package, ~1 KB flash): prints name, release with build id, build date,
  base, ABI, and the hardware revision and CPU2 stack version once Phase 6/15 can read them. The
  Settings → About screen (Phase 7) shows the same lines.
- **RAM: 0 B.** All strings are `const` in flash (kernel, libc text). `struct utsname` belongs to
  the caller, as now. No file on the card or in flash is written to hold the version.

**J.3 ABI number.**
- `GZ_ABI` is a small integer for **binary compatibility of programs and packages** with the
  kernel, MM, FS and the shared libc (Phase 2b). It is not the release number.
- It goes up when an already built program would break: changed system-call numbers or message
  layouts, changed sizes or layouts of shared types (`time_t`, `struct stat`, `struct
  sigcontext`), a changed executable format or relocation rules, or a changed shared-libc entry
  table. It does not go up for new calls, new devices or new files.
- **ABI 1** is today's main. **Phase 2d (64-bit `time_t`, R2d.1) raises it to 2.** Phase 2b (shared
  libc) raises it again if its entry table is not frozen in the same release as 2d; try to land
  both in one ABI step.
- **Executables:** `elf2mnx.py` writes `GZ_ABI` into the a.out header field `a_version`
  ("version stamp, not used at present", 16 bits, so no format change). MM's exec checks it:
  `a_version == 0` counts as ABI 1 (programs built before this change); any other value that is
  not the running ABI fails with `ENOEXEC`, before any memory is allocated. Cost: a few
  instructions in MM, 0 B RAM, no extra read (the header is already read).
- **Packages:** `mkpkg.py` writes `Abi: N` into `CONTROL/control`. `pkg install` refuses a
  package whose `Abi:` is not the running one (missing = 1) with the message
  `pkg: NAME needs ABI N, this system is ABI M`. Packages with no programs (data, scripts,
  Berry apps) write `Abi: any`.
- A program knows the ABI it was built for from `GZ_ABI`; the running one is printed by `ver`.
  No new system call.

**J.4 Releases, tags and changelog.**
- A release is a commit on main that changes `gzversion.h` and adds the `CHANGELOG.md` section,
  in one PR named `release: X.Y.Z`. After merge, the review side tags the merge commit
  **`vX.Y.Z`** (annotated tag, message = the changelog section). The base package's
  `BASE_VER` stays its own update counter; its control file gets `Os: X.Y.Z`.
- `CHANGELOG.md` (repo root), newest first. One section per release: date, phases or tasks
  completed, user-visible changes, ABI change (if any), RAM and flash change, and the
  latency numbers that moved (from PORTING.md). It is written for the user, in short lines,
  because it feeds the final system manual (one chapter of release notes).
- Every feature PR adds one line under `## Unreleased` at the top of `CHANGELOG.md`. The release
  PR turns `Unreleased` into the version section.
- `make BOARD=flipper versioncheck` (run together with `ramcheck`): fails if `GZ_MAJOR`/`GZ_MINOR`/
  `GZ_PATCH`, `GZ_RELEASE` and the newest released `CHANGELOG.md` section disagree.
- Flash writes: none at run time. A release changes only what `make base-flash` /
  `pkg install --flash` already write at install time (standing rule on flash writes, `next-tasks.md`).

For the manual: how to read the version (`uname -a`, `ver`, banner, Settings → About), what the
ABI number means for installing packages, and where the release notes are.

---

## K. Pipes, structured text and GUI modules (owner discussion, 2026-11-14)

**K.1 Pipes stay byte streams (decided).** PowerShell-style object pipes were analysed and
rejected. PowerShell's pipeline is function calls inside one .NET process; objects are serialized
anyway at every process boundary. Here every pipeline stage is its own MPU-isolated process, so
object pipes would cost a type runtime and an object heap in every stage (≈ 2–8 KB RAM per stage,
estimate), GC pauses on every stage (rule 0.8), and POSIX compatibility of `sh` and the ~75
commands. What is kept from the idea:
- **Structured text.** Commands that print records (sensor readings, radio scans, `ls`, `ps`,
  logs, app data) get a flag **`-t`**: tab-separated values with one header line. Field names
  carry type and unit: `_hz`, `_dbm`, `_ms`, `_c` (°C), `time` = UTC `time_t`. Escapes inside
  fields: `\t`, `\n`, `\\`. 0 B RAM (streamed, one line on the stack); one shared helper in libc
  (≈ 0.5 KB flash, R2b.9). Rule for every new command and app from now on; existing commands get
  it when they are touched.
- **Objects in Berry only**, inside one process (one heap, no serialization): a `pipe` module
  (`where`, `sortby`, `head`, `map`) and a `tsv` module (`read`/`write`) at the edges. Part of 7c.
- **JSON Lines only for export** to a phone or PC (`--json` where an export exists), never as an
  internal format.
- Not now, revisit only on demand: record-boundary pipes (packet mode, as Linux `O_DIRECT` pipes)
  and CBOR sequences for large binary data.

**K.2 Desktop and modules.** The GUI is one base app (the "desktop": status bar, home-screen
tiles, notification list, app menu), extended by modules.
- **Modules may be written in C, Berry or shell.** All talk to the desktop with the same
  line protocol (K.3), so any language that can write a line works. **Modules whose logic has
  settled are moved to C** (less RAM, no GC pauses); Berry and shell are for new, rarely run or
  user-written modules. Owner rule, 2026-11-14.
- **One process per module**, own uid and groups (A.3). Responsiveness wins over RAM: a GC pause
  or bug in one module never stalls the desktop or other modules. Expected load: at most ~4
  modules at once (≈ 16–24 KB if all are Berry; much less in C or with K.4).
- **The desktop owns the screen and does navigation locally** (scrolling menus, moving focus).
  Modules only get events such as "item 2 selected", so button → pixel (L1) does not depend on a
  module being scheduled, swapped in or between GC cycles.
- Complex apps may still take the whole screen as today (`libfuri`, Berry `gui` binding).
- **Title bar convention (owner decision, 2026-11-16).** Every screen of the launcher, the
  desktop, the apps and the module windows starts with the title bar the launcher and its apps
  use today: a black bar across the full width of the top text row with the screen's name in
  white, centred, in capitals (`gui_title()` in `flipper/gui/gui.c`). `libfuri` views, the Berry
  `gui` binding and desktop `window` records draw the same bar, so the look stays one across C,
  Berry and shell. Exceptions: the text console, full-screen content where a bar would hide
  data (e.g. a picture viewer), and `alert` screens, which may invert the whole screen to stand out.
- **Screen style: "Pine meets modern UX" (owner decision, 2026-11-17).** Every GUI screen of the
  system (launcher, apps, desktop windows, C, `libfuri`, Berry or shell) follows the launcher's
  look of R5.6a.7, so a user who knows one screen knows them all: text first, every screen says
  what its buttons do, nothing hidden, modern spacing. The parts:
  - **Title bar** (above), optionally with a short state on the right: position `3/12` in a
    list, `REC`, `12%`, at most 6 characters.
  - **Hint line**: a 1-pixel rule at y = 56 and one line of button hints at y = 58 in Pine's
    `key action` style (`<Back  OK Run  >More`, `OK Toggle  Back Done`). It names every button
    that does something on that screen, and only those. Long Back is not listed (it always ends
    or parks the app, R4b.8, R12.20).
  - **Selection**: a full-width inverted bar; `>` at x = 1 in lists of things to start or open.
  - **Values** of settings in brackets, right-aligned or after the name: `Icons   [on]`.
  - **Scrolling**: a 1-pixel scroll mark at x = 127 when a list or text is longer than the screen.
  - **Message line**: a short notice replaces the hint line for 2 s, then the hints come back; the
    screen behind stays drawn. Whole-screen notices are only for errors that end the app and for
    `alert`.
  - **Icons** (R5.6a): at most 8×6 pixels, two characters of the 4×6 font, before a list entry.
  - Exceptions, as for the title bar: the text console, full-screen content (pictures, graphs,
    the map), `alert`, and games or ported stock apps whose own screens would be cut. A ported
    stock app keeps its layout; its stock `elements_button_left/center/right` hints are already
    the same idea and stay as they are.
  - **Free for app authors, no RAM:** the parts come from the shared code, as `const` strings and
    stack, so an app writes its hints, not the drawing: `gui.c` for plain `/dev/lcd` programs
    (`gui_title2(title, right)`, `gui_hints(text)`, `gui_note(text)` with a timed key read through
    `poll`, `gui_scroll(top, shown, total)`, `gui_bits8()`), the `libfuri` standard modules
    (Submenu, VariableItemList, Popup, DialogEx, TextInput: hints from their button labels; part
    of the shared library, #201), the Berry `gui` binding (R7c) and the desktop `window` records
    (K.3, a `hints` field). No periodic redraw is added for any of it.
  - Existing apps are aligned in task T22: `info`, `lights`, `power`, `lfrfid` and the menu of
    `console` (`flipper/gui/`), `selftest`, `furihello`, `remind` and `alarm` (7b.1/7b.2), `camfind`
    and `sbctl` when they get their screens. New apps follow it from the start; reviews check it.

**K.3 Desktop protocol (TSV lines).**
- Modules write records to the desktop's FIFO (`/dev/gui` or `/var/run/gui`); one record per
  `write()`, ≤ `PIPE_BUF`, so records from several modules never interleave (POSIX).
- Records (first field = verb, second = module id): `tile`, `status`, `notify`, `window`
  (menu, text, form), `close`. The desktop answers through a per-module reply FIFO with event
  records (`select`, `back`, `input`, `redraw`).
- The desktop keeps a fixed table (≈ 16 tiles × 48 B ≈ 0.8 KB, last 8 notifications ≈ 0.6 KB,
  estimate) in its own data; nothing written to flash.
- Debugging: typing a record on the USB console shows its effect. Reading the desktop FIFO with
  `cat` would steal records, so the desktop gets a logging switch for watching traffic.

**K.4 Alarms never depend on the desktop or a script.** A Berry or shell alarm module only edits
the `timed` spool (F.2). Ringing is the C `alert` program with `FG_PREEMPT`, so an alarm rings
even if the desktop or a module has crashed, and nothing is resident before it fires. Tile
modules that refresh periodically run from `timed` and exit (0 B RAM between refreshes).

**K.5 Default terminal editor: `mined`** (owner decision). Kept from MINIX 2.0, made safe and
VT100-complete by task T8 (`next-tasks.md`). `ed` stays for scripts. A small text editor is
part of the base system so scripts and settings can be edited on the device.

For the manual: the `-t` convention, the desktop/module model and protocol, writing a module in
C, Berry or shell, and the `mined` key table.

## L. Flash writes and default settings (owner request, 2026-11-15)

The owner wants writes to internal flash as rare as possible and asked about a hybrid file
system like Linux overlayfs (read-only layer in flash, changes on the SD card). Analysis:
project report `hybrid-fs-overlay-analysis.md` (Polish, not in the repo). Decision: no overlay in FS. The whole file system,
`/etc` included, is already on the SD card, so a changed setting never writes flash today. An
overlay would add work to the FS core, slow lookups, break the `(dev, ino, ctime)` binding of
installed programs, and make every `pkg install` rewrite shared metadata pages in flash.
Instead:

- **L.1 Rule: no file data in internal flash.** Internal flash holds only the system image,
  the text of installed programs (Phase 2b), the run-time text cache (within R2b.11), and the
  read-only ROM image of L.2, all written at build, DFU or install time. File contents that can
  change at run time (settings, logs, keys, app data, swap) go to the SD card. A PR that breaks
  this needs the owner's sign-off.
- **L.2 Default settings in a ROM image.** The base system's default settings (`/etc` as built
  from `flipper/rootfs`, and the defaults of the base apps) are built on the PC into a small
  read-only MINIX FS image (target ≤ 32 KB) stored in the installed region by `make base-flash`
  (DFU only; `pkg` never writes it).
  - A memory-driver minor (`/dev/rom`, like the early `/dev/ram` root image in PORTING.md) serves
    that flash range read-only as a block device, read in place (XIP) through the FS cache. FS is
    unchanged.
  - `/etc/rc` mounts it read-only on `/rom` first.
  - RAM: 0 B static (one of the existing 8 super block slots, one mount).
- **L.3 Changes go to the SD card, defaults stay in ROM.** Settings are read with a libc helper,
  `FILE *cfopen(const char *path, const char *mode)`:
  - for reading, open `path` (on the SD card); if it does not exist, open `/rom` + `path`;
  - for writing, always the SD card path (create directories as needed);
  - programs that read settings use it: `/etc/rc` (`rc.conf`), the launcher, `sensors.conf`
    readers, `libfuri`'s `/int` mapping (R5.3), Berry's settings binding. Shell scripts use a
    small `cfcat path` command with the same rule;
  - deleting the file on the card means "back to the default". No whiteouts are needed: for
    settings, "missing" correctly means "default". To make a setting empty, write an empty file.
- **L.4 Reset to defaults.** `settings reset [path...]` (and a launcher item, Settings > System >
  Reset settings, with a confirmation) removes the given files, or every file under `/etc` that
  also exists in `/rom/etc`, from the card. `settings diff` lists the files that differ from
  their default.
- **L.5 First boot** (`/etc/setup`, Phase 18b wizard) copies only what must be writable from the
  start (`passwd`, `group`, `utmp`, `mtab`); everything else is read from `/rom` until the user
  changes it. A damaged or replaced card therefore always comes back with working defaults.
- **L.6 Package defaults** live with the package on the SD card (`/usr/lib/<pkg>/defaults`, same
  `cfopen` fallback with that directory instead of `/rom`), so installing a package never writes
  flash for its settings.
- **L.7 Later, not scheduled:** booting without a card. The same `/rom` image, extended with
  program files whose text is in the installed region (bound at build time, since the image's
  `(dev, ino, ctime)` are fixed), could be the root file system when no card is present. Not
  part of this work; it needs flash that R2b.9 has to free first.

Acceptance:
- ACL.1 Fresh card: `/etc/rc.conf` is not on the card, but `cfcat /etc/rc.conf` prints the
  default from `/rom`; the system boots and the launcher works.
- ACL.2 Change a setting in the launcher: the file appears on the card, and the flash erase
  counters (R2b.11) don't change. `settings reset` brings back the default.
- ACL.3 One day of normal use with nothing new installed: 0 internal-flash pages erased
  (`free -v`).

For the manual: where settings live, `cfopen`/`cfcat`, `settings reset/diff`, and the flash
counters in `free -v`.

---

## M. Canaries: what is something trying to do with my Flipper? (owner request, 2026-11-15)

Owner request: "various canaries, Bluetooth too". A **canary** is a gadget app (F.5) that sits on
one interface, lets the other side act, and shows and logs what it tried: a USB port that
enumerates, a phone that connects over Bluetooth and probes services, a reader that powers up an
NFC field in your pocket, a jammer on 433 MHz. The USB canary (11.2) is the first member; this
section sets the rules for all of them.

### M.1 Rules for every canary

- **Listen, or answer only what the protocol needs to keep the other side talking.** A canary never
  types, never accepts writes, never keeps a pairing beyond the session (the BLE honeypot of
  R16.5.6 pairs, with keys in RAM only and no retained bond), never transmits on a radio beyond
  its role (advertising for the BLE bait, the tag answers of the NFC bait). The canary only ever
  observes what comes to it: beyond accepting a pairing it performs no action on the other device,
  never reaches out to pair with one, and never jams, deauthenticates, floods or replays.
- **Never impersonate a real product or credential.** Baits use our own IDs (G.1), a random NFC
  UID and generic names; no payment, transit or access-card emulation, no other vendor's IDs.
- **The verdict is honest:** "nothing happened during this session" is not "this is safe", and a
  fingerprint is labelled as a guess. The raw log is always kept.
- **RAM only while running.** Each canary is a module of one app `canary` (GUI modules, section K),
  so only the active one is in RAM; common code (event ring, summary, log writer, alert) is a small
  shared library. Hot paths run in the driver (G.3 option B) with event rings lent by the FS cache.
- **Logs on the SD card only:** `/ext/apps_data/canary/<kind>/YYYYMMDD-HHMMSS.tsv` (TSV, K), one
  header line (kind, persona, verdict, guess) and one line per event. **No internal flash writes**
  (rule L). Rules tables (host and tracker signatures, AIDs) are `const`, plus an optional
  `rules.tsv` on SD.
- **Pocket mode** for canaries that are useful unattended (NFC field, Sub-GHz jammer, BLE
  trackers): the app parks (12d) and the alert service (F.2) vibrates on a trigger. Where the chip
  can watch on its own (ST25R3916 field wake-up, CC1101 carrier sense on a GDO interrupt) the
  canary uses tier 3 ("runs in the radio chip or USB driver"), so the CPU sleeps between events.
- **Privacy:** logs hold other people's addresses and names; they stay on the card, and the app has
  "delete all canary logs". Addresses can be stored hashed (setting).
- **Responsiveness:** an event costs the driver ≤ 5 µs in its interrupt; L1 holds under a flood.

### M.2 The family

| Canary (module) | Interface and role | What it shows | Needs | Phase |
|---|---|---|---|---|
| `usb` | USB device, passive personas | power only / enumerates / configures / uses; descriptor and class requests; host guess | 10, 11.1 | **11.2** |
| `ble` | BLE peripheral bait + observer | who connects, which GATT services and characteristics they discover, read and write; pairing requests and their capabilities; advertising floods; trackers that follow you | 15; observer parts need the full stack (R16.1.0) | **16.5** |
| `nfc` | 13.56 MHz field detector + tag bait | a reader field appeared (pocket alarm); what the reader asks: anticollision, RATS, SELECT of which AIDs (payment, transit, access) | 13 | **14.2** |
| `rfid` | 125 kHz field detector | an LF reader field, its frequency and polling rhythm | 8 | **9.2** |
| `subghz` | CC1101 receive only | bursts per band (frequency, RSSI, length, decoded protocol if known), new repeating transmitters, **jamming alarm** (continuous high RSSI) | 8 | **9.2** |
| `ir` | IR receiver | remote codes sent around you, decoded protocol | 6 | **9.2** |
| `wifi` (optional) | ESP32 devboard, passive scan (as `camfind`) | deauthentication floods, probe requests, evil-twin SSIDs (same name, new BSSID) | 9.1 firmware | 9.2 (optional) |

Out of scope: canaries that need to transmit to provoke the other side (active probing) and
anything that breaks a link to see the reaction.

---

## N. SD card: power loss and card removal (owner request, 2026-11-16)

The owner asked whether MINIX has journaling. It has none: MINIX FS writes back through the
buffer cache, `update` syncs every 30 s, and nothing records whether a file system was left in a
consistent state. Only internal flash is power-cut safe today (`tools/powercut.py`). **Owner
decision (2026-11-16): no journaling.** Instead a "cleanly unmounted" flag, `fsck` at boot only
after an unclean end with its `-v` output on the Flipper's screen, and a sync before ship mode.
The owner added (2026-11-16): the main threat is **someone pulling the card out while it is in
use**, more than a flat battery. N.6 covers that.

What can end the system without a sync, and what each case needs:

| Event | Today | After this section |
|---|---|---|
| `halt`, power menu, automatic power-off (R7.1) | MM kills, FS `SYNC`, ship mode | same, and every file system is marked clean (N.1) |
| Low battery (R6.1 `SIGPWR`, R7.1 SYSDWN) | `SIGPWR` to INIT, which ignores it | INIT does an orderly halt (N.5) |
| Battery cut by its protection, brownout, Left+Back reset, watchdog, panic | unclean, never checked | unclean only if a write was in flight in the last ~30–60 s (N.2); then `fsck` at the next boot (N.3) |
| Card pulled out | FS drops the buffers that fail to write, the system runs on with a missing root | writes stop, dirty buffers stay in RAM, "Put the card back" on the screen; the same card resumes with no loss (N.6) |

### N.1 Two flag bits in the super block (0 B RAM)

- The 16-bit `s_pad` field of the V2 super block (offset 18) becomes `s_flags`. Bit 0 **CLEAN**:
  nothing was written since the file system was last consistent. Bit 1 **CHECK**: it was found
  unclean and has not been checked since. These are the bits Linux's `minix` driver already uses
  in the same field (`MINIX_VALID_FS` = 1, `MINIX_ERROR_FS` = 2) and bit 0 is MINIX 3's
  `MFSFLAG_CLEAN`, so util-linux `fsck.minix` on a PC reads and sets them the same way. Old
  readers ignore the field.
- `mkfs`, `flipper/tools/mkfsimg.py` and `/etc/setup` write CLEAN = 1, CHECK = 0.
- The FS never keeps its own copy of the on-disk bits: it changes them by read-modify-write of
  block 1 through the buffer cache, written through at once. `fsck` writes the same block
  through the block device, which goes through the same cache, so the two never disagree.
  In-core state lives in spare bits of the in-core `s_pad` (bit 15 "written since the last
  sync", bit 14 "CLEAN on disk"); these bits are masked out of every disk write. FS static
  RAM: **+0 B**.
- **Mount** (also the root at boot), read-write: if CLEAN is 0, set CHECK = 1 on disk at once
  (one 1 KB SD write, only after an unclean end). Read-only mounts write nothing.
- **First write** after the flag is CLEAN on disk: before any other block of that device goes
  to the card, write CLEAN = 0 synchronously. The hook is the one write path in `cache.c`
  (`rw_block` and `rw_scattered`, WRITING), so the order is guaranteed without touching the
  callers.

### N.2 Clean again when idle

- In `do_sync` (called by `update` every 30 s, and by `sync`): a read-write file system whose
  flag is not CLEAN on disk is marked CLEAN when (a) nothing was written to it since the
  previous sync, (b) no buffer or in-core inode of it is dirty, and (c) no in-core inode of it
  has `i_nlinks == 0` while it is on disk (an unlinked file still open; pipes are never on disk,
  T9). So a file system becomes clean 30–60 s after its last write burst.
- Cost: two 1 KB SD writes per write burst on that file system (CLEAN off, CLEAN on), none while
  idle. A program that writes the card often (e.g. a statistics logger) should batch its writes
  (every 15 min, or on clean shutdown) rather than writing each sample; the manual says so.
- Effect: most of the time the card is clean. A power loss, reset or card removal while the
  Flipper is idle needs no check at all.
- **No atime writes on read** (needed for N.2 and N.3, and it saves SD writes on every `exec`):
  a read sets `ATIME` in the in-core inode but no longer marks it dirty
  (`src/fs/read.c`); the access time reaches the card only when the inode is written for
  another reason. `stat` still shows it while the inode is in core.
- **Orderly shutdown.** MM's `do_reboot` already sends `SYNC` after killing every process. It
  now sends `SYNC` with a "shutdown" flag: FS flushes and marks every read-write file system
  CLEAN unconditionally (CHECK stays as it is on disk). The SD driver already waits for the
  card's busy signal after each write (`sd_wait_ready`), so when `SYNC` returns the data is in
  the card. `hal_power_off` (ship mode) is only reached through `wreboot(RBT_HALT)`, after it.
- The idle ladder's "SD card power off" step (R7.1) runs only after a sync with nothing dirty,
  i.e. when the card is CLEAN.

### N.3 `fsck` at boot, only when needed, on the screen

- `fsck` (V2, `src/commands/simple/fsck.c`) is built, goes into the base flash image (it runs
  before `/usr` is mounted; text XIP from flash) and gets new options:
  - `-p` (preen): read the super block; if CLEAN = 1 and CHECK = 0, print nothing and exit 0
    (one block read, ≈ 2 ms). Otherwise check and repair automatically (as `-a`).
  - `-v`: verbose for a 32×10 LCD console: one header line (`fsck /dev/hd1 /: unclean`, plus
    the last reset cause from `hal_reset_cause`, e.g. `(watchdog)`), the current phase with a
    percentage updated in place at least once a second, one short line per repair, and a summary
    (`fixed 3, left 0, 4.1 s`). Lines ≤ 32 characters.
  - Exit status as BSD: 0 clean, 1 repaired, 2 root repaired (reboot now), 4 errors left,
    8 could not check (no memory, read error). The existing `-a -r -l -s -c -i -z` stay.
  - After a successful check: CHECK = 0; on an unmounted file system also CLEAN = 1.
- `/etc/rc`, before anything else writes the card (before `printroot >/etc/mtab`):
  1. `fsck -p -v /dev/hd1` (root, mounted). Exit 2: show the summary until a key or 10 s, then
     `reboot` at once (BSD practice: the in-core FS state may be older than the repaired disk).
     N.2's "no atime writes" keeps the in-core inodes clean at that point, so the reboot's sync
     cannot write stale inodes back.
  2. `fsck -p -v` on `/usr` and `/home` **before** mounting them (unmounted: no cache issues).
  3. Exit 4 or 8 on `/usr` or `/home`: mount it read-only (`mount -r`) and say so; the launcher
     shows a notice "Card has errors: run fsck" until the next clean check.
- The output goes to the console, which is the LCD while the launcher has not started (boot
  messages are already shown there). If anything was repaired the summary stays on screen until a
  key or 5 s. The `/usr` and `/home` output is also appended to `/usr/adm/fsck.log` (kept under
  16 KB) once `/usr` is mounted read-write. A root repair is shown on screen only: writing a log
  to the root just before the reboot is what the reboot avoids.
- **Skip:** holding Back during a `/usr` or `/home` check stops it; that file system is mounted
  read-only with CHECK still set, so the next boot checks it again. The root check cannot be
  skipped (it is small, N.4).
- First boot after updating to this version: existing cards have `s_flags` = 0, so each file
  system is checked once. CHANGELOG and manual say so.

### N.4 `fsck` memory: bounded for any card below 4 GB

Stock `fsck` keeps five bit maps and a link-count byte per inode in RAM. On the 3.8 GB card
laid out by `/etc/setup` (1 KB blocks, 1 KB zones):

| File system | Zones | Inodes | Stock `fsck` needs | After N.4 |
|---|---|---|---|---|
| `/` (`/dev/hd1`, 32 MB) | 32 K | ≤ 8 K | ≈ 19 KB | ≈ 12 KB, 1 pass |
| `/usr` (`/dev/hd2`, 2 GB) | 2 M | 65 535 (`INODE_MAX`) | ≈ 600 KB (2 × 256 KB zone maps, 64 KB counts) | ≤ 64 KB, 4 zone passes |
| `/home` (rest, ≈ 1.5–1.9 GB) | 1.5–1.9 M | 65 535 | ≈ 450–550 KB | ≤ 64 KB, 3–4 zone passes |

About 106 KB is free at boot (nothing else runs before the launcher), so the stock program
cannot check `/usr` or `/home` at all. Changes, all in `fsck`, no format change:
- The `spec_imap`/`spec_zmap` maps are allocated only when `-i`/`-z` is given. The disk maps are
  compared one 1 KB block at a time (`chkmap` streams), not loaded whole.
- The link-count array covers only inodes up to the highest one in use in the disk inode map
  (inodes are allocated from the bottom: a card with 5 000 files needs ≈ 5 KB). If that does not
  fit, the tree walk is repeated per inode range.
- **Zone windows.** The computed zone map is a window of W zones (W from the memory left, at
  most `fsck_mem` in `/etc/rc.conf`, default 64 KB, minimum 8 KB). Pass 1 walks the tree as
  today (directories, link counts, inode map) and marks zones inside window 0; it also records
  per used inode one byte with the windows its zones touch. Each later pass reads only the
  inodes, indirect and double-indirect blocks that touch its window, marks the zones, finds
  duplicates, and compares with that slice of the disk map. Typical cost of the extra passes:
  one more read of the used inodes and indirect blocks in total, since a file's zones are
  mostly in one window.
- Optional, for `/etc/setup` on new cards: `mkfs -i 16384` for `/usr` (packages are few files)
  cuts its inode table, map and count array by 4×.
- `chmem` of `fsck`: the window size plus ≈ 24 KB; it runs at boot when the RAM is free.
- A PC fallback for a bad case: util-linux `fsck.minix -av /dev/sdX2` checks the same file
  systems on a computer (the manual shows how).

Time targets (SD ≈ 300 KB/s single-block, more with multi-block, R4b.9): root ≤ 3 s; a card
with ≤ 5 000 files and ≤ 1 GB used ≤ 30 s for `/usr` and `/home` together; nothing at all on a
clean card (three super block reads). Progress at least once a second (N.3).

### N.5 Low battery and other power-off paths

- INIT handles `SIGPWR` (R6.1) like the power menu's power off: `SIGTERM` to all processes,
  up to 2 s grace, then `reboot(RBT_HALT)` (N.2 marks the card clean, then ship mode). The
  whole path takes ≤ 3 s; the low-battery threshold (R7.1, default SYSDWN) must leave at least
  10 s of battery under full load.
- Every software power-off (power menu, `halt`, automatic power-off, low battery) goes through
  `reboot(RBT_HALT)`; no code calls `hal_power_off` another way.
- Hardware events cannot sync: Left+Back reset, the battery's own protection, watchdog, panic.
  N.2 makes them harmless when idle and N.3 repairs the rest. The manual says that Left+Back
  does not save.

### N.6 Card pulled out while in use (owner, 2026-11-16)

The card has a card-detect switch on PC10 (configured in `hal_disk_init`, never read today). Its
EXTI line 10 belongs to the Up button (PB10), so it is **polled**: before every SD command, and
in the clock task each tick while the card is selected or a buffer is dirty (one GPIO read, 0 B
RAM). A socket's detect switch opens while the contacts may still touch, but the timing is not
guaranteed: the write in flight can be lost or torn (1 KB block = two 512 B sectors). The aim is
therefore: **nothing else is lost, and nothing is written to the wrong card**.

- **R-N6.1 Freeze.** When the line says "no card", or a transfer fails and the line then says
  "no card", the SD driver returns a distinct error (`ENXIO`) for every request until the card
  is back. FS treats `ENXIO` on write as "keep the buffer dirty", not as today's "Unrecoverable
  write error" which drops it (`cache.c`), and does not count it as a write for N.2. Reads fail
  with `EIO` to the program. `update` keeps trying every 30 s at no cost.
- **R-N6.2 Tell the user.** The kernel sets a "card out" state; the launcher (or the console if
  no launcher) shows "SD card removed: put it back" with a short vibration. Apps running from
  flash keep running; programs that need a file wait or fail with `EIO`.
- **R-N6.3 Same card only.** When the card comes back the driver initialises it and compares its
  CID (manufacturer, serial) and the root super block with the copies taken at boot (16 B CID +
  a 4 B CRC of block 1 of each mounted file system, in the existing driver state where possible;
  RAM ≤ 32 B kernel). Same card: rewrite the dirty buffers (the torn block too, since it stayed
  dirty), clear the state, carry on: **no loss**. A different card or no match: never write
  to it; the screen says "Different card: put the original back or power off". Power off then
  ends without the sync for that device and the next boot checks the original card (N.3).
- **R-N6.4 Never returned.** The user can power off from the screen; the original card has CLEAN
  set if it was idle (N.2), else it is checked at its next boot.
- **R-N6.5 Swap.** R12.25 already turns swap off on card removal; with N.6 it also waits for the
  same card before turning swap back on.

### N.7 Flash and RAM

- Internal flash: **0 run-time writes**. Everything here writes the SD card only; `fsck`'s
  text goes into the base image at build time (≈ 20 KB, checked by the base image size).
- RAM: FS +0 B (spare `s_pad` bits), MM +0 B (a flag in the existing `SYNC` message), kernel
  ≤ +32 B for N.6 (CID and super block CRCs), INIT ≤ +16 B for `SIGPWR`. `fsck` uses RAM only
  while it runs, at boot.

Acceptance:
- ACN.1 (QEMU) Kill QEMU with no sync while `cp` writes `/home`: the next boot prints the `fsck`
  lines on the console, repairs, the boot after that runs no check; files synced before the kill
  are intact.
- ACN.2 (QEMU) Kill QEMU 90 s after the last write: the next boot runs no check (N.2).
- ACN.3 (QEMU) `fsck -v` on a full-size 2 GB image (sparse file) with 5 000 files, damaged with
  `tools/fsdamage.py` style edits (zone marked free but used, duplicate zone, wrong link count,
  orphan inode): every error repaired, peak memory ≤ `fsck_mem` + 24 KB, a second run clean.
  Same image through util-linux `fsck.minix -n` on the host: clean.
- ACN.4 (QEMU) Root damaged: `fsck` exits 2, `rc` reboots, the second boot is clean.
- ACN.5 (Flipper) Pull the card while `cp` writes a 1 MB file and put it back within 10 s: the
  message shows, then the copy completes and `cmp` matches; no `fsck` at the next boot after a
  clean halt. Pull it and insert another card: nothing is written to the other card.
- ACN.6 (Flipper) Unclean boot on the 3.8 GB card with ~5 000 files: the `fsck -v` lines are
  readable on the LCD, progress moves at least once a second, total ≤ 30 s; Back skips `/home`
  and it is mounted read-only.
- ACN.7 (Flipper) Low battery (simulated threshold): orderly halt, the next boot runs no check.
- ACN.8 Normal use for a day: internal-flash erase counters unchanged (ACL.3); SD writes for the
  flags ≤ 2 per write burst.

For the manual: what CLEAN means, why a check may run after a reset or a pulled card, what the
screen shows and how to skip, `fsck_mem`, `/usr/adm/fsck.log`, checking the card on a PC with
`fsck.minix`, "put the card back" and "different card", Left+Back does not save, batch your
writes.

---

## Roadmap: phase order

Phases are ordered by what the next one needs, and by the stock features they unlock. Old numbers
(Revision 1) in brackets.

| Phase | Content | Unlocks |
|---|---|---|
| 1 | Close the local-root holes [1] | everything |
| 2 | Memory foundation: 2a static RAM [4], 2b flash-installed programs and shared libc [4b], 2c vfork [6]; 2d time ABI: UTC and 64-bit `time_t`, before Phase 3 (section I) | room for apps; a clock past 2038 |
| 3 | Accounts, packages, randomness [2] | multi-user, safe installs |
| 4 | Device platform: 4a Cortex-M4 hardening [3], 4b driver platform (new) | every driver |
| 5 | App framework `libfuri` and porting toolchain (new) | app ports |
| 6 | Basic hardware: power, RTC, sound, vibro, GPIO, IR, iButton, power modes (new) | Phase 7 |
| 7 | App ports I: archive, settings, power, IR, iButton, GPIO | |
| 7b | Time services and first native apps: interval reminder, alarm clock, Self Test (section F); GPIO sensors: thermo-hygrometer, sonar (7b.4, section H); more native apps get 7b.N subsections | apps that act at a set time |
| 7c | Scripting runtime: Berry (owner decision) with bindings to sensors, timers, alerts, GPIO and radios | script apps |
| 8 | Sub-GHz and LF RFID drivers | |
| 9 | App ports II: Sub-GHz, 125 kHz RFID; gadget: hidden camera finder v1 (Sub-GHz, IR meter, 9.1; its Wi-Fi devboard part is in Phase 22) | |
| 10 | USB composite classes: HID, mass storage, second CDC | BadUSB, 19a |
| 11 | App ports III: BadUSB, USB mass storage; USB reader modes for RFID, iButton, IR, Sub-GHz (11.1, section G) | |
| 12 | Swapping to the SD card [5] | RAM headroom for NFC/BLE |
| 13 | NFC driver | |
| 14 | App ports IV: NFC; USB NFC reader, CCID/PC-SC (14.1) | |
| 15 | Bluetooth LE and RPC (CPU2, `bled`, `rpcd`) | mobile app, 19c |
| 16 | App ports V: BLE remote, BLE serial, mobile app; native: Bluetooth adapter (BLE mouse/keyboard → USB, 16.1); Bluetooth dongle mode for the PC (16.2); camera finder v2 with BLE scan (16.3) | |
| 17 | Trust: U2F, signed packages [9], updater, option bytes (deferred item) | |
| 18 | App ports VI: U2F, catalog, porting guide, CI | parity |
| 18b | First-boot wizard: hostname, region, date/time and zone, location, today's sunrise and sunset (section I) | out-of-box setup |
| 19 | Networking: lwIP over USB, a Wi-Fi module, BLE and CC1101 (19a can start right after 10) | gadget web UI, time sync |
| 20 | Continuous: 20a speed and power [7], 20b code modernization [8] | |
| 21 | Install MINIX on another Flipper: over BLE by acting as the stock mobile app (21a); SWD rescue mode (21b) | a second Flipper |
| 22 | Wi-Fi / ESP32 devboard work, deferred to the end (owner decision 2026-11-15): `camfind`'s devboard firmware (R9.1.1, R9.1.8), the `wifi` canary (R9.2.4), ESP32 offload mode (R19.7) | Wi-Fi scans |

Phases 1–4 are the foundation and run in order (2a, 2c, 2d and 4a can overlap; 2d must be merged
before Phase 3). From Phase 5 on,
a driver phase and the app phase before it can overlap.

---

## Phase 1: Close the local-root holes

**Goal:** no unprivileged process can gain kernel privilege or halt the system.
**Depends on:** nothing. **Do first.**

Requirements:
- **R1.1 [S1]** `do_sigreturn` MUST reject a context whose `sc_sp` is not 4-byte aligned or for
  which `[sc_sp − 32, sc_sp)` is not inside the process's D..S range. This applies to both the
  normal and the `SC_NOREGLOCALS` path. Return `EFAULT`.
- **R1.2 [A1]** `restart` and `SAVE` in `mpx.S` MUST access the frame of an unprivileged
  process with `STRT`/`LDRT`, so a bad PSP faults instead of touching kernel memory. A fault in
  this path MUST kill the process (SIGSEGV, or SIGKILL if delivery is impossible), not panic.
- **R1.3 [S2]** `relocate()` MUST check every write address (`dbase ≤ adr`,
  `adr + 4 ≤ dbase + data_bytes` for PIC; the text+data range for the old format) and 4-byte
  alignment. Any failure, or `relocate()` returning −1, MUST kill the new image.
- **R1.4 [S12]** The run-time flash text cache MUST NOT let one user's file supply the text
  that runs for another user's file.
  - The cache key MUST include the **owner uid of the program file** (from `stat`), in addition
    to CRC and size. `fc_lookup()` only matches records with the same owner as the file being
    executed. This needs a new record format: 32 B records (four double words: crc, size |
    addr, seq | owner, flags | dead), bump `FC_MAGIC`; old directories are reformatted at boot.
  - MM MUST only cache files that no one but their owner (and root) can write: not group- or
    other-writable. Other files run with text in RAM.
  - Why this is enough: a forged CRC can now only alias text among files of one owner, who
    controls all of them anyway. Root-owned programs can only be aliased by root. No read-back
    comparison is needed, so exec stays as fast as it is now.
  - Programs in the flash-installed region (Phase 2b) are looked up there first and never go
    through the run-time cache.
- **R1.5 [S6]** `mini_send` and `mini_rec` MUST return `EFAULT` for a message pointer that is
  not 4-byte aligned.
- **R1.6 [S3]** `mpu_switch` MUST NOT fall back to whole-RAM access.
  - MM SHOULD refuse (ENOMEM) any exec or brk layout that `mpu_cover()` cannot express.
  - If one still reaches the kernel, the process MUST be killed.
- **R1.7** Add `make ramcheck` and `flipper/ram-budget.txt` (rule 0.3).

Acceptance criteria:
- AC1.1 `tests/sigret`: a forged sigcontext with `sc_sp` pointing into kernel RAM, and one with an
  unaligned `sc_sp`. Both return EFAULT or kill only the test; `proc[]` is unchanged (checksum
  before and after via `/dev/kmem` as root), and the system keeps running.
- AC1.2 `tests/badreloc`: executables with first offset and deltas pointing outside the image or
  unaligned. Exec fails or the process dies; the kernel and other processes are unaffected.
- AC1.3 `tests/fcpoison`: a `nobody`-owned binary claiming the CRC and size of `/bin/su`, run as
  `nobody` after `su` was evicted. Then `su` still runs the real `su` (check with a known output),
  and `free` shows two separate cache entries. A root-owned, mode-666 test binary is not cached at all.
- AC1.4 `tests/unaligned`: `_sendrec` with `&m + 1` returns EFAULT; no panic.
- AC1.5 `tests/mpufrag`: chmem/brk sizes chosen to need more than 7 regions. Exec or brk is refused,
  or the process is killed; it never gets other processes' memory.
- AC1.6 The m2 self-test passes in QEMU, and login, the launcher and `pkg get` work on the Flipper.

Responsiveness: AC1.R `latcheck` and `lat` exist (R0.8e); baseline L1, L2, L4, L6, L7, L10 recorded. No later phase may make them worse than this baseline without a note in PORTING.md.

RAM budget: **0 B** for all images and per process. Flash: about +0.5 KB (MM checks). The cache
directory holds 127 instead of 170 records per page.

---

## Phase 2: Memory foundation

**Goal:** free RAM and flash for everything that follows. 2a and 2c can run in parallel; 2b needs Phase 1 (R1.4).

### 2a. Recover static RAM

**Goal:** about −5 KB of kernel and server RAM with no change in behaviour.
**Depends on:** none (can run in parallel with Phases 2 and 3).

Requirements:
- **R2a.1 [M3]** Make these `const`:
  - `__ctype` and `tasktab`;
  - the driver tables (`w_dtab`, `m_dtab`) and `k_environ`;
  - the printk tables;
  - MM `call_vec`, FS `call_vector` and `dmap`.
  Replace `_sys_errlist` with one string plus a `const unsigned short` offset table (no pointers),
  keeping the `strerror`/`perror` API.
- **R2a.2 [M4]** `irq_table` becomes a build-time `const` table, or a small index table.
- **R2a.3 [M5]** Driver buffers:
  - SD transfers go directly to and from the caller's memory; `w_buf` is removed, or kept only
    if Phase 12 needs it, decided there. One shared 512 B sector buffer MAY stay for
    unaligned and partial transfers;
  - `tmp_buf` is not a separate 512 B array;
  - `/dev/lcd` writes stream from the caller (no `fbuf`); `/dev/lcd` **read is kept** (screen
    mirroring over USB/BLE, R15.4) and returns the last writer's buffer, which R4b.8 keeps
    pinned for the foreground process;
  - console input reads the USB ring directly; `ibuf` is only for the UART, at ≤ 64 B.
  - `p_physio` MUST be set around any direct transfer into a process that could be shadowed.
- **R2a.5 Command bss diet** (measured on the scratch build): the largest per-process `.bss` is
  in `sed` 10 KB, `fgrep` 8.9 KB, `cmp` 8.3 KB, `cat` 8.3 KB (4 KB input + 4 KB output buffer),
  `ls` 8 KB and the launcher 6.7 KB. Use 512 B–1 KB buffers (one SD sector is the natural unit),
  `malloc` large tables on demand, and drop the double buffer in `cat`. Target: no base command
  above 4 KB of `.bss` except where a POSIX limit needs it (document those).
- **R2a.4 [M14]** Stub `atexit` in server builds (no `__functab`); `print_buf` in MM and FS removed or shrunk.

Acceptance criteria:
- AC2a.1 `make ramcheck` shows at least −4.5 KB total versus the Phase 0 baseline.
- AC2a.2 Device: free memory after login rises by the same amount, within ±256 B.
- AC2a.3 m2 self-test, `pkg get` of a 100 KB package, `ls -l /bin | wc`, the launcher and the Lights
  app all work. A 1 MB `dd` between files gives an identical `cksum`.

Responsiveness: AC2a.R L1 is not worse than the Phase 1 baseline now that `/dev/lcd` streams from the caller.

RAM budget: **at least −4.5 KB** (target −5 KB). Per process: 0, or −260 to −520 B for programs using
ctype or strerror, and 4–8 KB less for the commands in R2a.5.

### 2b. Flash-installed programs

**Goal:** the programs that make up the device (base system, launcher, apps from packages) are
written into internal flash **once, at install time**, and run in place from there. The
run-time cache from `5e3d0f0` stays as the fallback for everything else.
**Depends on:** Phase 1 (R1.4). For installing from packages at run time: Phase 3 (pkg fixes).
Package signatures (Phase 17, R17.2) become mandatory for this path once they exist.

### Flash layout (STM32WB55RG, 1 MB, 4 KB pages, 10,000 erase cycles per page)

| Region | Address | Size | Written when |
|--------|---------|------|--------------|
| System image (kernel, MM, FS, INIT) | 0x0800_0000 | **192 KB** (was 128 KB; drivers of Phases 4b–15 need ≈ +60 KB) | DFU only |
| **Installed programs** (new) | 0x0803_0000 | from there up to the cache; grows with `INST_SIZE` | firmware build (DFU), or `pkg install` as root |
| Run-time text cache | after the installed region | 128 KB default (`CACHE_SIZE`) | exec (as now) |
| Guard | `SFSA − 16 pages` to `SFSA` | 64 KB | never |
| CPU2 wireless stack + FUS | from `SFSA` to 0x080F_FFFF | set by ST's FUS (≈ 200 KB with the BLE light stack stock uses) | never by MINIX, except an updater stack swap (R17.3) |

With the BLE light stack the flash between 0x0803_0000 and the guard is about 520 KB. That has to
hold the base userland (436 KB of text measured, mostly duplicate libc) plus the device apps
(≈ 310 KB, section B), so R2b.9 is not optional: without a shared libc the apps don't fit.

Requirements:
- **R2b.1** The board code MUST read the CPU2 secure start (`SFSA` in `FLASH->SFR`, in 4 KB pages)
  at boot, as the stock firmware does (owner confirmed 2026-11-14). It MUST refuse to use any
  page at or above `SFSA − 16 pages`, so FUS has room below the stack for its own upgrade process
  (check AN5185 for the exact space a stack upgrade needs). Print `SFSA` once at boot. The
  installed region and the cache are laid out from this value at boot, not at build time.
  Placement is tiered: base system and launcher first (lowest addresses), then installed
  packages, then the run-time cache at the top.
- **R2b.2** `hal_flash_region()` returns two regions (installed, cache). The existing
  `SYS_FLASH` call takes a region argument:
  - **cache** writes stay MM-only, as now;
  - **installed** writes are only accepted while MM is handling an install request from a
    process with effective uid 0 (an MM `svrctl`, e.g. `MMINSTALL`).
- **R2b.3** Installed region format:
  - two directory pages (ping-pong, append-only records with a dead marker, the same scheme as
    `fcache.c`), then text pages;
  - one record per program: text address, size, SHA-256 of the text (32 B), and the binding
    `(dev, ino, ctime)` of the program file it was installed for;
  - records are never written by exec.
- **R2b.4** Exec lookup order for a PIC program:
  1. installed region, matching the file's `(dev, ino, ctime)`: use it, skip the text in the
     file (`lseek`), with no CRC or hash at exec time;
  2. the run-time cache (R1.4 rules);
  3. RAM.
  Safety argument: the installed region only ever contains text that root installed and that was
  verified at install time (R2b.5). A binding match therefore always runs genuine installed code,
  even if an inode number is later reused. `ctime` changes on every write or chmod, so a modified
  file never matches.
- **R2b.5** Installing:
  - `pkg install --flash` (and a `flash: yes` line in `CONTROL/control`) writes each listed
    program's text to the installed region after:
    (a) the package passed the R3.5/R3.6 checks;
    (b) the program file is installed owned by root or bin and not group/other-writable;
    (c) once R17.2 exists, the package signature verified;
  - MM computes the SHA-256 of the text it read from the file, writes it, then **reads the flash
    back and compares it** before writing the record (one-time cost, at install);
  - upgrade: write the new copy, write its record, mark the old record dead, then erase the old
    pages if no process still runs them (otherwise at the next boot);
  - `pkg remove` marks records dead the same way.
- **R2b.6** Firmware build: `make BOARD=flipper base-flash` builds the base programs into the
  installed region of `minix.bin` (host tool computes the records). The records carry a
  "bind at first boot" flag: `/etc/setup` (first boot) fills in `(dev, ino, ctime)` for the files
  it finds in `/bin` with the same SHA-256 (hashing at first boot only). DFU flashing needs
  physical access, so no signature is needed on this path.
- **R2b.7** Compaction:
  - only for entries no process is using (the `mproc` scan already in `fcache.c`);
  - triggered by `pkg` when an install doesn't fit, never by exec.
- **R2b.9 Shared libc in flash.** One copy of libc (and later `libfuri`, D.2) in the installed
  region, used by every program in place:
  - the library is built PIC and installed at a fixed flash address; programs are linked against
    its export table and reach it through their GOT (FDPIC-style; `r9` already holds the GOT
    base), so library code is shared while each process keeps its own library data in its data
    segment;
  - the library's writable data (errno, stdio buffers, `malloc` state) is per process and is
    allocated by exec like any other `.data`/`.bss`;
  - a program whose library version (hash in its header) doesn't match the installed one falls
    back to a static build, or is refused with a clear message by `pkg`;
  - expected effect: base userland text from 436 KB to about 200 KB, and ≈ 20 KB less per
    ported app.
- **R2b.10 CPU2 stack probe at boot** (cheap, done here so the local agent can confirm the
  stack on the real device long before Phase 15):
  - after reading `SFSA` (R2b.1), set `PWR->CR4.C2BOOT`, enable IPCC, and wait (≤ 500 ms) for
    CPU2's "system ready" event;
  - read the device information table that CPU2 fills in the shared tables in SRAM2a
    (`MB_RefTable` → `DeviceInfoTable`): FUS version, wireless stack version, the stack type
    (`InfoStack`: BLE full, light, HCI-only, Thread, Zigbee, concurrent …) and its memory sizes;
    the ready event's payload also says whether CPU2 came up in FUS or in the stack;
  - print one line, e.g. `cpu2: FUS 1.2.0, stack BLE light 1.17.3, SFSA 0x080Cx000`, and
    record the owner's values in PORTING.md;
  - then hold CPU2 idle (no BLE traffic) until Phase 15. The shared tables live in SRAM2a, which
    the stock layout already reserves for them, so the probe costs no SRAM1 and no new RAM;
  - if no ready event arrives, print `cpu2: no response` and continue booting.
  - AC2b.8 On the owner's Flipper, boot prints the FUS and stack versions and the stack type,
    and they match what the stock firmware's "Settings > About" shows.
- **R2b.8** `free -v` (or `pkg list -f`) shows the installed region's use, the cache's use, and
  `SFSA`.
- **R2b.11 Run-time cache write budget** (owner request 2026-11-15, section L). On top of the
  admission rule from T2 (a program is copied only after 3 runs from RAM):
  - MM MUST NOT erase more than `FC_BUDGET` run-time cache pages per 24 h (default 32, the size
    of the 128 KB cache, i.e. at most about one cycle per page per day: > 25 years at 10,000
    cycles). When the budget is spent, programs keep running with their text in RAM; nothing
    else changes;
  - the counter and the start of its 24 h window SHOULD live in one free RTC backup register
    (battery domain, 0 B RAM, survives a reset so a reboot loop can't reset the budget; take the
    register from the map in PORTING.md, I.3). If no register is free: 4 B in MM, reset at boot;
  - a total erase counter for the cache and one for the installed region (a backup register or
    the directory header's generation word, whichever costs no RAM), shown by `free -v` (R2b.8)
    as `flash: cache N pages erased (M today, budget B), installed K`;
  - root can change `FC_BUDGET` with `svrctl` (`fcache budget 0` turns run-time caching off);
    `/etc/rc` sets it from `fcache_budget=` in `rc.conf`.
- **R2b.12 Rebind by content.** When a file no longer matches its record's `(dev, ino, ctime)`
  (the card was restored from a copy, `/bin` was re-copied, `tar x` over the base system),
  `pkg bind` MUST compare the file's text SHA-256 with the live records and, on a match, write a
  new record with the new binding and mark the old one dead, instead of leaving exec to copy the
  same text into the run-time cache. Cost: one hash per such program, only when `pkg bind` runs;
  one 64 B record (no page erase unless the directory is full). `/etc/rc` keeps calling
  `pkg bind /bin/*` at boot; files whose binding already matches are skipped without hashing.

Acceptance (R2b.11, R2b.12):
- AC2b.9 With `fcache_budget=2`, running five different uncached programs three times each erases
  at most 2 cache pages that day (`free -v`); the rest run from RAM; after a reboot the count is
  still 2 (backup register).
- AC2b.10 `tar cf` of `/bin`, `rm`, `tar xf` back, reboot: `pkg bind` rebinds every base program
  by hash, the run-time cache stays empty after a login plus `ls -l /bin` (as AC2b.1), and the
  installed region's directory gets only new records (no text pages written).

Erase-cycle budget:
- Installing or upgrading a program erases its own pages once (a 20 KB program: 5 pages, one cycle
  each), and a directory page once per ~120 records.
- At 10,000 cycles per page, a program can be reinstalled thousands of times before its pages wear
  out.
- The installed region is never erased by normal use, only by `pkg` and DFU.
- The run-time cache's wear is bounded by R1.4 (only owner-protected files are cached).

Acceptance:
- AC2b.7 With R2b.9, `ls -l /bin` shows each base program's text 10–20 KB smaller than at
  `5e3d0f0`, and `free -v` shows the shared libc once.
- AC2b.1 A firmware built with `base-flash`: after first boot, `sh`, `ls` and the launcher run
  with their text in the installed region (`free -v`), and the run-time cache stays empty after a
  login plus `ls -l /bin`.
- AC2b.2 `pkg install --flash hello_1.0.tar`, then `hello` runs from the installed region. After a
  reboot it still does. After `pkg remove hello` the record is dead and the pages are reused by
  the next install.
- AC2b.3 Replacing `/bin/ls` with a different file (new ctime) makes exec fall back to the cache or
  RAM, never to the old installed text.
- AC2b.4 As `nobody`, no way exists to write to the installed region (`MMINSTALL` returns EPERM).
- AC2b.5 Cutting power during an install (simulated in QEMU by killing it at random points) leaves
  either the old or the new version working, never a half-written record in use.
- AC2b.6 Boot prints `SFSA`; a debug build that sets the installed region to overlap
  `SFSA − 16 pages` refuses to start the region and says why.

Responsiveness:
- AC2b.R1 L6 ≤ 300 ms for `sh`, `ls` and the launcher from the installed region.
- AC2b.R2 During `pkg install --flash` of a 50 KB program, L1 stays within budget except single
  freezes ≤ 25 ms (L9); exec of an uncached program never waits for a flash write (R0.8c).

RAM budget:
- MM +0 B: the directories are read in place from flash (XIP); the page bitmap for the installed
  region (64 pages → 8 B) replaces part of the existing cache bitmap.
- Per process: 0, and the same RAM saving as the cache, without ever falling back to RAM because
  the cache is full.
- Flash: system 192 KB; installed region and cache share the rest below the guard, split by
  `CACHE_SIZE` (128 KB default).

### 2c. vfork for fork+exec

**Goal:** no image copy for fork followed by exec.
**Depends on:** Phase 1.

- **R2c.1 [M1]** MM `VFORK` call:
  - the child runs in the parent's memory and on its stack, and the parent stays blocked until
    the child's `exec` or `_exit`;
  - no shadow buffer is allocated.
- **R2c.2** libc `vfork()` (assembly stub: it must not return through a frame the child destroys).
  Use it in the simple-command path of `sh`, and in `init`, `getty`, `launcher` and `pkg`'s `run()`.
- **R2c.3** The child between `vfork` and `exec` may only call `exec*`, `_exit`, `dup2`, `close`,
  `chdir`, `setuid`/`setgid` and `signal`. Document this in the man page and audit the call sites.

Acceptance:
- AC2c.1 `ls -l /bin | sort | wc` (the known failure in PORTING.md) works on the Flipper.
- AC2c.2 Peak memory of `sh -c 'ls; ls; ls'` (via `free -t`) drops by at least the shell's image size.
- AC2c.3 m2 self-test and the fork tests pass.

Responsiveness: AC2c.R `sh -c ls` start-to-first-output time drops by at least the time of one image copy (record it).

RAM budget: **0 B static**. Saves one image copy per fork+exec.

### 2d. Time ABI: UTC and a 64-bit `time_t` (section I; owner request 2026-11-14)

**Goal:** a clock that is right after 2038, kept in UTC, decided before any package or port is
built against the old ABI. **Depends on:** Phase 1. **Must be merged before Phase 3** (packages)
and Phase 5 (porting toolchain). Can overlap with 2a and 2c.

- **R2d.1 Types.** `time_t` becomes a signed 64-bit integer (`long long`) in `include/sys/types.h`
  and `include/time.h`. A new `ftime32_t` (`unsigned long`) names the 32-bit stored form (I.2).
  `clock_t` stays 32-bit.
- **R2d.2 System calls without bigger messages.** `time()` returns its value in `m2_l1` (low) and
  `m2_l2` (high); `stime()` takes it the same way. `utime()` keeps its message: libc refuses values
  outside 1970–2106 with `EINVAL` (FS cannot store them) and sends them as `ftime32_t`.
  No 64-bit member is added to the message union (I.2).
- **R2d.3 FS and MM.** Inode times on disk and in core are `ftime32_t`, read as unsigned. `stat`
  and `fstat` widen them into the 64-bit `struct stat`. FS stamps files with the current time
  saturated at `0xFFFFFFFE` (and reports it once on the console), never wrapped. `mp_ctime` in MM
  and the installed-region records stay 32-bit. `flipper/tools/mkfsimg.py` packs times unsigned.
- **R2d.4 libc.** `gmtime`, `localtime`, `mktime`, `ctime`, `asctime`, `strftime`, `difftime`
  work on the 64-bit type; `gmtime` uses a days-from-civil conversion (no year loop) with a
  32-bit fast path. The 64-bit division helper lives once in the shared libc (R2b.9).
- **R2d.5 Kernel.** The wall clock is read from the RTC on each `time()` call (I.1); the kernel's
  `boot_time` is removed. Until the RTC driver of R6.2 exists (and in QEMU), the kernel keeps
  `boot_time` as a 32-bit `ftime32_t` and adds the uptime, so the RAM figure is the same.
- **R2d.6 Uptime wrap.** `realtime` becomes unsigned and every deadline compare is wrap-safe
  (I.3). A debug build option starts `realtime` 10 s before the 32-bit wrap.
- **R2d.7 Audit.** Every place that keeps a time in `int` or `long` (about 10 call sites in
  `src/commands`, plus `ls`, `make`, `date`, `touch`, `find`, `tar`, `pkg`, `ash`) is fixed. A
  script in `flipper/tools/` greps for `long.*time(`, `(long) *time` and `%ld` next to time values
  and runs in `make check`.
- **R2d.8 `date`.** `date` prints UTC with `-u` and local time otherwise; `date -s` takes local
  time, or UTC with `-u` or a trailing `Z`, and always writes UTC to the RTC.
- **R2d.9 Manual notes** for I.7 go into PORTING.md under "Time" in the same PR.

Acceptance:
- AC2d.1 With the clock set to 2038-01-19 03:13:58 UTC: after 20 s `date -u` shows 03:14:18;
  `touch f; ls -l f` shows 2038; `make` rebuilds a target whose source is newer; `sleep 5` and
  `alarm()` behave normally across the second.
- AC2d.2 `touch -t 210601010000 f` works; `touch -t 210701010000 f` fails with `EINVAL`.
- AC2d.3 The uptime-wrap debug build runs `sleep 30` and a pending `alarm(20)` across the wrap
  correctly.
- AC2d.4 `make INIT=m2 minix minix-run` passes; a file system image made before this phase mounts
  and shows the same times.
- AC2d.5 `TZ=CET-1CEST,M3.5.0,M10.5.0/3 date` around 2026-03-29 00:59:59 UTC shows 01:59:59 then
  03:00:00 local.

Responsiveness: AC2d.R `time()` ≤ 5 µs (one RTC read and conversion); `ls -l` of 100 files is not
measurably slower than before (record both).

RAM budget: kernel −4 B (no `boot_time`, once R6.2 is in) or 0; FS 0; MM 0; INIT 0; per process
0 static, `struct stat` +16 B on the stack while used. Flash: ≈ +1 KB in the shared libc.

---

## Phase 3: Accounts, packages, randomness

**Goal:** no default open root, no privilege escalation through `pkg`, real randomness.
**Depends on:** Phase 1.

Requirements:
- **R3.1 [A12]** RNG driver for the STM32WB RNG.
  - A kernel helper `hal_rng(u32_t *)` with the CECS/SECS checks.
  - `/dev/random` as a new minor of an existing kernel device (no new task, so no stack).
  - QEMU: a documented weak fallback, clearly marked.
- **R3.2 [S9]** `passwd` MUST take its salt from `/dev/random`.
- **R3.3 [S9]** `pwdauth` MUST sleep 2 s after a failed `##` check. It SHOULD allow `##user`
  lookups only when the real uid is 0 or matches that user.
- **R3.4 [S4]** Passwords:
  - Ship `root` as `##root` with `/etc/shadow` (mode 600, empty hash for the first boot only).
  - `/etc/setup` MUST make the user set a root password at first boot, before the login prompt
    appears on USB.
  - `passwd` MUST write to `/etc/shadow` for `##` entries (it already supports this).
- **R3.5 [S5]** `pkg`:
  - incoming and scratch directories under `/usr/var/pkg/` (root-only);
  - `mkdir` followed by `stat()` requiring uid 0 and mode 0700, else abort;
  - `pkg get` installs only the files received in that run;
  - scratch names use `/dev/random`.
- **R3.6 [S7]** `pkg` input handling:
  - no unbounded `sprintf`/`strcpy` on package data (add a small `snprintf` to libc or check
    lengths);
  - `Package:` names and `Depends:` tokens follow the `good_name()` character rules;
  - archives whose `tar tf` list contains `..` components or absolute paths are rejected
    before extraction;
  - `pkg remove` only unlinks paths that the package's own list recorded at install time.
- **R3.7 [S5]** FS MUST implement the sticky bit: in a directory with `S_ISVTX`, `unlink` and
  `rename` require the caller to own the file or the directory, or be root. `/tmp` becomes 1777
  in `proto`.

Acceptance criteria:
- AC3.1 Two consecutive `passwd` runs at the same second give different salts.
- AC3.2 A fresh card requires setting a root password before the first login; `root` with an empty
  password is refused afterwards; `/etc/passwd` contains no hashes.
- AC3.3 `tests/pkgplant` (as `nobody`): pre-creating the old incoming path and planting a tar has
  no effect on a later `pkg get` as root.
- AC3.4 Packages with a 255-char `Depends:` token, a 250-char member path, a `../` member and a
  `Package: ../x` name are each rejected with a message; no crash.
- AC3.5 As `nobody`, `rm /tmp/rootfile` (owned by root, in sticky `/tmp`) fails with EPERM.

RAM budget:
- kernel +0 B (`/dev/random` reuses the memory task);
- per process: 0;
- `pkg`: transient stack only.

---

## Phase 4: Device platform and hardening

### 4a. Kernel hardening with Cortex-M4 features

**Goal:** faults stay visible, memory errors are caught by hardware, hangs recover.
**Depends on:** Phase 1.

Requirements:
- **R4a.1 [A3]** `lock()`/`unlock()` MUST use BASEPRI (`KERNEL_PRIO`) instead of PRIMASK.
  Fault handlers (priority 0) MUST stay live inside locked sections.
- **R4a.2 [A2]** In `mpu_switch`:
  - all user RAM regions get `XN`;
  - a no-access region at `0x00000000` (NULL guard);
  - a 32 B no-access region at the process's current break, maintained on `brk`
    (heap/stack guard).
  - Region budget: flash, NULL guard and heap guard; the rest for the process (R1.6 still applies).
- **R4a.3 [A2]** `arm_flash()` MUST NOT disable the MPU. It opens a privileged-RW region over the
  target page only, for the duration of the operation.
- **R4a.4 [A17]** IWDG with a timeout of about 2 s, refreshed from the clock task. `hal_halt`
  either keeps refreshing (halt) or stops refreshing on purpose (panic reboot); document which.
- **R4a.5 [A15]** Enable flash ECC error interrupts. A double error (NMI) in the text-cache region
  MUST invalidate that cache entry and kill processes using it. Anywhere else: panic with a clear message.
- **R4a.6 [A10]** SHOULD: one DWT comparator watches the guard word of the kernel stack and of the
  current task stack (moved on task switch). A hit reports the task and halts.
- **R4a.7 [S10]** USB `SET_LINE_CODING` MUST check `wLength`.
- **R4a.8 [S8]** FS `read_super()` MUST validate counts against the device size (inodes, zones,
  map blocks, first data zone) and refuse to mount otherwise.

Acceptance criteria:
- AC4a.1 `tests/nullderef` gets SIGSEGV; `tests/execstack` (jump into a stack buffer) gets SIGSEGV;
  `tests/stackheap` (recursion into the heap) gets SIGSEGV before corrupting heap data.
- AC4a.2 A fault forced inside a `lock()` section in a debug build reports MemManage/BusFault, not
  an escalated HardFault.
- AC4a.3 USB console input keeps working while the text cache erases a page (no lost characters
  with `flsend`).
- AC4a.4 A deliberate `for(;;)` in a debug kernel task reboots the device within the watchdog timeout.
- AC4a.5 Mounting a crafted image with a huge inode count fails cleanly.

Responsiveness: AC4a.R L4 ≤ 20 µs measured over the m2 self-test and an SD copy (BASEPRI keeps radio levels live); L3 ≤ 5 µs.

RAM budget: **0 B**. Each process loses 32 B of its own gap to the heap guard; that is not new RAM.

### 4b. Device-driver platform

**Goal:** the shared kernel machinery every feature driver needs, built once.

- **R4b.1 Bus arbitration and DMA.**
  - SPI1 (CC1101, ST25R3916) and SPI2 (LCD, SD) each get an owner field (one byte) taken and
    released by the kernel task using the bus; tasks never block while holding it except on DMA
    completion.
  - SPI transfers ≥ 16 bytes use DMA (DMAMUX request lines from the reference manual).
  - LCD flushes stream from the writer's buffer by DMA (A.6).
- **R4b.2 Millisecond timer.** A one-shot hardware timer (LPTIM1, which keeps running in Stop2)
  multiplexed for kernel drivers and for user alarms with millisecond resolution (`setitimer`
  subset). It replaces busy waits in drivers. RAM: one 8-byte entry per armed user alarm, in the
  existing clock task table.
- **R4b.3 REVIVE priority boost.** A user process revived by a device task goes to the head of the
  user queue. AC: the IR echo test (R6.x) measures wake latency ≤ 100 µs at 64 MHz.
- **R4b.4 Supplementary groups.**
  - Up to 4 per process (`setgroups`, `getgroups`), checked by FS permission tests.
  - `login` and the launcher set them from `/etc/group` and app manifests.
  - RAM: 4 × 2 B × 16 slots in FS `fproc` (128 B) plus 4 in MM if needed.
- **R4b.5 Zero-copy DMA rule.** A driver may point DMA at a user buffer only if:
  - (a) the range passed `numap()` and lies in the process's D..S range;
  - (b) the process is pinned: `p_physio` set, not shadowable, not swappable;
  - (c) every path that frees or moves process memory stops the DMA first: exit, exec, kill,
    `rmshadow`, swap-out (add a hook in `do_xit`/`do_exec`/`rmshadow`);
  - (d) the DMA channel's addresses are reloaded per transfer, never left armed for a different
    process.
  This is the safety condition for A.6. The MPU does not check DMA.
- **R4b.6 `poll()`.**
  - FS gains `poll(fds, n, timeout)` over pipes, ttys and character devices, with a new
    `DEV_SELECT` request to device tasks (TTY, flipdev, and each new driver), and to `udrv`
    drivers such as `inet` (R19.4).
  - RAM: per process one small poll record in FS (≈ 16 B × 16).
- **R4b.7 Pulse engine.** One kernel module for IR, LF RFID and async Sub-GHz:
  - **capture:** timer input capture (both edges) → circular DMA of 16-bit durations straight into
    a user ring registered with `ioctl(PULSE_ARM)` (R4b.5), with half/full-transfer interrupts
    waking the reader; overflow is counted, not silently dropped;
  - **playback:** DMA from a user buffer of durations into the timer's auto-reload/compare
    registers, with an optional carrier (IR 36–40 kHz, LF 125 kHz) on a second channel;
  - only one stream per timer at a time (`EBUSY` otherwise).
  - RAM: ≈ 64 B of state, no kernel buffer.
- **R4b.8 Foreground ownership.**
  - `/dev/lcd` and `/dev/buttons` belong to the foreground app.
  - The launcher hands them over with `ioctl(FG_SET, pid)`. Writes from others are dropped and
    reads block.
  - Long-press Back is delivered as a signal (`SIGTERM`) to the foreground app, with SIGKILL after
    3 s, which is the stock "exit app" behaviour.
  - `/dev/lcd` read returns the last frame from the last writer's buffer (C table), 0 B.
- **R4b.9 SD multi-block + DMA** (old R7.3 pulled forward): `CMD18`/`CMD25`, `CMD59` with the SPI
  hardware CRC16, DMA. One shared 512 B sector buffer (C table).

Acceptance:
- AC4b.1 A test driver streaming captured edges from a function generator into a user ring at
  20 kHz edge rate for 60 s loses no edges (overflow counter 0) while the shell runs `ls -lR /`.
- AC4b.2 Killing a process with an armed capture stops the DMA before its memory is reused
  (debug check: the DMA destination is never outside a live process's range).
- AC4b.3 `poll()` on `/dev/buttons` + a pipe + a 250 ms timeout behaves as POSIX.
- AC4b.4 Two concurrent apps can't both draw: the background one's writes are dropped, and
  long-press Back ends the foreground one.
- AC4b.5 1 MB SD read ≥ 2× the baseline speed.

Responsiveness:
- AC4b.R1 The interactive queue (R0.8a) is in place: with three CPU-bound background processes,
  L1 and L2 stay within budget.
- AC4b.R2 L5 ≤ 100 µs; SD and LCD DMA on SPI2 never delay a button reply by more than one 4 KB
  chunk (R0.8b).

RAM budget: kernel ≤ +256 B; FS ≤ +400 B (groups, poll). Flash: ≈ +10 KB kernel.

---

## Phase 5: App framework (`libfuri`) and porting toolchain

**Goal:** stock apps can be compiled for MINIX with small changes (section D).
**Depends on:** 2b (installed region, shared libc), 4b (poll, ms timer, foreground ownership).

- **R5.1** `libfuri` core:
  - cooperative threads with stacks sized from the app manifest;
  - message queues, event flags, timers on `poll()`;
  - `furi_string`, records;
  - `furi_check` mapped to `abort()` with a message on `/dev/log`.
- **R5.2** `libfuri` GUI: ViewPort/Canvas on the app framebuffer, with fonts as `const` data;
  ViewDispatcher, SceneManager and the standard modules (Submenu, Popup, DialogEx, TextInput,
  ByteInput, VariableItemList, Widget, Loading, FileBrowser).
- **R5.3** Storage mapping (`/ext`, `/int`, `/any`), dialogs (file pick, message), notification
  sequences, FlipperFormat.
- **R5.4** HAL stubs: each `furi_hal_*` function used by ported code maps to the matching device
  `ioctl`. Functions without MINIX support fail loudly (`ENOSYS` + log), never silently.
- **R5.5** `fam2pkg` host tool: `application.fam` → package with `CONTROL` (label, icon, category,
  groups, chmem, license) and the launcher entry. Assets go under `/usr/share/apps/<appid>`.
- **R5.6** Launcher: categories, icons, app manifests from installed packages; runs each app as its
  own uid with its declared groups (R4b.4); shows "app crashed" on abnormal exit.
- **R5.6a Launcher icons** (owner request 2026-11-17, out of milestone order; task T21). The
  launcher can show a small icon before each label in the app list. An icon is **at most two
  characters of the list font**: 8×6 pixels, the cell of two 4×6 glyphs, so a row stays 6 pixels
  high and the list keeps its 9 visible rows. The black title bar stays as it is.
  - **R5.6a.1 Where an icon comes from.** `/etc/apps` gets an optional third field:
    `Label:user:icon:/program arg ...`. The old form `Label:user:/program ...` stays valid: the
    launcher tells them apart by the field after the user, which is the program only if it starts
    with `/` (programs in `/etc/apps` are absolute paths; the manual says so). The icon field is
    one of:
    - empty (`Label:user::/program`): no icon;
    - `@name`: a built-in icon (R5.6a.2), e.g. `@bell`; an unknown name draws no icon;
    - `#` and 12 hex digits: an 8×6 bitmap, six rows top to bottom, one byte per row, the most
      significant bit on the left (the same layout as one row of two font glyphs), e.g. a
      small bell `#183C3C7E0018`;
    - otherwise 1 or 2 printable ASCII characters, drawn in the list font, e.g. `>_`, `Zz`, `!`.
      A text icon cannot contain `:` or start with `/`, `@` or `#` (use the bitmap form).
    A malformed field (bad hex, too long) is treated as no icon; the entry itself still loads.
    For packages the same value goes into an `Icon:` line of `CONTROL/control` (mkpkg.py `-i`);
    until the launcher reads manifests itself (R5.6), the package's `postinst` writes it into
    its `/etc/apps` line. `fam2pkg` (R5.5) makes the `Icon:` from the stock 10×10 `fap_icon` by
    a 2×2-box downscale to 8×6 with a threshold, or takes `fap_icon_mini` / an explicit value
    from the build file.
  - **R5.6a.2 Built-in icons.** A `const` table in the launcher (in the text segment, i.e. flash,
    since the launcher runs from the installed region): about 24 icons, each 6 bytes plus a name
    of at most 8 characters, at most 400 B of text in total and 0 B of RAM. At least: `app`,
    `console`, `info`, `light`, `power`, `battery`, `rfid`, `nfc`, `subghz`, `ir`, `ibutton`,
    `gpio`, `usb`, `ble`, `clock`, `alarm`, `bell`, `timer`, `sensor`, `game`, `music`, `tools`,
    `file`, `settings`. Original drawings (not copies of stock or Unleashed icons), one host tool
    renders them enlarged into a PNG for review. The base `/etc/apps` uses them for its own apps
    (`Console` `@console`, `About` `@info`, `Lights` `@light`, `Power` `@power`, `RFID` `@rfid`)
    and the base `postinst` lines get icons too.
  - **R5.6a.3 Drawing.** Icons on: `>` at x = 1 as today, the icon at x = 6–13, the label from
    x = 16 (23 characters still fit). An entry without an icon gets an empty 8×6 cell, so labels
    stay in one column. The selected row is inverted, icon included. Icons off: the screen is
    **pixel for pixel** what it is today. `gui` gets one helper,
    `gui_bits8(x, y, const unsigned char *rows, int n, int inverse)`, which the text drawing can
    share; text icons are rendered into the 6 bytes when `/etc/apps` is read, so drawing never
    looks at the font for icons.
  - **R5.6a.4 The setting.** `/etc/launcher.conf`, stock `Key: value` style, read with `cfopen()`
    (spec L.3): `Icons: on|off`, default `on` in `/rom/etc/launcher.conf`. In the launcher,
    **Left** opens a one-screen settings page (black title bar `LAUNCHER`, a line `Icons  on`;
    OK or Left/Right toggles and redraws at once; Back returns to the list). The card's file is
    written once, on Back, and only if the value changed; nothing is ever written to internal
    flash. Right stays free for the next page (Running, R12.20; the desktop pages of K.6).
    The settings page is a **page**, not a special mode: the launcher keeps a small `const`
    table of pages (name, row source, OK handler, hint text), the app list and settings are its
    first two entries, and both use the same list drawing, scrolling and key loop, so later
    pages (K.6) are new table entries.
    Page switching (Left to Settings, Right to the next page) acts on the **release of a short
    press**, never on the press, and a key that was part of a chord is ignored, so the backlight
    gesture Left+Up (R6.10) never opens a page. The kernel marks chord keys in the button event
    (#145); the launcher only checks that flag. The launcher reads the setting at
    start, after its settings page, and together with `/etc/apps` when an app exits (one more
    `cfopen` of a ~12-byte file), so a change from the shell (`echo 'Icons: off' >
    /etc/launcher.conf`) shows after the next app.
  - **R5.6a.5 RAM.** Icons cost 6 B of heap per entry that has one, inside the entry's existing
    `malloc` block (no new allocation per entry); `.bss` grows by at most 8 B; the settings page
    uses only stack. Worst case, 16 entries with icons: ≤ 104 B. Nothing is resident besides
    the launcher itself.
  - **R5.6a.6 Speed.** No icon work on the key path: a key press redraws exactly as today plus
    at most 9 × 6 byte copies. Parsing happens only when `/etc/apps` is read.
  - **R5.6a.7 "Pine meets modern UX"** (owner, 2026-11-17; the system-wide screen style of K.2
    starts here). The launcher's look follows the
    Pine mail program: text first, every screen says what each button does, nothing hidden; with
    modern touches: icons, even spacing, a clear selection bar, a quiet scroll mark. Layout on
    128×64 with the 4×6 font:
    - **Title bar** (black, as now): the screen name centred; on the right, the position
      `3/12` of the selected entry (the list only).
    - **List**: 8 rows (y = 7 + 6·i), the selected row a full-width inverted bar with `>`;
      when there are more than 8 entries, a 1-pixel scroll mark at x = 127 whose length and
      place show the visible part (no arrows, no animation).
    - **Hint line**: a 1-pixel rule at y = 56 and one line at y = 58 naming the buttons in
      Pine's `key action` style, e.g. `<Setup  OK Run  >Running` (`>Running` only once R12.20
      exists). Every launcher screen has one: the settings page says `OK Toggle  Back Done`.
    - **Message line**: short notices that today take the whole screen (`unknown user`, `app
      crashed`, the park hint of R12.20) replace the hint line for 2 s instead, so the list stays
      visible; the hint line comes back at the next key or after the 2 s.
    - **Settings page**: one line per setting, value in brackets (`Icons   [on]`,
      `Hints   [on]`); Up/Down choose, OK or Left/Right toggles.
    - `Hints: on|off` in `/etc/launcher.conf` (default `on`): off gives the list its 9th row back
      and drops the title counter. Hints off and icons off = today's screen exactly.
    - No extra RAM beyond R5.6a.5 (the strings are `const`, the counter is formatted on the
      stack); no timer and no redraw when nothing changes (the 2 s message uses a timed key read
      through `poll`, not a periodic tick). Any timed wake of the launcher (this message, a
      later status line) runs only while the launcher is in the foreground and the screen is on;
      otherwise it waits for a key with no timeout.
- **R5.7** (old A9) Opt-in FPU with lazy stacking, as in the earlier proposal: S16–S31 saved on the
  process stack, registers cleared on exec and on handover; `fam2pkg` sets the FP flags for apps
  that use `float`.
- **R5.8** Provenance per D.3: upstream notices kept, SPDX lines, source file noted.

Acceptance:
- AC5.1 The stock "Snake" game and a hello-world FAP build with `fam2pkg` and run from the
  launcher unchanged except for the build file.
- AC5.2 A ported app that calls `furi_crash` is ended alone; the launcher comes back.
- AC5.3 Two `libfuri` apps installed: the shared library (R2b.9) is in flash once (`free -v`).
- AC5.4 (R5.6a) In QEMU, with the base `/etc/apps` and an extra entry of each icon form (empty,
  `@bell`, `@nosuch`, `#` bitmap, `#` bad hex, `>_`, `!`, old two-field form), the launcher's
  frame shows each icon at x = 6–13 of its row (inverted on the selected row) and a blank cell
  for the empty, unknown and malformed ones; every app still starts.
- AC5.5 (R5.6a) With `Icons: off` and `Hints: off` the launcher's frames are byte-identical to the frames of the
  launcher before R5.6a for the same `/etc/apps` (old and new forms) and the same keys.
- AC5.6 (R5.6a) Left, OK, Back on the list: the setting toggles, `/etc/launcher.conf` on the card
  holds the new value, the list redraws without icons; Left, Back without a change writes
  nothing (the file's mtime is unchanged). No internal-flash erase (`flashcheck` unchanged).
- AC5.7 (R5.6a.7) With hints on: 12 entries show 8 rows, the title shows `1/12` … `12/12` while
  moving, the scroll mark moves, the hint line is on every screen; an entry with an unknown user
  shows the notice on the message line with the list still drawn, and the hint line is back
  after 2 s with no key pressed.

Responsiveness: AC5.R A `libfuri` app's view redraw after a key event meets L1; the cooperative threads of one app never block its input thread for more than 10 ms (measured with `lat`).

RAM budget: kernel 0. Per app ≈ 1.5 KB `libfuri` state + 1 KB framebuffer + thread stacks.
Flash: `libfuri` + fonts ≈ 45 KB in the installed region, once if shared.

---

## Phase 6: Basic hardware drivers

**Goal:** everything on the board that isn't a radio: power, time, sound, haptics, GPIO,
infrared, iButton, low-power modes.
**Depends on:** 4b.

- **R6.1 Power.**
  - BQ27220 (state of charge, voltage, current, temperature) and BQ25896 (charging state, 5 V
    OTG boost, ship-mode power-off) on I2C1, exposed as `/dev/power` (read: a status record;
    `ioctl`: OTG on/off, power-off).
  - Low battery sends `SIGPWR` to INIT.
  - **Battery profiles** (owner, 2026-11-15): `full` (charge to the stock 4.208 V) and `care`
    (charge to ~4.0 V, about 80 %, for a Flipper kept on a charger, e.g. desk mode), chosen in
    settings and kept in an RTC backup register (0 B RAM, no flash write). Only the end-of-charge
    voltage (`VREG`) changes; the stock firmware already sets it the same way.
  - **The charging current stays within the manufacturer's limits** (owner, 2026-11-15): `ICHG`,
    the input limit and ICO default to the stock values (charger default ≈ 2 A, about 1C) and are
    never raised above them. The only user choice is **gentle charging** (battery care, R7.1):
    `ICHG` lowered to ≈ 1 A (0.5C), slower but cooler. Besides that the kernel may only pause:
    - pause while Sub-GHz transmits (as stock: the charger's switching disturbs the radio);
    - pause outside 0–45 °C by the BQ27220 temperature (the board has no battery thermistor on the
      charger's TS pin, so the charger cannot do it itself).
  - `/dev/power` shows the charge state, current, time to full, the profile and the source
    (charger vs computer, from USB enumeration, as R11.2).
- **R6.2 RTC.** LSE RTC holding **UTC** (section I): `time()` reads it directly and `stime()`
  writes it (R2d.5); the century and last-seen year in a backup register, the 2100-02-29 fix-up
  and the backup register map (I.3); `RTC_CALR` smooth calibration exposed to the clock settings
  and driven by the daily rate (I.6).
- **R6.3 Vibro and speaker.**
  - `/dev/vibro` (on/off, timed);
  - `/dev/speaker` (frequency, volume; PWM on TIM16 via PB8);
  - the device task plays short notification sequences from a `const` table so apps don't need to
    stay awake for them.
- **R6.4 GPIO** (and `/dev/adc` on the header's ADC pins with hardware oversampling, analog
  watchdog and circular DMA into a user buffer; used by R9.1.7):
  - `/dev/gpio` on the header pins only (2–7, 15, 16, and 13/14 when the debug console is off):
    mode, pull, read, write, edge events through `poll()`;
  - internal pins (SPI, SD, USB, buttons, radio) are never reachable.
- **R6.5 UART bridge.**
  - USART1 or LPUART1 ↔ a second USB CDC interface (Phase 10) or a tty, by circular DMA into the
    USB PMA and a 256 B RX ring;
  - the console moves to USB-only while the bridge owns USART1.
- **R6.6 Infrared** on the pulse engine:
  - RX from the IR receiver pin; TX with carrier;
  - TX power and duty limits as stock.
- **R6.7 iButton.**
  - 1-Wire master (reset, read ROM, write for RW1990 blanks) with slot timing in the kernel
    (interrupts masked ≤ 70 µs per slot via BASEPRI);
  - slave emulation (presence, ROM reply) on EXTI + timer;
  - Cyfral/Metakom via the comparator and the pulse engine.
- **R6.8 Power modes** (old R7.4; power analysis 2026-11-15, owner: "add power optimisation to
  the plan"; agent steps in next-tasks T14a–d):
  - EXTI buttons;
  - tickless idle;
  - Stop2 with LPTIM1 wake when nothing is runnable, USB is not active and no stream is armed;
  - **boot fixes (0 B RAM):** SMPS in step-down mode; CC1101 put to `SPWD` and ST25R3916 to
    power-down at boot (after a power-on reset the CC1101 idles at ~1.7 mA); unused pins analog;
    debug clocks in Stop off unless the debug flag is set;
  - **sleep blocker:** one 32-bit kernel mask, a bit per driver that needs fast clocks (transfer in
    progress, pulse engine armed, speaker PWM, USB attached, UART console). Stop2 only at 0;
  - **watchdog:** the stock option bytes keep the IWDG running in Stop2 (`IWDGSTOP=1`). Handled in
    software (longest IWDG period plus an LPTIM kick, or a short periodic wake), never by writing
    option bytes;
  - **Stop2 only**: Standby and Shutdown are not used for sleep. The board floor (IR receiver on
    SYS 0.35 mA, LCD, regulators) stays either way, and they lose RAM. Power-off is ship mode (F.3);
  - **wake sources and the EXTI map:** buttons 3, 6, 10–13; IR RX PA0 (0), CC1101 GDO0 PA1 (1), NFC
    IRQ PA2 (2), iButton PB14 (14), RTC and LPTIM1 internal. Header pins with a free line: PA4 and
    PA7 only; edge events on the other header pins (R6.4) use timer capture or polling and block
    Stop2 while armed;
  - **no interrupt lines** from the charger (INT) or the gauge (GPOUT): USB plug and low battery are
    polled over I2C, every 1 s while the screen is lit and every few minutes in Stop2;
  - statistics: time in Stop2 and wakes per source (≤ 16 B), shown by `/dev/power`.
- **R6.9 Peripheral power policy** (power analysis 2026-11-15):
  - backlight: default idle timeout **15 s** (owner, 2026-11-15; 5 min was too long); an optional
    dim stage before off (off by default), by the LP5562 engine ramp; separate timeouts on battery
    and on USB;
  - **screen-off state:** LCD power save (the ST7567 keeps its RAM, no redraw on wake) a few minutes
    after the backlight goes off, on battery; desk mode (R7b.2) keeps the LCD on;
  - SD card: ext3.3V (PA3) off when the screen is off and the card idle (cache flushed, no open
    writes); re-initialisation starts at the first key press, hidden behind the backlight ramp. Never
    while `/dev/gpio` uses header 3V3 (pin 9 shares the rail);
  - charger ADC one-shot on demand (stock leaves it continuous); 5 V OTG off when unused;
  - below a low-voltage threshold: no flash writes; SYSDWN sends `SIGPWR` and INIT powers off.
- **R6.10 Button gestures: backlight forced off / forced on** (owner request 2026-11-16):
  - **Gestures.** Hold **Left+Up** for 1.5 s: the backlight goes **forced off** (it stays dark
    whatever keys are pressed) until the same gesture again returns it to the normal idle timeout
    (15 s, R6.9). Hold **Right+Down** for 1.5 s: **forced on** (never times out), until held again.
    Doing the other gesture switches straight from one forced mode to the other.
  - **Chord rule.** A gesture is exactly its buttons down, no others, in any order; the hold time
    counts from the last button change (`k_input`, already kept: 0 B). Any change (a third button,
    one released) cancels it. Once it fires, nothing more happens until a button changes. Chords
    are at least two buttons and **never include Back**, and Back going down cancels a chord, so
    long Back (R4b.8: park or close the app) is never affected.
  - **Feedback while holding.** From the moment the chord is complete, the RGB LED blinks red
    (low level, ~25 %), faster and faster until the threshold: half-period `max(30, 200 - 170 *
    elapsed / hold)` ms. When it fires: a 30 ms vibro pulse (R6.3 timed buzz, ms timer) and the LED
    back to what it was before the chord. Cancelled: the LED back at once. The blink is driven by the
    button sampling the kernel already does every 8 ms while a key is down (`flipdev_btn`, ms timer):
    the interrupt only decides that the LED must flip and wakes the task, which does the I2C write
    (no I2C at interrupt level). Before the first blink the task reads the LP5562's three PWM
    registers into a 3 B save area; a `/dev/led` write during the hold goes into the save area, so
    it is not lost. If an LP5562 engine pattern owns the LED (an alert, R7b.5), there is no blink
    (the gesture still works, the vibro pulse confirms it). An engine program instead of the
    sampling is allowed if it scales with the hold time and leaves engine 1 to the backlight fade.
  - **Keys reach the app as usual.** The kernel cannot know at the first press that a chord is
    coming, so the down and up events of both buttons are delivered to the foreground app normally
    (never a down without its up). Apps should not act on Left+Up or Right+Down held together.
  - **Idle-off vs forced-off.** The existing rule "with the light off by the idle timeout, a press
    only turns it on and is swallowed" stays for idle-off. In forced-off, presses are delivered
    (the user works in the dark on purpose). On battery the LCD power save (R6.9, `LCD_SAVE_S`
    after the last key) still comes in forced-off; the press that wakes a blank LCD is swallowed,
    as today. Forced-on: no dimming, no light-off, no LCD power save. Later idle-ladder steps (SD
    rail off, automatic power-off, R7.1) still count from the last key in both modes.
  - **Apps that need every key.** The foreground app (or the launcher for it, from a `Gestures: no`
    key in the app's `.desktop` entry) turns gestures off with `ioctl(BTNIOCGEST, 0)` on
    `/dev/buttons`; allowed only for the foreground app (R4b.8 rules), cleared at the next
    `FGIOCSET`. Games that use diagonals set it. Long Back keeps working.
  - **State, 0 B for the mode.** The mode lives in the backlight's RTC backup register (BKP15R,
    PORTING.md map): bits 14-15 of the timeout field, 00 auto, 01 forced off, 10 forced on; the
    idle timeout shrinks to 14 bits (≤ 16383 s; `/dev/backlight` writes clamp). The mode is
    cleared at boot (a reboot always gives the normal light back). `read()` on `/dev/backlight`
    returns it, so `power`, the launcher's status bar and `backlight` can show it; `backlight force
    on|off|auto` sets it from the shell (root).
  - **The gesture table, in RAM (owner, 2026-11-16: config not in flash until it settles).** Up
    to 4 entries of 1 byte (bits 0-5 the button mask, bits 6-7 the action: 0 none, 1 toggle
    forced off, 2 toggle forced on, 3 user gesture: reserved for R7.2) and 1 byte hold time in
    50 ms units shared by all (default 30 = 1.5 s), plus the 3 B LED save area: **≤ 8 B kernel
    RAM** (owner accepts ~10 B). The defaults (the two gestures above) are a `const` table copied
    at boot. `ioctl(BTNIOCGTAB)` on `/dev/buttons` (root only; masks with Back or fewer than 2
    buttons: `EINVAL`) replaces the table; a boot script applies `/etc/flipper/gestures.conf` from
    the SD card if present (section L: defaults in ROM, changes on SD). **No flash writes** at run
    time; moving a settled table into the installed flash is a later, install-time step.
  - **Flags, 0 B.** Chord in progress, fire due (task work) and the matched entry (2 bits) fit in the
    free bits of `last_buttons` (bits 6-7, 26-28 are free today); the per-app gestures-off flag too.
  - **Interactions.** Profiles of `power`/`pm` (R7.1) sit under the forced modes: switching a
    profile does not clear them. Desk mode (R7b.2) does not override forced-off. An alert
    (`FG_PREEMPT`, alarm) respects forced-off: sound, vibro and LED wake the user, the reflective
    LCD still shows the text. Mode changes use the same ramps as the idle timeout (0.5 s fade out;
    the planned ~150 ms fade in, "Still open" in next-tasks.md, applies here too).
  - **Responsiveness.** At interrupt level only a compare of the buttons against ≤ 4 masks at each
    change, and an elapsed-time check at the 8 ms samples while a chord is held: L1 unchanged.

Acceptance:
- AC6.1 `cat /dev/power` matches the stock firmware's battery screen within 2 %; power-off and
  wake by button work.
- AC6.2 The clock survives a power-off.
- AC6.3 An IR capture of a TV remote replays and controls the TV; a raw loop-back (TX into RX with
  a reflector) reproduces the timings within ±5 %.
- AC6.4 Reading a DS1990A gives its ROM; emulation is accepted by a reader (or a second Flipper running stock).
- AC6.5 Idle current with the launcher on screen and the backlight off is lower than before this
  phase (record the numbers). Targets: ≤ 1.5 mA (stock) after R6.8, ≤ 0.6 mA with the screen-off
  state and the SD rail off (R6.9). Measured with an external meter on the battery leads (the
  gauge's 1 mA resolution is too coarse).
- AC6.6 8 h idle in Stop2: no watchdog reset, clock error within the LSE tolerance.
- AC6.7 (R6.10) Left+Up held 1.5 s: light off, keys still reach the app, light stays off;
  held again: back to the 15 s timeout. Right+Down: never times out; again: normal. Released at
  1.4 s: nothing changes and the LED is as before. With Back in the chord, or a third button, or in
  an app that set `BTNIOCGEST 0`: nothing. Long Back still parks/closes the app during and after
  a forced mode. The LED blinks faster towards the threshold; a 30 ms buzz confirms. A reboot
  clears the mode. No flash erase or write during any of this (`flashcheck` unchanged).

Responsiveness: AC6.R Waking from Stop2 by a button meets L1 (≤ 50 ms max including clock restart); speaker and vibro never delay input (L2).

RAM budget: kernel ≤ +300 B in total (R6.1–R6.9); R6.10 ≤ +8 B on top. Flash: ≈ +13 KB kernel.

---

## Phase 7: App ports I: system apps, Infrared, iButton, GPIO

**Depends on:** 5, 6.

| Stock app | MINIX process and devices | Notes |
|---|---|---|
| Archive (file browser) | `archive` app, POSIX files under `/home/flipper`; starts the matching app per file type (`.ir`, `.ibtn`, `.sub`, `.rfid`, `.nfc`, `.txt`) | Uses `libfuri` FileBrowser |
| Settings (display, sound, LED, time, power, storage, about, system) | `settings` app as root-group `operator` writing `/etc/flipper/*.conf`; `/dev/power`, `/dev/backlight` | Storage info via `df`; "format SD" disabled while mounted |
| Power (battery info, power off, reboot) | Native app `power` (R7.1) plus a status-bar battery from the launcher | `/dev/power`, `/dev/backlight` |
| Infrared | `infrared` app, group `ir`; stock `lib/infrared` codecs + universal remote DBs from SD | Stock worker thread → pulse engine + cooperative thread |
| iButton | `ibutton` app, group `ibutton`; stock `lib/ibutton` + `lib/one_wire` with HAL stubs on `/dev/ibutton` | Timing stays in the kernel (R6.7) |
| GPIO (pin control, USB-UART bridge, 5 V) | `gpio` app, groups `gpio` and `usb` | Bridge via R6.5 |

- **R7.1 Power management app `power`** (owner, 2026-11-15): the user controls **how deeply and
  how soon the device powers down**, and every other power knob that is safe and makes sense to
  expose. Stock has none of this (only backlight time and a "legacy sleep" debug switch).
  - **Profiles** as a quick choice, each a set of the knobs below: `performance` (light sleep only,
    long timeouts), `balanced` (default), `saver` (short timeouts, SD off early, BLE on demand, auto
    power-off), `custom` (any knob changed). A profile can be picked for battery and another for USB.
  - **Sleep depth:** deepest idle state allowed: `light` (WFI only, for debugging or measurement),
    `deep` (Stop2, default). Shown with what it costs (mA, from R20a.5 numbers).
  - **Idle ladder**, each step with its own delay or "never", always in this order: backlight dim →
    backlight off → screen off (LCD power save) → SD card power off → **automatic power-off** (ship
    mode, after N hours without a key; never while USB is connected or a background job runs).
    Armed alarms still ring after an automatic power-off (F.3).
  - **Low battery:** power off below N % (default: when the gauge says SYSDWN); warn at M %.
  - **Wake sources:** which buttons wake the device (all, or only Back/OK as a pocket mode against
    accidental presses); IR remote wake, phone-tap (NFC field) wake and wake-on-radio on/off (R8.5,
    R14.2.1, R20a.5). RTC alarms always wake and cannot be turned off here.
  - **Radios:** BLE off / on demand / always visible (R16.1.8).
  - **Peripherals:** 5 V on the header: off, on, or "off when idle"; header 3V3 off with the SD rail
    or always on (for a module that must stay powered); status LED notifications on/off and
    brightness.
  - **Battery care:** end-of-charge profile `full` / `care` and **gentle charging** on/off (≈ 1 A
    instead of the stock ≈ 2 A; R6.1). Nothing can set a current above the stock value.
  - **Who keeps it awake:** live list of the sleep-blocker bits and the processes holding the
    devices behind them, sleep statistics (Stop2 share, wakes per source), battery screen (%, V,
    mA with sign, °C, health, charge state, time to full or to empty, charger or computer).
    The battery screen comes first, before the rest of the app (task T15a, owner 2026-11-16): live
    current, and an **idle** value read after the backlight has been off for 30 s, so the user can
    check the idle draw on battery without opening the device or using a USB meter.
  - **Actions:** power off (says how many alarms are armed and that they will ring), reboot.
  - Limits: no setting may break L1 (≤ 50 ms) or stop alarms; settings that cost responsiveness are
    not offered (e.g. no lower CPU clock).
  - Storage: live values packed in free RTC backup registers (`ioctl` on `/dev/power` and
    `/dev/backlight`), mirrored to the settings file on the SD card (section L) so they survive a
    battery disconnect; **never internal flash**. The kernel applies them (it owns the idle ladder
    and the wake sources); the app only edits them. Group `operator`. GUI as the other system apps
    (`flipper/gui` now, `libfuri` later). RAM: ~2 KB while open, 0 B static beyond the registers.
  - Command-line twin `pm` (same `ioctl`s, `-t` output per K.1) for scripts and the shell.
- **R7.2 Gestures app `gestures` (planned; owner request 2026-11-16), after R6.10:**
  - in the settings group; lists the 4 gesture slots of R6.10; "record" a chord by holding the
    buttons (Back cannot be part of one), pick the action, set the common hold time (0.5–5 s); a
    test mode that shows the LED countdown without acting.
  - saves to `/etc/flipper/gestures.conf` on the SD card (one line per slot, TSV: mask, action,
    and for a user gesture the command line) and applies it at once with `ioctl(BTNIOCGTAB)`.
  - user gestures (action 3): the kernel flags the entry and signals INIT, which forks the
    short-lived `/usr/bin/gesture N` (the same pattern as `timed` on `SIGRTC`, F.2); it reads the
    command for slot N from the conf file and runs it (e.g. start the mouse jiggler, a TV-remote
    "mute", toggle desk mode). No resident daemon, no extra kernel RAM.
  - once the owner says the table has settled, the defaults move into the installed flash
    (install-time write only).
  - Later ideas, not scheduled: sequences (e.g. Up Up Down within 1 s), per-app tables.

Acceptance:
- AC7.0 Every setting in `power` takes effect at once, survives a power-off and a battery
  disconnect (from the SD copy), and matches what `pm` and `cat /dev/power` report. With
  `saver` the measured idle current is lower than with `balanced`; automatic power-off happens at
  the set time and an alarm armed before it still rings.
- AC7.1 Each app opens and saves files compatible with stock: files saved by stock firmware 1.x
  load, and files saved here load on stock.
- AC7.2 The IR universal remote for TVs works on at least 3 brands.
- AC7.3 Running the IR app as a user outside group `ir` fails to open `/dev/ir`.
- AC7.4 (R7.2) A gesture recorded in `gestures` works at once and after a reboot (from the SD
  file); a user gesture runs its command; removing the SD card gives the default table.

Responsiveness: AC7.R L6 and L7 met for every app in the table; the IR learn screen shows a received signal within 100 ms of the end of the burst.

RAM: per app as in section B (2–6 KB plus `libfuri`); nothing static. Flash: ≈ 40 KB installed.

## Phase 7b: Time services and the first native apps

**Goal:** apps that act at a set time without staying in RAM, alerts that always get through,
and the first native apps: an interval reminder, an alarm clock and a hardware Self Test.
**Depends on:** 6 (RTC, speaker, vibro, LED, power modes), 4b (foreground ownership), 3 (sticky
bit, groups), 2b (shared libraries). Uses 5 (`libfuri`) for the GUI if it is ready; otherwise
plain `/dev/lcd`. Can start as soon as Phase 6 is done, in parallel with Phase 7.

Infrastructure (section F):
- **R7b.1 RTC alarm driver.** `/dev/rtc` `ioctl`s: set and clear Alarm A (date or weekday + H:M:S),
  read the armed time; wake-up timer for periodic minute ticks; `SIGRTC` to INIT on alarm, after
  `stime()` and after a wake from power-off. Only group `timers` (and root) may arm Alarm A; the
  value armed is always the one `timed` computed.
- **R7b.2 Foreground preemption.** `ioctl(FG_PREEMPT)` on the foreground device (R4b.8), allowed
  for group `notify`: the caller gets LCD and buttons until it calls `FG_RELEASE` or exits; the
  preempted app keeps running, its LCD writes are dropped meanwhile, and it gets `SIGWINCH` on
  release to redraw. Nested preemption: the newer alert queues behind the shown one (one queue
  entry per alert process, in the existing foreground state).
- **R7b.3 INIT** handles `SIGRTC` by forking `/usr/lib/timed` (at most one instance; a second
  signal while it runs sets a "run again" flag).
- **R7b.4 `timed`, `alert`, `libapp`, notification policy** as F.2.
- **R7b.5 LP5562 engine patterns** for alert LEDs (F.2), loaded by the LED driver from a `const`
  table; apps choose a pattern by name.
- **R7b.6 Power-off with alarms** as F.3.

### 7b.1 Interval reminder (`remind`)

- Any number of reminders, each with: text ("Drink a glass of water"), interval in minutes,
  active window (e.g. 09:00–17:00), weekdays, sound/vibro/LED choice, snooze length.
- Each reminder is one `interval` timer, category `reminder`.
- The alert's OK means "done": `remind` appends one line to
  `/ext/apps_data/remind/log` (time, reminder id). The app shows today's count and a 7-day bar chart.
- While the reminder app is closed, nothing of it is in RAM.
- Acceptance:
  - AC7b.1.1 A reminder every 30 min from 09:00 to 17:00 fires at 09:00, 09:30 … 16:30 (and 17:00
    if the window end is inclusive, a setting), within ±1 s of RTC time, on chosen days only.
  - AC7b.1.2 It fires while another app (e.g. Sub-GHz RAW receive) is in the foreground; that app's
    capture loses no edges and it redraws after the alert closes.
  - AC7b.1.3 Do-not-disturb from 22:00 to 07:00 suppresses reminders but not alarms.
  - AC7b.1.4 Changing the clock by +2 h skips (or fires once, per `Missed`) the reminders in between.

### 7b.2 Alarm clock (`alarm`)

- Any number of alarms: time, weekdays or one-shot, label, sound (built-in tones or a user
  `.fmf`/RTTTL file), volume ramp, vibro on/off, snooze length, ring time.
- Each alarm is one `once` or `daily` timer, category `alarm`: it **always** sounds or vibrates
  per its own setting, regardless of the foreground app, the global sound switch and
  do-not-disturb.
- **Desk clock mode:** the app in the foreground shows a large clock and the next alarm; display
  update once a minute from the RTC wake-up timer (CPU in Stop2 in between); backlight level
  chosen in settings (off, dim, on; "on while charging" when `DeskMode` is granted). Buttons work
  as usual; leaving desk mode keeps alarms armed.
- Acceptance:
  - AC7b.2.1 An alarm set 2 min ahead rings within 1 s of its time with: desk mode open; another
    app in the foreground; the device idle in Stop2; and (if AC6.2 holds) powered off.
  - AC7b.2.2 Snooze 5 min re-rings 5 min later; dismiss stops sound, vibro and LED at once.
  - AC7b.2.3 With the global sound switch off, an alarm set to sound still sounds; one set to
    vibro-only only vibrates.
  - AC7b.2.4 Desk mode for 8 h on battery uses less charge than the stock clock app over the same
    time (record both).

Responsiveness:
- AC7b.R1 From RTC alarm to first sound: ≤ 500 ms from Run or Stop2 (wake, INIT fork, `timed`,
  `alert`); from power-off the alarm rings within 1 s of its time (boot lead, F.3).
- AC7b.R2 Alert buttons meet L1; the preempted app redraws ≤ 150 ms after dismiss.
- AC7b.R3 An alert never masks interrupts or holds the scheduler beyond rule 0.8 (L3, L4 unchanged
  while an alarm rings).

RAM budget:
- kernel ≤ +64 B (alarm state, preemption owner and queue); INIT ≤ +8 B (flag);
- resident between events: **0 B** (the next time is held in the RTC alarm registers);
- while firing: `timed` ≈ 3 KB for under a second, `alert` ≈ 4 KB while shown;
- while open: `remind` ≈ 4 KB, `alarm` ≈ 5 KB in desk mode (with the shared libc and `libapp`).
Flash: kernel ≈ +2 KB; installed ≈ 30 KB (`timed`, `alert`, `libapp`, two apps, tones).

### 7b.3 Self Test (`selftest`, owner request 2026-11-14)

A built-in hardware self test in the spirit of the 8-bit home computers' self-test programs: a
short menu, each test drawn as a big, clear picture, and an "all tests" loop for burn-in. The
look, the sounds and the tune are our own (no copied screens, graphics or melody).

Structure (extensible, section F.4):
- The app is a small menu shell. Each test is a **module**: a separate small program in
  `/usr/lib/selftest/` (in the installed region, so all code is flash) with a one-line
  description file. The menu lists what is installed; a phase that adds a driver adds its module.
  A module prints a result line (`PASS`, `FAIL <reason>`, `SKIP <reason>`) and draws its own
  screen through `libapp`.
- Modes: interactive (pick a test), **All tests** (every non-interactive module in a loop, with a
  pass counter; stop on the first failure or keep looping for burn-in, a setting), and
  `selftest -a` from the shell for scripts and CI on the device. Each run appends to
  `/ext/apps_data/selftest/log`.
- SHOULD: holding Up + Back while powering on starts the self test from INIT before `/etc/rc`
  mounts anything but the root file system (useful when the SD card is the suspect).

Modules in the basic version (Phase 7b, needs Phases 4a, 4b, 6):
- **R7b.3.1 Memory.**
  - RAM: MM lends free holes to the test one 4 KB block at a time (`svrctl(MMTEST)`, root or group
    `selftest`), runs a March C− pattern test on the block **inside MM** and gives it back; the
    process never sees the memory, so data left by exited processes can't leak, and the pattern
    overwrites it. Blocks in use are shown as "in use", not tested. SRAM2 (FS cache, CPU2-shared
    area) is shown as owned, not tested.
  - Flash: CRC of each region with the CRC unit (system image against the CRC stored at build
    time; installed programs against their install-time hash, R2b.3; the cache), and the ECC
    counters from R4a.5 (single errors corrected, double errors seen). CPU2's area is shown as
    owned.
  - Screen: RAM as an 8 × 8 grid of 4 KB blocks, flash as a 16 × 16 grid of 4 KB pages, each cell
    showing untested, testing, pass, fail, in use or owned.
- **R7b.3.2 Audio-visual.** An original short tune on the speaker in rising and falling scales
  (checks the full frequency range), vibro pulses, the RGB LED through each colour (an LP5562
  engine pattern), and display patterns (all on, all off, checkerboard, inverse, contrast sweep,
  backlight ramp). Interactive: OK = "looked/sounded right", Back = "wrong" (logged as FAIL).
- **R7b.3.3 Input.** A drawing of the d-pad, OK and Back; each lights while pressed. Short, long
  and repeat presses are shown, and the press-to-read latency of each press (L2, measured as by
  `lat`). Passes when every button has been seen down and up.
- **R7b.3.4 Board.** RTC running (seconds advance, LSE ready, calibration value); SD card (CID,
  size, a read of 1 MB with speed, a write-read-compare of one scratch sector in a reserved
  area); battery gauge and charger (device IDs, voltage, temperature, charge state); LP5562
  present; USB connected or not.
- **R7b.3.5 GPIO loopback** (interactive): asks the user to connect header pin pairs with a jumper
  (2–3, 4–5, 6–7, 15–16), then drives each pin and reads the other, with pull-ups and pull-downs;
  5 V output switches on and its presence is read back through the gauge.

Modules added by later phases:

| Phase | Module | What it checks |
|---|---|---|
| 8 | `subghz` | CC1101 part number and version registers, PLL lock on each band, RSSI noise floor (receive only) |
| 8 | `rfid` | LF field on/off, carrier frequency measured on the comparator; reads a tag if one is present |
| 6/7 | `ir`, `ibutton` | IR receiver idle level and a TX pulse seen by RX with a reflector (interactive); 1-Wire bus reset with presence if a key is held on |
| 10 | `usb` | Enumeration state, PMA test pattern on unused buffers |
| 13 | `nfc` | ST25R3916 identity register, oscillator and regulator, field on/off, tag read if present |
| 15 | `cpu2` | FUS and stack versions and type (R2b.10), IPCC round trip, BLE advertising starts and stops |

Acceptance:
- AC7b.3.1 On a good Flipper, All tests passes 100 loops in a row, and the log shows each result.
- AC7b.3.2 A deliberately corrupted installed program (one byte flipped through DFU) shows its
  pages as FAIL in the flash grid and names the program.
- AC7b.3.3 With the SD card removed, the SD module reports `FAIL no card` and the rest still runs.
- AC7b.3.4 The memory test running in All tests mode does not disturb a USB console session
  or an alarm (7b.2) that fires during it.

Responsiveness: the memory test works one 4 KB block per MM step (R0.8b), so L1 and L2 hold
while it runs; Back stops any test within 100 ms.

RAM: app ≈ 3 KB plus one module ≈ 2 KB while it runs (shared libc, `libapp`); grid state 2 bits
per cell (≈ 80 B); MM ≤ +16 B; kernel 0. Flash: ≈ 20 KB installed for the basic modules, ≈ 2 KB
per later module, ≈ 1 KB in MM.

### 7b.4 GPIO sensors and their apps (section H)

**Depends on:** 4b (pulse engine, ms timer, zero-copy DMA), 6 (GPIO, 5 V output, ADC), 7b.1–7b.2
(timers and alerts). Adds a Self Test module (`sensors`: each configured sensor answers and
passes its checksum).

- **R7b.4.1** `sensor` driver part, `/etc/sensors.conf`, `/dev/sensor/*` and `sensorctl` as H.3,
  with the modules of H.2: DHT11/22, DS18B20 (UART 1-Wire), SHT3x/SHT4x, AHT20, HTU21D,
  BME280/BMP280, HC-SR04 family.
- **R7b.4.2 Thermo-hygrometer app `thermo`:**
  - big temperature and humidity, dew point, pressure when available; several sensors on one
    screen (e.g. room and outside);
  - min/max since reset and for today, and a 24 h / 7 day graph from a log;
  - logging every N minutes (default 5) by a `timed` `run` timer (`thermo --log`): the app is not
    resident between samples; the log is `/ext/apps_data/thermo/<sensor>.csv`, one line per
    sample (time, values), rotated monthly;
  - alarms by threshold with hysteresis ("freezer above −15 °C", "humidity above 70 %: air the
    room") posted through `alert` (category chosen per alarm: `alarm` always sounds,
    `reminder` follows do-not-disturb); checked at each logged sample;
  - desk mode like the alarm clock's (R7b.2): updates once a minute, CPU in Stop2 in between;
  - later, the same app can show Sub-GHz weather stations (F.5 suggestion) as more sources.
- **R7b.4.3 Sonar app `sonar`:**
  - big distance readout (cm or m, 1 mm resolution shown, accuracy stated as about ±1 cm for the
    HC-SR04), hold button, min/max, average of 5;
  - **parking-sensor mode:** beeps faster and higher as the distance shrinks, continuous tone
    under a set limit (default 30 cm), vibro option;
  - measurement helpers: area (two measurements) and volume (three);
  - temperature compensation when a temperature sensor is configured;
  - 5 V on pin 1 is switched on only while the app is open.
- Acceptance:
  - AC7b.4.1 A DHT22, a DS18B20 and a BME280 (or SHT3x) each give values matching a reference
    thermometer within the sensor's stated accuracy; `cat /dev/sensor/room` works from the
    shell.
  - AC7b.4.2 Reading a DHT22 every 2 s for 1 h gives no checksum errors with the SD card busy and
    the USB console active, and L3/L4 are unchanged (no masked intervals added).
  - AC7b.4.3 A 24 h log at 5-minute intervals has 288 lines; `free` between samples shows no
    `thermo` process.
  - AC7b.4.4 A freezer alarm at −15 °C fires (with a DS18B20 in a cold drink) within one log
    interval.
  - AC7b.4.5 The sonar measures a wall at 0.5, 1 and 2 m within ±1 cm (with temperature
    compensation), and the parking-sensor beep rate follows a moving hand without visible lag.
- Responsiveness: sonar display and beep update ≥ 10 times per second, beep follows a new
  reading within 50 ms; sensor reads never mask interrupts (all timing by timers, UART or DMA);
  L1 holds in both apps.
- RAM: kernel ≤ +64 B (sensor state union, configured pins); per read 192 B in the caller;
  `thermo` ≈ 4 KB while open (graph drawn row by row from the log file, not loaded whole),
  0 B between samples; `thermo --log` ≈ 2 KB for under a second; `sonar` ≈ 3 KB while open.
  Flash: ≈ +4 KB kernel; ≈ 20 KB installed.

---

## Phase 7c: Scripting runtime for gadgets and sensors (owner request, 2026-11-14)

**Goal:** small apps and sensor logic written as scripts, without a C toolchain, without giving up
the RAM and responsiveness rules.
**Depends on:** 2b (shared library in flash, installed region), 5 (`libfuri` GUI), 7b (`libapp`:
timers and alerts), 7b.4 (`/dev/sensor`).

### 7c.0 Options compared

Flash figures marked *measured* were compiled for this review (Thumb-2, `-Os`, interpreter core
without OS/file libraries; compile only). The others come from each project's own documentation.
Runtime RAM was **not** measured (running the built interpreters in an emulator was not possible
in this review), so those figures are documentation claims or estimates from data-structure
sizes; AC7c.1 measures them on the device.

| | Berry | Lua 5.4 | Lua 5.4 + flash tables (eLua/NodeMCU-style "rotables" and Lua Flash Store) | MicroPython | mJS (stock Flipper JS engine) | Forth (e.g. zForth) |
|---|---|---|---|---|---|---|
| Flash | < 40 KiB core (docs) | ≈ 94–103 KB core *(measured)*; ≈ 15 KB of that is the compiler, which can be left out if scripts are precompiled | as Lua + patch | ≈ 80 KB minimal, 200–300 KB with a useful library set (docs/typical ports) | ≈ 57 KB *(measured)* | 2–8 KB |
| RAM at start | < 4 KiB heap (docs) | ≈ 6–10 KB with base, string, table and math libraries (estimate: every library function is a table entry and an interned string in RAM) | ≈ 1.5–3 KB (library tables and their strings stay in flash) | fixed heap reserved up front; 2 KB is the bare minimum, 16–32 KB is needed for real scripts | < 1 KB (docs) | < 1 KB |
| Constant data and code in flash (XIP) | **Yes, built in:** compile-time constant objects, and `solidify` turns Berry functions and classes into C constants in flash | No: library tables and loaded bytecode live in RAM | Yes, but needs a maintained out-of-tree patch for 5.4 | Yes: frozen bytecode modules and ROM tables | No: scripts are parsed into RAM | Words can be in flash |
| Garbage collector | Mark-and-sweep; small heaps keep pauses short | Incremental or generational, short pauses | same | Stop-the-world over the whole fixed heap; pause grows with heap size | Simple; scripts kept small | None |
| Language | Python/Lua-like, classes, closures | Lua | Lua | Python 3 subset | JavaScript subset (no closures, restricted) | Forth |
| Sensor/driver track record | Tasmota (sensor and relay firmware) uses it for exactly this kind of scripting | Wide (games, NodeMCU) | NodeMCU on ESP8266 | Very wide (boards, education) | Mongoose OS, stock Flipper | Niche |
| License | MIT | MIT | MIT | MIT | GPL-2.0 or commercial | varies (MIT for zForth) |

What matters on this system:
- Each script runs as **its own MINIX process** with its own interpreter state, so a GC pause
  stalls only that script; rule 0.8's budgets for the foreground app and the system are not
  affected. Within a script's own UI, short GC pauses still matter (≤ 10 ms target, AC7c.3).
- The interpreter is a **shared library in flash** (R2b.9), so ten scripts cost ten states, not
  ten interpreters. Its RAM at start is paid once per running script: that is the number to
  minimise.
- Precompiled scripts whose bytecode and constants run from flash cost almost no RAM for code.

### 7c.1 Recommendation

**Berry as the scripting language**, because it is the only option that keeps library tables
*and* compiled scripts in flash without out-of-tree patches, starts in under 4 KiB, has a
friendly Python-like syntax, and is proven for sensor and gadget scripting (Tasmota). **Lua 5.4
with flash tables** is the alternative if a bigger ecosystem matters more than the patch
upkeep; plain Lua costs 6–10 KB per script. MicroPython is the most popular but needs a fixed
16–32 KB heap per running script, which is too much here. mJS is only worth it for stock-JS
compatibility (7c.3). **Decided 2026-11-14 by the owner: Berry.** Lua and MicroPython are not planned; the bindings
are written against Berry's C API.

### 7c.2 Requirements

- **R7c.1** The Berry interpreter as a shared library in the installed region; a small launcher
  binary (`berry`) and `#!` support so scripts run from the shell and the launcher. Script
  apps are packages like any other (manifest, own uid, groups, A.3).
- **R7c.2** Precompilation at install: `pkg` compiles scripts to bytecode, and Berry's
  `solidify` places the bytecode and constants in the installed region so they run from flash. Source stays on the SD card for editing.
- **R7c.3 Bindings** (one C module each, in flash): `sensor` (read `/dev/sensor/*`, H.3),
  `timer` (`libapp` timers: a script registers a `run` action and is started with `--timer id`, so
  it isn't resident between events), `alert` (`notify_post`), `gpio` and `adc` (R6.4), `subghz`,
  `ir`, `nfc` and `rfid` (high-level read/send only; raw streams stay with the C apps), `gui`
  (`libfuri` canvas, menus and dialogs, input events), `storage` (files under the app's data
  directory), `usbhid`. Each binding checks the app's groups through the normal device
  permissions; scripts get no more access than a C app with the same manifest.
- **R7c.4 Limits per script process:** heap limit from the manifest (default 8 KB), instruction
  budget per event callback (a runaway loop is stopped with an error, not by killing the
  launcher), and Back long-press works as for any app (R4b.8).
- **R7c.5 REPL** over the USB console and BLE serial (`berry -i`), for trying sensors live.
- **R7c.6 Examples shipped:** the thermo logger alarm in 20 lines, a sonar beeper, a GPIO blink
  and button counter, a "drink water" reminder written as a script.

### 7c.3 Optional: stock JavaScript apps

The stock firmware runs JavaScript apps on mJS with its own modules (`badusb`, `serial`, `gpio`,
`gui`, `notification`, `storage` and others). An optional `mjs` runtime with a module shim
mapped onto the same bindings (R7c.3) could run many of those scripts unchanged. It costs about
57 KB of flash *(measured)* and little RAM, but mJS is GPL-2.0-or-commercial and its project is
no longer actively developed; schedule it with the ecosystem work in Phase 18 if wanted.

Acceptance:
- AC7c.1 On the device: RAM of an idle script process (`free -t`) ≤ 6 KB including its stack,
  with the chosen language; record the number for each candidate tried.
- AC7c.2 The R7c.6 examples run; the thermo script logs for 24 h through timers with no resident
  process between samples.
- AC7c.3 A script allocating in a loop keeps its own GC pauses ≤ 10 ms (heap ≤ 8 KB), and the
  foreground app's L1/L2 are unaffected while it runs in the background.
- AC7c.4 A script without group `subghz` can't open the Sub-GHz binding (EACCES), as for C apps.

RAM: kernel 0; per running script ≈ 4–6 KB (Berry) or ≈ 2–4 KB (Lua with flash tables) plus its own
data; 0 between timer events. Flash: interpreter ≈ 40–60 KB shared, bindings ≈ 15 KB.

---

## Phase 8: Sub-GHz and 125 kHz RFID drivers

**Depends on:** 4b (pulse engine, bus arbitration), 6 (power for the 5 V external module).

- **R8.1 CC1101 on SPI1** (`/dev/subghz`):
  - register read/write `ioctl` (burst through the 64 B FIFO, A.6);
  - RSSI, LQI, calibration, the RF switch for the 315/433/868 MHz paths;
  - async RX/TX on GDO0 through the pulse engine;
  - packet mode by FIFO with GDO interrupts.
- **R8.2 Transmit limits in the kernel.**
  - Before any transmit, the kernel reads back the frequency registers (`FREQ2..0`), the PA
    table and the data rate, and refuses (`EPERM`) anything outside the band table for the
    configured region.
  - The table is `const` in flash, with the same bands as stock.
  - The region is set by root (`/etc/flipper/region`) and applied at boot.
  - Registers that bypass the check while transmitting are write-protected while TX is active.
- **R8.3 External CC1101** on the header SPI (stock supports it): same driver, second instance,
  selected by `ioctl`; the 5 V OTG is enabled through `/dev/power`.
- **R8.4 125 kHz RFID** (`/dev/lfrfid`):
  - carrier PWM;
  - comparator + pulse engine capture (read);
  - DMA-driven pull-pin modulation from a user buffer (emulate, T5577 write);
  - exclusive with `/dev/nfc` because of the shared NFC-IRQ/RFID-pull pin.
- **R8.5 Wake-on-radio** (power analysis 2026-11-15): the CC1101 WOR mode (RC-timed RX sniffs,
  GDO0 → EXTI1) wakes the CPU from Stop2 when a matching packet or carrier arrives; the sleep
  blocker stays clear. Use: a 433/868 MHz doorbell, gate remote or sensor as a notification
  source (~tens to hundreds of µA instead of ~16 mA for full RX). Duty cycle and RC calibration
  as the datasheet requires; `ioctl` on `/dev/subghz`.

Acceptance:
- AC8.1 A transmit request at a frequency outside the table fails with `EPERM`, even when written
  directly to the registers.
- AC8.2 Raw capture of a 433.92 MHz remote replays and is accepted by its receiver.
- AC8.3 An EM4100 card reads and emulates; a T5577 writes and verifies.

Responsiveness: AC8.R No captured edge is lost during a 60 s RAW receive while the user scrolls a menu and the SD card is written (L3, L8; flash writes are held off).

RAM: kernel ≤ +200 B (state only; streams use user buffers, R4b.5). Flash: ≈ +10 KB kernel.

## Phase 9: App ports II: Sub-GHz and 125 kHz RFID

**Depends on:** 5, 8.

| Stock app | MINIX mapping | Notes |
|---|---|---|
| Sub-GHz: Read, Read RAW, Saved, Add manually, Frequency analyzer, Region info | `subghz` app, group `subghz`; stock `lib/subghz` protocols, `.sub` files (including RAW); keystore from `/home/flipper/subghz/assets` | Decoders run on capture blocks from `read()`; history kept in a bounded file-backed list to cap RAM |
| Sub-GHz CLI | `subghz` command (`rx`, `tx`, `chat`) | Unix pipes allow `subghz rx \| grep` |
| 125 kHz RFID: Read, Saved, Add manually, Extra actions (T5577) | `lfrfid` app, group `rfid`; stock `lib/lfrfid` | — |

Acceptance:
- AC9.1 At least 15 of the stock Sub-GHz static-code protocols decode from recorded `.sub` RAW files
  identically to stock (run the same files through both).
- AC9.2 The frequency analyzer runs at ≥ 10 updates/s.
- AC9.3 The Sub-GHz app's peak RAM (`free -t`) ≤ 20 KB.

Flash: ≈ 85 KB installed (protocols are the bulk).

### 9.0 LF RFID: every protocol of the Unleashed firmware (owner request 2026-11-17)

The owner wants every 125 kHz decoder of the Unleashed firmware (DarkFlippers/unleashed-firmware,
`lib/lfrfid/protocols/`, checked at commit `53818b1`, 2026-11-16) working on MINIX. GPLv3: the
protocol files are copied almost verbatim (rule of the whole project), not rewritten.

What MINIX has today (R6.5 parts 1–5, #174–#194): `/dev/rfid` read at 125 kHz with 50 % duty,
one-shot capture of up to 512 edges; `rfid` decodes **EM4100 at RF/64 only**; T5577/EM4305 write
of an EM4100 number, T5577 block read, password clear (proxmark3 dictionary); EM4100 emulation
(`RFIDIOCEMU`, ≤ 512 segments); the `lfrfid` launcher app (read, save `.rfid`, write, emulate).

**Gap table** (Unleashed protocol list order; "seg/frame" is the number of `RFIDIOCEMU` segments
one frame needs today, where one segment is one PWM period of the pull pin):

| # | Protocol (Unleashed name) | Maker | Modulation, rate | Frame | Read mode | seg/frame | MINIX read | emulate | write |
|---|---|---|---|---|---|---|---|---|---|
| 1 | EM4100 | EM-Micro | Manchester RF/64 | 64 b | ASK 125 k | ≤ 128 | ✅ | ✅ | ✅ T5577, EM4305 |
| 2 | EM4100/32 | EM-Micro | Manchester RF/32 | 64 b | ASK | ≤ 128 | ❌ | ❌ | ❌ |
| 3 | EM4100/16 | EM-Micro | Manchester RF/16 | 64 b | ASK | ≤ 128 | ❌ | ❌ | ❌ |
| 4 | Electra | Electra | Manchester RF/64 (EM4100 + 64-bit epilogue) | 128 b | ASK | ≤ 256 | ❌ | ❌ | ❌ |
| 5 | H10301 | HID | FSK2a RF/50 | 96 b | ASK | ≈ 540 | ❌ (summary only) | ❌ > 512 | ❌ |
| 6 | Idteck | IDTECK | PSK1 RF/32 | 64 b | PSK | ≈ 2048 | ❌ | ❌ > 512 | ❌ |
| 7 | Indala26 | Motorola | PSK1 RF/32 | 64 b | PSK | ≈ 2048 | ❌ (summary only) | ❌ > 512 | ❌ |
| 8 | IoProxXSF | Kantech | FSK2a RF/64 | 64 b | ASK | ≈ 460 | ❌ | ❌ (tight) | ❌ |
| 9 | AWID | AWID | FSK2a RF/50 | 96 b | ASK | ≈ 540 | ❌ | ❌ > 512 | ❌ |
| 10 | FDX-A | FECAVA | FSK2a RF/50 | 96 b | ASK | ≈ 540 | ❌ | ❌ > 512 | ❌ |
| 11 | FDX-B | ISO 11784/5 (animals) | Biphase (diphase) RF/32 | 128 b | ASK; Unleashed also 134.2 kHz | ≤ 256 | ❌ | ❌ | ❌ |
| 12 | HIDProx (generic) | Generic | FSK2a RF/50 | 96 b | ASK | ≈ 540 | ❌ | ❌ > 512 | ❌ |
| 13 | HIDExt (generic) | Generic | FSK2a RF/50 | 6 blocks, 192 b | ASK | ≈ 1080 | ❌ | ❌ > 512 | ❌ |
| 14 | Pyramid | Farpointe | FSK2a RF/50 | 128 b | ASK | ≈ 720 | ❌ | ❌ > 512 | ❌ |
| 15 | Viking | Viking | Manchester RF/32 | 64 b | ASK | ≤ 128 | ❌ | ❌ | ❌ |
| 16 | Jablotron | Jablotron | Biphase (diphase) RF/64 | 64 b | ASK | ≤ 128 | ❌ | ❌ | ❌ |
| 17 | Paradox | Paradox | FSK2a RF/50 | 96 b | ASK | ≈ 540 | ❌ | ❌ > 512 | ❌ |
| 18 | PAC/Stanley | N/A | Direct (NRZ) RF/32 | 128 b | ASK | ≤ 128 | ❌ | ❌ | ❌ |
| 19 | Keri | Keri | PSK1 RF/32 | 64 b | PSK | ≈ 2048 | ❌ | ❌ > 512 | ❌ |
| 20 | Gallagher | Gallagher | Manchester RF/32 | 96 b | ASK | ≤ 192 | ❌ | ❌ | ❌ |
| 21 | Nexwatch | Honeywell | PSK1 RF/32 | 96 b | PSK | ≈ 3072 | ❌ | ❌ > 512 | ❌ |
| 22 | Radio Key (Securakey) | Securakey | Manchester RF/40 | 96 b | ASK | ≤ 192 | ❌ | ❌ | ❌ |
| 23 | GProxII | Guardall | Biphase RF/64 | 90 b | ASK | ≤ 180 | ❌ | ❌ | ❌ |
| 24 | Noralsy | Noralsy | Manchester RF/32 | 96 b | ASK | ≤ 192 | ❌ | ❌ | ❌ |
| 25 | Indala224 | Motorola | PSK2 RF/32 | 224 b | PSK | ≈ 7168 | ❌ | ❌ > 512 | ❌ |

**Encoders** (owner, 2026-11-17: "and the encoders too"): every one of the 25 Unleashed protocols
has an encoder (`encoder_yield`, used for emulation) and a `write_data` that fills T5577 blocks;
8 of them can also be written to an EM4305 (EM4100 and its /32, /16 variants, Electra, FDX-B,
Gallagher, GProxII, Jablotron, Radio Key, Viking). MINIX has the encoder and both write paths for
EM4100 RF/64 only, so 24 encoders and 24 T5577 / 9 EM4305 write paths are missing; R9.0.3 and
R9.0.6 bring all of them.

Also in Unleashed `lib/lfrfid`, not protocols: write targets **Hitag µ (ID8265, ID8210, H55)** and
**Hitag S (ID8268)** besides T5577/EM4305 (MINIX: T5577/EM4305 only); **RAW read/emulate**
(`.ask.raw`/`.psk.raw`, varint pairs; MINIX: none); the **134.2 kHz ASK read mode** (MINIX: none);
`.rfid` save/load for every protocol (MINIX: EM4100 only).

Result: 1 of 25 protocols done (EM4100 RF/64): 24 decoders and 24 encoders missing. What blocks them is mostly not the
decoders (pure C, host-testable) but four kernel gaps: the PSK carrier, the 134.2 kHz carrier, a
capture longer than 512 edges, and emulation of more than 512 segments.

Requirements:

- **R9.0.1 `liblfrfid`** (user space, `src/lib/lfrfid/`): the Unleashed `lib/lfrfid/protocols/*`,
  `tools/fsk_demod`, `tools/fsk_osc`, `tools/t5577`, `tools/em4305` and the toolbox pieces they use
  (`protocol_dict`, `manchester_decoder`, `bit_lib`, `level_duration`, `varint_pair`) copied with a
  thin `furi` shim (`furi_assert`, `malloc`, `FuriString` → fixed `char` buffers). The worker
  threads, `furi_hal_rfid` and the GUI are not ported: the caller feeds edges and plays segments.
  One protocol table, so a new Unleashed protocol is one more file and one table line.
  - RAM: the decoders' state is allocated only while a read runs (`protocol_dict_alloc` of all 25:
    measure it; budget ≤ 6 KB heap in the `rfid` process, freed at exit). Nothing resident.
  - Flash: ≈ 25 KB text (XIP), linked into `rfid` (no shared library needed). 0 run-time flash
    writes.
- **R9.0.2 Host tests** (`flipper/tests/lfrfid/`, `make lfrfidtest` on the PC and in CI): for each
  of the 25 protocols, encoder → edge timings (± jitter, random start phase, inverted polarity) →
  all decoders: exactly that protocol decodes, the same bytes, `render_data` text equal to
  Unleashed's for a fixed vector per protocol. Also EM4100 vectors captured on the Flipper by
  `rfid -r` (from #174) still decode. No hardware needed.
- **R9.0.3 `rfid` uses the library:** `rfid` (read) prints `PROTOCOL HEXDATA` and the rendered
  text (as the stock app's info screen); `rfid write PROTO HEX…` writes any protocol whose
  `write_data` fills T5577 (or EM4305) blocks, verified by reading back and decoding (as stock);
  `rfid emulate PROTO HEX…`; `rfid copy` for every protocol. EM4100 behaviour and options stay as
  they are (`write EM …` kept as an alias). `.rfid` files in the stock format for every protocol
  (`Key type:` = the Unleashed protocol name). Read alternates ASK and PSK windows as the stock
  worker does (ASK 2 s, PSK 2 s; `-a`/`-p` for one only).
- **R9.0.4 Kernel: read carrier modes** (`/dev/rfid`, `ioctl RFIDIOCCARRIER`): 125 kHz/50 % (ASK
  and FSK, today's default), **62.5 kHz/25 % (PSK, as stock `furi_hal_rfid_tim_read_start(62500,
  0.25)`)** and **134.2 kHz/50 %** (FDX-B, Unleashed's ASK134 mode). One byte of state, kernel
  RAM ≤ +4 B. The mode resets to 125 kHz at close.
- **R9.0.5 Kernel: continuous capture.** A PSK or FSK frame is far longer than 512 edges
  (Indala26 ≈ 33 ms; Indala224 ≈ 115 ms; FSK edges every 32–40 µs). `/dev/rfid` gets a streaming
  read: the comparator capture keeps running into a ring in the caller's pinned buffer (two
  halves; `read` returns a full half; `poll` readable when one is full), without losing an edge
  between reads; an overrun is reported (`EOVERFLOW` on the next read), never silent. The decoder
  runs in the `rfid` process at normal priority: the UI stays responsive (AC9.0.4). Kernel RAM ≤
  +8 B.
- **R9.0.6 Kernel: long emulation.** FSK and PSK frames need 460–7200 PWM segments (table), so
  `RFIDIOCEMU` with ≤ 512 pinned words cannot play them. Add a compact run list, e.g.
  `struct rfid_emu_run { u16 period_minus_1; u16 pulse; u16 count; }` (count = how many equal
  segments in a row: an FSK bit is one run, a PSK bit one or two runs), at most 512 runs, played by
  the kernel: the DMA keeps reading a small ping-pong ring of ARR/CCR pairs in the caller's pinned
  memory, refilled from the run list in the DMA half/complete interrupt (as stock refills from the
  encoder in its DMA ISR). The interrupt is O(ring half), ≤ 10 µs, at ≤ 2 kHz (PSK worst case).
  The agent may pick a different mechanism with the same limits (no CPU work per carrier period,
  kernel RAM ≤ +16 B, the reader's clock drives it). `RFIDIOCEMU` stays for EM4100.
- **R9.0.7 Hitag µ / Hitag S write targets** (Unleashed `tools/hitagmicro.c`, `tools/hitags.c`): as
  extra targets of `rfid write` for EM4100 data, after R9.0.1–R9.0.6. Uses the existing field-gap
  write and the held-field capture (R6.5 part 3); kernel unchanged if the timings fit.
- **R9.0.8 RAW read** (`rfid raw FILE`, `.ask.raw`/`.psk.raw` as Unleashed, varint pairs) on the
  SD card, for tags no decoder knows; RAW emulate optional. Writes go to the SD card only.
- **R9.0.9 `lfrfid` app:** the read screen shows the protocol name and the rendered data; saved
  files of every protocol load, write and emulate; "Add manually" lists all 25 protocols (data
  entered as hex bytes). Black title bar as every GUI app.

Acceptance:
- AC9.0.1 `make lfrfidtest`: all 25 protocols round-trip encoder → decoder on the PC, with jitter
  and both polarities, and no protocol decodes another's frame.
- AC9.0.2 On the Flipper, with real tags or a T5577 written by `rfid write PROTO …`: each protocol
  is read, written, read back, and emulated to a second reader (the owner's stock/Unleashed Flipper
  or a reader) — at least EM4100 RF/32, H10301, Indala26, FDX-B, Viking, Paradox, Keri, Nexwatch,
  PAC/Stanley, Indala224; the others where a T5577 can carry them. Results table in PORTING.md.
- AC9.0.3 A PSK read of 10 s and an FSK emulation of 60 s lose no edge and no segment (overrun
  counter 0).
- AC9.0.4 While `rfid` reads (streaming, all decoders) or emulates PSK, L1 (button → pixel) stays
  within budget and the launcher scrolls without a visible hitch; `lat` numbers in the PR.
- AC9.0.5 Kernel RAM growth for R9.0.4–R9.0.6 together ≤ 32 B (`ramcheck`); no internal flash
  writes at run time (`flashcheck`).

### 9.1 Hidden camera finder `camfind`, version 1 (gadget, F.5)

**Depends on:** 6 (UART bridge, IR, LED), 8 (Sub-GHz). Wi-Fi needs the optional ESP32 devboard.

- **R9.1.1** Wi-Fi via the ESP32 devboard: a small passive-scan firmware for the devboard (part of
  this project, GPL) speaks a line protocol on the UART: `AP <bssid> <rssi> <ch> <ssid>` and
  `STA <mac> <rssi> <ch> <bssid>` per sighting, `CH <n>` to lock a channel for locating, and
  `RATE <mac> <bytes/s> <frames/s>` every 250 ms for the station followed by `TRACK <mac>` (R9.1.8). Channel
  hopping and sniffing run on the ESP32; the Flipper only reads lines.
- **R9.1.2** Scoring: the MAC prefix is looked up in a `const` sorted table of camera and camera-
  module vendors (3-byte OUI + vendor index; ≈ 300 entries ≈ 1.5 KB of flash, generated from the
  public IEEE registry by a build script with a hand-kept list of camera makers); SSID patterns
  (`IPC`, `CAM`, `HD-`, setup SSIDs of common camera apps) add to the score; locally administered
  (random) MACs are shown as "unknown". Three levels: likely camera, IoT module, other.
- **R9.1.3** Sub-GHz sweep: steps across the CC1101 bands, keeps the peak RSSI per channel, and lists
  steady carriers above the noise floor (uses the frequency-analyzer code of Phase 9).
  - **Cellular mode:** sweeps only LTE band 20 uplink (832–862 MHz) and band 8 / GSM-900 uplink
    (880–915 MHz) at the widest RX filter, with peak hold and a burst counter per 1 MHz step. The
    app first asks the user to put their own phones in airplane mode; repeated bursts then mean a
    cellular transmitter in the room (SIM cameras upload on motion, so walking in triggers them).
    It can't decode LTE and can't see bands 3/7; the front end's filters reduce sensitivity off
    the 868–915 MHz path, which is fine at 1–3 m (to measure on hardware).
- **R9.1.4** IR hint and lens-glint strobe as in F.5, clearly labelled as weak.
- **R9.1.5** Locate mode ("hot/cold"): pick one device; the Flipper locks the radio to it (ESP32
  channel lock, or the CC1101 frequency) and maps smoothed RSSI to beep pitch and rate and to
  vibro, with a bar on screen. A press of OK marks the current strength as the reference.
- **R9.1.6** Results can be saved as a report in `/ext/apps_data/camfind/` (time, devices, scores).
- Acceptance:
  - AC9.1.1 A Wi-Fi IP camera (any common brand) in a room is listed as "likely camera" within
    30 s of starting the scan, and locate mode leads to it from 5 m.
  - AC9.1.2 A 433 MHz transmitter sending every few seconds is found by the sweep and located.
  - AC9.1.3 Without the devboard, the app says Wi-Fi is unavailable and still runs the other scans.
- Responsiveness: locate mode updates sound and bar ≥ 10 times per second; L1 holds.
- RAM: app ≈ 5 KB with a scan list of at most 48 devices × 24 B ≈ 1.2 KB; tables in flash; kernel
  0. Flash ≈ 15 KB installed (plus the ESP32 firmware, which lives on the devboard).
- **R9.1.7 IR intensity meter** (needs a small external sensor; the built-in IR receiver can't
  measure intensity, see F.5):
  - Sensor: an IR photodiode with a daylight-blocking filter (e.g. BPV10NF, SFH 203 FA, BPW34 with a
    940 nm-pass filter) from 3.3 V (header pin 9) through 100 kΩ–1 MΩ to an ADC-capable header pin
    (default pin 7 = PC3; pins 3, 4, 15, 16 also have ADC inputs; pin 2 = PA7 has one too but is
    the stock firmware's external IR transmitter pin, so the app warns when it is chosen), or a
    phototransistor with a
    10–47 kΩ load. A 2–3 cm black tube over the sensor narrows its view so it points.
  - Driver: `/dev/adc` (added to R6.4; header ADC pins only). Steady level uses the ADC's
    **hardware oversampling** (up to 256×) so one 16-bit result replaces 256 samples in RAM; the
    analog watchdog can raise an event above a threshold with the CPU asleep. Modulation analysis
    uses circular DMA at 4 kHz into a 2 × 64-sample (256 B) buffer in the app (R4b.5).
  - Display: a bar graph of the level above the "zero" reference (OK = zero at a blank wall),
    peak hold, and a tone whose pitch follows the level (vibro at the peak). A Goertzel filter on
    the DMA blocks shows whether the IR is steady or modulated (50/60/100/120 Hz flicker, or kHz
    PWM dimming), which tells an IR illuminator from a lamp or a remote.
  - **Active glint mode:** the Flipper's own IR LED sends 940 nm pulses at a few kHz and the meter
    measures only the reflected part in step with them (synchronous detection, rejects ambient
    light). A camera lens reflects light back towards its source much more than a wall does, so
    a lens shows as a sharp peak while sweeping, even with the camera's own IR off. Day cameras
    help further: their IR-cut filter is a dichroic mirror that reflects 940 nm rather than
    absorbing it. The IR LEDs and the GPIO header are both on the top edge, so emitter and sensor
    are nearly coaxial, as retro-reflection needs.
  - **Point and sweep procedure** (shown as help in the app):
    1. Turn the room lights off and close the curtains (night-vision IR LEDs switch on in the dark,
       and ambient IR goes down).
    2. Point the sensor at a blank wall and press OK to zero.
    3. From 1–2 m, sweep slowly left to right, then up and down, across likely places: smoke
       detectors, clocks, chargers and USB adapters, TV and set-top box, air vents, picture frames,
       shelves, the bed head.
    4. Where the tone rises, step closer and narrow the sweep until the peak is sharp.
    5. Repeat in active glint mode to look for a lens that has no IR LEDs of its own.
    6. Confirm by eye and with the phone camera; then check the Wi-Fi and BLE lists for a
       matching device.
  - Limits stated in the app: a camera with its IR off and its lens hidden behind dark glass may
    not show; remotes, chargers' LEDs and some lamps also emit IR (the modulation readout helps).
  - AC9.1.4 With a BPV10NF on pin 7, a 940 nm night-vision camera's illuminator is located from
    3 m in a dark room, and a camera lens with its IR off gives a clear peak in active glint mode
    at 1 m.
  - RAM: 256 B DMA buffer in `camfind` while the meter runs; kernel ≤ +16 B for `/dev/adc`.
- **R9.1.8 Wi-Fi motion correlation** (owner approval 2026-11-15; needs the devboard):
  - The devboard firmware counts, per station, data-frame bytes and frames it transmits (monitor
    mode, all frames, encrypted or not; length and transmitter address are always readable).
  - "Test this device" on a listed station sends `TRACK <mac>` and locks its channel. The app
    shows "stand still" for 10 s, then "wave your hand / walk in front of likely spots" for 10 s,
    twice. It compares the uplink rate in the move windows with the still windows (ratio of
    medians, and the correlation of the per-250 ms rate with the window pattern). A ratio ≥ 2 in
    both rounds marks the station "watches this room", the strongest result `camfind` has.
  - The classic ESP32 sees 2.4 GHz only; 5 GHz cameras need an ESP32-C5 or a Linux module with a
    monitor-mode adapter speaking the same line protocol.
  - Prior art: research on detecting hidden Wi-Fi cameras by the causality between user motion and
    traffic (e.g. SnoopDog), and on locating hidden IoT devices by their traffic (e.g. Lumos).
  - AC9.1.5 A Wi-Fi camera streaming to a cloud service, in a WPA2 network the devboard is not
    part of, is marked "watches this room" by the test; a phone idling in the same network is not.
  - RAM: two 40-entry rate rings (2 × 40 × 2 B) + counters ≈ 200 B in `camfind` while testing.
- **R9.1.9 Optional 2.4/5.8 GHz modules** (analog video transmitters):
  - RX5808 (5.8 GHz FPV receiver): tuned over its 3-wire interface on header GPIO, RSSI output
    to an ADC pin through `/dev/adc`; sweeps the 40 standard FPV channels (Bands A, B, E, F, R)
    and the locate mode works on the strongest one.
  - nRF24L01+ (2.4 GHz) on SPI: RPD (power > −64 dBm) per 1 MHz channel, many sweeps summed into
    a histogram. Shown as "2.4 GHz energy", not as cameras.
  - Unverified idea: the STM32WB55's own radio test commands (BLE stack, R16.1.0) may give RSSI on
    2402–2480 MHz without any module; check the stack's ACI HAL test commands in Phase 16.
  - RAM: one sweep row (40 × 1 B or 80 × 1 B) in `camfind` while in use.

### 9.2 Radio canaries: Sub-GHz, 125 kHz, infrared (section M)

**Depends on:** 6 (IR), 8 (Sub-GHz, LF RFID), 7b (alert service), 12d for pocket mode.

- **R9.2.1 `subghz` canary:** receive only. Watches one band or hops a list of presets (433.92,
  868.35, 315, 915 MHz and the user's list), dwell ≥ 50 ms per preset. Each burst above the noise
  floor becomes an event: frequency, peak RSSI, length, and the decoded protocol and fields when a
  Phase 9 decoder recognises it.
  - "New repeating transmitter": the same decoded ID or the same burst shape seen ≥ 3 times in
    10 min and not seen in the last session.
  - **Jamming alarm:** RSSI above a threshold for ≥ 2 s on one preset (e.g. while you lock a car);
    the threshold is learned from the first 10 s of the noise floor.
  - Pocket mode: CC1101 carrier sense (CS) on a GDO interrupt wakes the CPU (M.1, tier 3).
- **R9.2.2 `rfid` canary:** the LF front end's comparator capture (pulse engine, R4b.7) measures an
  external 125 kHz-band field: frequency (to ±1 kHz), on time, polling period. Shows "reader
  field 125.0 kHz, polls every 300 ms". It never answers (no emulation). Exclusive with NFC on the
  shared pin, as usual.
- **R9.2.3 `ir` canary:** the IR receiver logs every frame with its decoded protocol, address and
  command (Phase 7 codecs), or the raw timing when unknown; a counter per (protocol, address)
  shows "something keeps sending Samsung power".
- **R9.2.4 `wifi` canary (optional, ESP32 devboard):** the passive-scan firmware of R9.1.1 gains
  `DEAUTH <bssid> <count/s>`, `PROBE <mac> <ssid>` and the evil-twin rule (an SSID seen with a new
  BSSID and a different security mode). Passive only, like `camfind`.
- Acceptance:
  - AC9.2.1 A 433.92 MHz weather station shows as a repeating transmitter within 10 min; a
    signal generator (or a second Flipper's carrier test, inside R8.2 limits) held 3 s triggers the
    jamming alarm, also in pocket mode with the screen off.
  - AC9.2.2 A stock Flipper in LF read mode, and an access-control reader, show as a 125 kHz
    field with a polling period that matches a scope measurement within 10 %.
  - AC9.2.3 A TV remote's key presses appear decoded.
- Responsiveness: AC9.2.R L1 holds while the Sub-GHz canary hops presets.
- RAM: app module ≈ 3 KB while running (event list of 32 entries, noise-floor table per preset);
  kernel 0 B new (pulse engine and CC1101 state exist); in pocket mode only the parked app's
  swap-out remains (12d).

## Phase 10: USB device classes

**Depends on:** 4b.

- **R10.1** Composite USB device: CDC console (always), a second CDC for the UART bridge or RPC,
  HID keyboard + mouse, HID FIDO (used in Phase 17).
  - Interfaces are switched at run time with a re-enumeration, keeping the console interface.
  - Endpoint buffers in the USB PMA only (A.6).
  - Descriptors are `const`.
- **R10.2** `/dev/hidkbd`, `/dev/hidmouse`: write reports; LED (caps lock) state readable.
- **R10.3** Mass storage: exposes `/dev/hd4` (`/home`) as a USB drive.
  - Only when `/home` is unmounted (`EBUSY` otherwise).
  - SCSI READ/WRITE stream sector by sector through the shared sector buffer.

Acceptance:
- AC10.1 The console keeps working while HID types 10,000 characters into a PC editor without loss.
- AC10.2 The SD `/home` partition mounts on Linux, Windows and macOS as a drive; after eject and remount on
  the Flipper, `fsck` is clean.

Responsiveness: AC10.R HID report latency ≤ 2 ms from the app's write (USB frame-bound); L1 holds while HID typing runs.

RAM: kernel ≤ +150 B. Flash: ≈ +8 KB kernel.

## Phase 11: App ports III: BadUSB and the USB modes

| Stock app | MINIX mapping |
|---|---|
| Bad USB (DuckyScript, layouts) | `badusb` app + `badusb` command (scriptable from the shell), group `usb`; stock `badusb` script engine ported; layouts from SD |
| USB-UART bridge, mass storage | Parts of the `gpio` app and a `usbmode` app |

Acceptance: AC11.1 Stock example DuckyScripts run unmodified on Windows and Linux hosts with
US and DE layouts.

Responsiveness: AC11.R A BadUSB script can be stopped with the Back button within 100 ms.

RAM: app ≈ 4 KB. Flash: ≈ 15 KB installed.

### 11.1 USB reader modes I: RFID, iButton, infrared, Sub-GHz (section G)

- **R11.1.1** `usbmode` profiles (R10.1 re-enumeration): `rfid`, `ibutton`, `ir`, `subghz`,
  each = console CDC + one function; a profile's descriptors are `const`; switching back to
  `default` restores console + HID.
- **R11.1.2** Keyboard-wedge output for RFID and iButton: ID format setting (hex, decimal, with or
  without prefix and Enter), keyboard layout as in BadUSB; repeat suppression (same ID within 2 s
  is typed once).
- **R11.1.3** Serial protocols as G.2: RFID ASCII lines; DS2480B 1-Wire master emulation; IR
  Toy sampling protocol; Sub-GHz raw pulse stream with TX limited by R8.2 (band and duty limits
  apply to the PC too).
- **R11.1.4** All four modes run in the device task (G.3 option B): wedge formatting from `const`
  tables into the HID buffer in the PMA; serial protocols as kernel state machines; IR and
  Sub-GHz pulse rings in a 512 B block lent by the FS cache for the mode's duration. Mode states
  share one `union`. `usbmode` configures and exits.
- **R11.1.6** USB input parsed in the kernel (serial commands, pulse files) is bounds-checked
  (I2); AC11.1.5 runs a fuzzer from the PC for 1 h per mode with no crash or hang.
- **R11.1.5** A status-bar icon shows the active mode; the screen stays free for other apps
  except those needing the same radio (`EBUSY`).
- Acceptance:
  - AC11.1.1 An EM4100 card and an HID Prox card type their IDs into a PC text editor on Linux,
    Windows and macOS.
  - AC11.1.2 `owfs` (or `digitemp_DS9097U`) on Linux reads a DS1990A and a DS18B20 through the
    Flipper.
  - AC11.1.3 `ir-ctl -r` on Linux receives a TV remote, and `ir-ctl -s` replays it.
  - AC11.1.4 A recorded Sub-GHz RAW pulse file streamed from the PC transmits within the allowed
    bands only; a request outside them fails with an error on the serial line.
- Responsiveness: AC11.1.R Card or key placed → ID typed on the PC ≤ 300 ms (LF read time
  dominates); pulse-stream overrun = 0 at 1 MB of IR/Sub-GHz capture with L1 held.
- RAM: kernel ≤ +64 B static (shared with 14.1 and 16.2 via the mode `union`); 0 B user; 512 B
  borrowed from the FS cache only during IR/Sub-GHz modes. Flash ≈ +8 KB kernel, ≈ 4 KB installed
  (`usbmode`).

### 11.2 USB canary: what does this port try to do? (owner request, 2026-11-15)

The first canary of section M; the M.1 rules apply. It is the `usb` module of the `canary` app.

**Goal:** plug the Flipper into a USB port the owner does not trust (a hotel or airport charger, a
car, a kiosk, a stranger's computer) and see what the port tries to do, without the Flipper ever
doing anything back. Module `usb` of the `canary` app (section M), group `usb`.

The STM32WB55 USB is device-only, so the canary watches the **host** side of a link. Watching a
device plugged into the Flipper (a pendrive, a "charging cable" with a chip inside) needs an
external USB host chip; see Deferred (MAX3421E).

- **R11.2.1 Verdict levels,** shown on screen as they are reached, with the time since attach:
  1. **Power only:** VBUS present, no bus reset and no SOF within 3 s. Where the BQ25896 charger
     reports its BC1.2 result (`VBUS_STAT`: SDP, CDP, DCP, non-standard), show it; whether the
     charger sees D+/D− on this board is checked against the schematic, and the line is left out
     if it does not.
  2. **Host alive:** bus reset and SOF seen, no SETUP (a host that powers the port but never
     enumerates).
  3. **Enumerates:** the device descriptor was read; the address was set.
  4. **Configures:** `SET_CONFIGURATION` received, so a driver on the host wants to use the device.
  5. **Uses the function:** traffic on a non-control endpoint of the presented persona (R11.2.3).
- **R11.2.2 Trace:** every SETUP packet is recorded as 8 B + a 4 B millisecond timestamp, together
  with bus resets, suspend/resume and the first SOF. Also kept: string descriptor indexes and
  language IDs asked for, Microsoft OS descriptor requests (string `0xEE`, vendor requests),
  device-qualifier and BOS requests, `wLength` of the first `GET_DESCRIPTOR(DEVICE)`, and the
  number of re-enumerations. The app shows a live list (newest first) and a summary screen.
- **R11.2.3 Personas** (chosen in the app before arming; each is a `const` descriptor set with our
  own IDs, G.1):
  - `bare` (default): one vendor-specific interface (class `0xFF`), so no host driver binds. Pure
    observation of enumeration.
  - `storage`: a mass-storage device with a small read-only medium generated from `const` data
    (an empty FAT12 volume). Logs which SCSI commands and which sectors the host reads, i.e.
    whether it mounts and scans the medium on its own. WRITE commands fail with "write protected"
    and are logged.
  - `keyboard`: a HID keyboard that never sends a key. Logs `SET_IDLE`, `SET_PROTOCOL` and LED
    output reports (some hosts set Num Lock on attach).
  - `serial`: a CDC-ACM port. Logs `SET_LINE_CODING`, `SET_CONTROL_LINE_STATE` (DTR: something
    opened the port) and the first 64 B the host writes, shown as hex.
  - `network` (after Phase 19b): a CDC-NCM/ECM interface that never answers. Logs EtherType and,
    for DHCP and DNS packets, the requested options and names, decoded from the first frame
    bytes only. Hosts often send their own host name in DHCP option 12; it is shown as found.
  The canary never sends data on a function endpoint except what the class requires to stay
  enumerated (CSW status replies, the empty HID report, NCM notifications). It never types,
  never answers network traffic, never accepts writes.
- **R11.2.4 Host fingerprint (a guess, labelled as one):** request order and the early `wLength`
  values differ between hosts (for example Windows asks for 64 B of the device descriptor first
  and for MS OS descriptors; Linux asks for the full 18 B and for the device qualifier). Matching
  is a scored table:
  - built-in rules in a `const` table;
  - optional extra rules in `/ext/apps_data/canary/usb/rules.tsv` (K, TSV), so new traces add
    rules without a rebuild;
  - the result is shown as "looks like: Windows 10/11 (score 7/9)" or "unknown"; the raw trace is
    always kept, because the guess can be wrong or deliberately spoofed.
  The first built-in rules come from traces recorded on Windows, Linux, macOS, Android and a
  plain charger, saved under `tests/canary/usb/` in the PR that adds them.
- **R11.2.5 No console in this profile (security).** Every other USB profile keeps the CDC console
  (G.1); the canary profile **drops it** (G.3 "mode without the console"), so an untrusted host
  never sees a shell. When the app exits, the previous profile is re-enumerated.
- **R11.2.6 Implementation (G.3 option B):** control-request handling for the personas and the
  trace recorder run in the device task. Trace entries go into a ring in one 512 B block lent by
  the FS cache for the session (42 entries), like the IR and Sub-GHz modes. The app drains the
  ring, keeps the summary, and writes the log. Mode state shares the G.3 `union`.
- **R11.2.7 Bounds (I2):** every host-supplied `wIndex`, `wLength`, LBA and transfer length is
  checked before use; the storage persona's medium is `const` and has no write path. A fuzzer run
  from a Linux host (raw control requests through `libusb`, random SCSI CDBs) runs for 1 h per
  persona with no crash, hang or out-of-range read.
- **R11.2.8 Log:** on Back (or after 10 min), the session is written to
  `/ext/apps_data/canary/usb/YYYYMMDD-HHMMSS.tsv`: one header line with the verdict, persona and
  fingerprint, then one line per event. SD only; **no internal flash write** at any point (rule
  L). With no SD card, the session stays on screen only.
- **R11.2.9 Power:** with `bare` and verdict 1, the app can be left running as a charging-port
  check; the backlight follows the normal timeout.

Acceptance:
- AC11.2.1 A USB charger gives verdict 1; a Linux PC gives verdict 4 with `bare`, and its trace
  matches `usbmon` captured on the PC for the same session.
- AC11.2.2 With `storage`, a Windows and a Linux PC that auto-mount show their reads of the boot
  sector and FAT in the log; `dd of=/dev/sdX` from Linux fails with a write-protect error and is
  logged.
- AC11.2.3 With `serial`, `screen /dev/ttyACM0 115200` on Linux shows up as DTR set, line coding
  115200 8N1 and the typed bytes; no shell prompt appears on the host.
- AC11.2.4 The fingerprint names Windows, Linux, macOS and Android correctly on the recorded
  reference traces, and says "unknown" for a charger.
- AC11.2.5 The fuzzer of R11.2.7 passes.
- AC11.2.6 A full session leaves the internal-flash write counters unchanged (R2b.11 counters).

Responsiveness: AC11.2.R A SETUP packet is recorded in the USB interrupt in ≤ 5 µs; the list on
screen updates within 100 ms of an event; L1 holds while a host floods control requests.

RAM: kernel ≤ +32 B static (in the mode `union`); 512 B lent by the FS cache while armed; app
≈ 3 KB only while running. Flash ≈ +3 KB kernel (personas, recorder), ≈ 6 KB app (installed, or
run from the SD card).

**Notes for the system manual:** what each verdict means in plain words ("this port only gives
power" … "a program on that computer opened the serial port"); that the fingerprint is a guess;
how to read the TSV log; that the canary never sends keystrokes, files or network replies and
hides the console; that a malicious host can still lie about itself, so "power only" means "did
nothing during this session", not "is safe".

---

## Phase 12: Swapping to the SD card

**Goal:** idle processes give their RAM back. With 12b, swapped data is also confidential and
tamper-evident.
**Two steps (owner decision, 2026-11-16):** swap ships **first unencrypted, as an option**
(12a, 12d, 12e with R12.28), and encryption (12b) is added later as a separate step. The plain step
needs no RNG and no AES1 driver, so it can start right after M1.
**Depends on:** Phase 1 (R1.4 especially). Phase 2a SHOULD come first (it frees `w_buf`, or 12b
decides to keep it as the crypto buffer). Only 12b depends on Phase 3 (R3.1 RNG).
Design rationale: report Part 4.

### 12a. Swap processes blocked in MM

- **R12.1** Swap device:
  - a root-only `svrctl(MMSWAPON, dev)` sets the raw swap partition, and `/etc/rc` calls it for
    `/dev/hd3`;
  - no swap file; no FS involvement in swap I/O.
- **R12.2** Slots:
  - slot *i* belongs to `mproc[i]`, with fixed size `SWAP_SLOT` (192 KB), so the partition must
    be at least 16 × 192 KB = 3 MB;
  - sector 0 of a slot is the header: magic, pid, `D.start`, data length, stack start, stack
    length, write counter, GCM tag.
- **R12.3** Eligibility:
  - the user process is blocked in `WAITING` or `PAUSED` state;
  - not MM, FS or INIT; not traced; not in a shadow group (`p_shadow == 0` and no other member
    at its address); `p_physio == 0`;
  - text in the installed region or the flash cache (not in RAM);
  - resident for at least 1 s;
  - no armed DMA into or out of it (R4b.5) and no open pulse-engine or radio stream (R4b.7);
    a process waiting in `poll()` on a radio device is not swapped.
- **R12.4** Swap-out:
  - MM writes `[D.start, brk)` and `[sp, S.end)` to the slot by `DEV_WRITE` straight to the
    WINCHESTER task with `PROC_NR` = the process;
  - then the kernel sets `P_SWAPPED` (new `SYS_SWAP` call) and MM frees the whole range
    `[D.start, S.end)`;
  - the text map and `mproc` entry stay, so `fcache` keeps the text pinned.
- **R12.5** Trigger:
  - only when `new_mem`, `do_fork` or `do_brk` cannot allocate;
  - victims are chosen to create the largest contiguous hole, ties broken by longest blocked;
  - then retry the allocation once.
- **R12.6** Swap-in:
  - before MM replies to a swapped process (`wait`/`waitpid` completion, `pause` wake, signal
    delivery that needs the stack), MM claims exactly `[D.start, S.end)` with a new
    `alloc_mem_at()`, swapping out eligible occupants if needed;
  - then it reads the image back, **zeroes the gap**, and clears `P_SWAPPED` (which also forces
    an MPU reload);
  - if the range can't be had, the wake stays pending and is retried whenever memory is freed;
  - `SIGKILL` and exit of a swapped process just free the slot, with no swap-in.
- **R12.7** Kernel safety net:
  - with `P_SWAPPED` set, the process is never scheduled;
  - `umap`/`numap` return 0 for it and `cp_mess`/`mini_send` refuse delivery (`E_BAD_DEST`), with
    a console warning;
  - none of these may ever write into its range.
- **R12.8** `free -v` lists swapped processes and the slot use.

- **R12.28 Plain swap first (owner decision, 2026-11-16).** Until 12b lands, swap runs
  unencrypted and only when asked for:
  - `/etc/rc.conf` has `swap=off|plain|crypt`. Before 12b the default is `off`, and `crypt` is
    refused with a console warning (treated as `off`). After 12b the default becomes `crypt`, and
    `plain` stays available as an option;
  - integrity in `plain` mode: the slot header holds a **CRC-32 of the image** in the place of the
    GCM tag, computed by the STM32 CRC unit (0 B RAM; software CRC-32 in the QEMU build). A
    mismatch or read error on swap-in kills the process (SIGKILL), as R12.12. This catches card
    corruption, not a deliberate attacker;
  - confidentiality in `plain` mode: set-uid/set-gid images and packages whose manifest says
    `Secret: yes` are **not eligible** (R12.3 gets this extra condition), so their RAM never
    reaches the card in clear. `passwd` and `su` are such images;
  - the slot layout (R12.2) is the same in both modes, so 12b changes no on-card format (nothing
    in swap survives a reboot anyway);
  - `free -v` shows the mode (`swap: plain` / `swap: crypt`).

### 12b. Encryption (later, separate step; MUST be in place before swap is on by default)

- **R12.9** AES1 driver (kernel, used by the WINCHESTER task):
  - AES-GCM;
  - the key is generated with the RNG at boot and written directly into AES1's key registers;
  - no copy in SRAM after loading (clear the temporary);
  - AES1 is reserved for swap. U2F does not need it: its keys stay in the CPU2 secure enclave
    and signing runs on the PKA (R17.1).
- **R12.10** Nonce:
  - 64 random bits per boot, plus the slot's 32-bit write counter, which MM keeps in RAM and
    increments on every swap-out;
  - associated data: slot, pid, `D.start`, lengths.
- **R12.11** Swap-out:
  - encrypt on the bounce buffer, or in place in the outgoing image (re-decrypt it if the write fails);
  - write the tag to the slot header from MM's `mbuf`.
- **R12.12** Swap-in:
  - decrypt and verify;
  - on a tag mismatch, the process MUST be killed (SIGKILL), never run.
- **R12.10a** Per-chunk encryption, so R12.29 (write only what changed) keeps working: each 4 KB
  chunk has its own nonce (boot nonce, slot, chunk number, the chunk's own write counter) and its
  own tag, and only changed chunks are re-encrypted. 48 tags × 16 B do not fit in one sector, so
  the header grows to 2 sectors (on the card only). The chunk counters live in the header, and the
  header has its own tag over the chunk counters and tags, bound to the slot's RAM write counter
  (R12.10), so an old chunk or header can't be replayed; RAM stays as R12.26. The header must not hold plaintext CRCs in `crypt` mode (they leak about the
  data). Decide this before 12b's format is fixed.
- **R12.13** QEMU build: software AES-GCM behind the same interface (flash only).

### 12c. Later extension (separate sign-off)

- **R12.14** Swap processes suspended in FS (tty reads, pipes, `/dev/buttons`):
  - `numap`/`umap`/`cp_mess` on a swapped target return a "deferred" result;
  - TTY, flipdev and FS keep the request pending and ask MM to swap the process in;
  - after swap-in the kernel pokes the driver to retry.
- **R12.15** Swap long-lived shadow copies (real `fork` without `exec`) instead of keeping them in RAM.

### 12d. Background apps: four tiers (owner request, 2026-11-14)

The stock firmware has no swap, so its background work lives inside one always-running image. Here
background work must either be very small or be swapped cleverly. Each background job picks the
**cheapest tier that does the job**. The tier is declared in the package manifest
(`Background: event|hibernate|park|stream`) and checked by the launcher and `pkg`.

| Tier | While waiting | RAM while waiting | Process slot while waiting | Back to work | Needs swap? |
|---|---|---|---|---|---|
| 0 `event` | no process; the trigger is a `timed` timer (F.2) held in RTC Alarm A, or an INIT event | 0 B | no | exec of installed text, ≈ 50–150 ms | no |
| 1 `hibernate` | no process; the app saved a small state blob and exited | 0 B (blob on SD) | no | exec + read blob, ≈ 100–200 ms, at **any** address | no |
| 2 `park` | process blocked in MM (`sigsuspend`), image swapped to SD when memory is needed | 0 B once swapped (whole image until then) | **yes** (one of 13 user slots) | swap-in at the **same** address, L6 ≤ 600 ms | yes (12a/12b) |
| 3 `stream` | live radio, DMA or USB stream (never swappable, R12.3) | the logic sits in a kernel device task (section G), ≤ 64 B; a user part is ≤ 4 KB or exits | only if a user part stays | immediate | no |

- **R12.16 Tier rule.** An app MUST use tier 0 or 1 when it can.
  - Anything with a deadline (alarms, reminders, sensor alarms, logging at a set time) MUST be
    tier 0: a swapped image can wait for its address range (R12.6), a fresh exec cannot.
  - Tier 2 is for stock ports and other apps whose state is a large, pointer-rich heap that
    can't be serialized cheaply.
  - Tier 3 user parts above 4 KB need a measured reason in the PR.
- **R12.17 Why there is no "resident stub" process.** The smallest MINIX process costs about
  1.5–2 KB (minimal stack, libc data, `proc`/`mproc`/`fproc` entries) and one of only 16 process
  slots (`NR_PROCS`, 13 for users). So the "tiny resident stub" of an app is not a process: it is
  a registration that costs 0 B of process RAM. The stub forms are a `timed` timer file (tier 0),
  the hibernation record (tier 1), the parked `mproc` entry (tier 2), or kernel device-task state
  (tier 3).
- **R12.18 Hibernate API** (in `libapp`, flash only; works without swap, so it MAY land with
  Phase 7b):
  - `app_hibernate(const void *state, size_t len, const struct app_wake *wake)` writes `state`
    (≤ 4 KB) to `/var/hib/<uid>/<app>` (mode 0600, owner the app's uid; on the SD card, never in
    internal flash), registers the wake sources, and exits with status 0;
  - wake sources: a `timed` timer (`Action: run` with `--resume`), "user opened the app from the
    launcher", a `notify_post()` reply. More sources (GPIO edge, sensor threshold) are added by the
    phase that adds the driver, as INIT events like `SIGRTC`;
  - `app_resume(void *buf, size_t max)` returns the blob's length (0 = fresh start) and removes the file;
  - the blob holds **no pointers** (offsets and ids only), so the new process may live anywhere:
    this is the relocation answer for tier 1;
  - a blob is not encrypted: apps MUST NOT put secrets in it (keys go to the CPU2 enclave, R17.1).
    A secret-holding app uses tier 2 with `Secret: yes`; its image is encrypted with
    `swap=crypt` (12b) and stays in RAM with `swap=plain` (R12.28);
  - an SD write of ≤ 4 KB goes through the FS cache as idle work; 0 flash writes.
- **R12.19 Park API** (in `libapp` and in the `libfuri` main loop):
  - `app_park(const struct app_wake *wake)` blocks the process in `sigsuspend()` with the given
    timer (`alarm`/`setitimer`), so it is in the `PAUSED` state, which 12a may swap. It returns
    when the app is brought to the front again (`SIGFG`, new) or its timer fires;
  - a parked app MUST first release `/dev/lcd`, `/dev/buttons` and every radio or DMA device; R12.3
    still applies, and a process holding one is refused with `EBUSY`;
  - the `libfuri` shim does this for stock ports: when the launcher sends `SIGBG` (new), the main
    loop leaves its `/dev/buttons` read, which is an FS wait and not swappable before 12c, closes
    its devices and calls `app_park()`. Timers created with `furi_timer` become the park timer
    (the earliest one); when it fires, the app runs at background priority, without screen or buttons;
  - parking needs no swap: without a swap device, a parked app keeps its RAM (see R12.24).
- **R12.20 Launcher support.**
  - Apps with `Background: park` are parked on long-press Back instead of being ended. They end
    from their own menu, or from the launcher's new **Running** list (Back on an entry = end with
    `SIGTERM`, then `SIGKILL` after 3 s, as R4b.8).
  - Other apps keep the stock behaviour: long-press Back ends them.
  - The Running list shows each parked app with its state (`in RAM`, `on SD`, `waiting for
    memory`) and its RAM, and is the place to free memory by hand.
  - Decided by the owner (2026-11-14): long-press Back parks apps that declare `Background: park`.
    The app's own menu SHOULD also offer "Quit", and the launcher shows a one-time hint the first
    time an app is parked ("Parked. End it from Running.").
- **R12.21 Size guidance** (for app authors; the manual repeats it):
  - tier 2 image (data + heap + stack) ≤ 16 KB SHOULD be the target: its swap-in is ≈ 55 ms with
    single-block SD (≈ 300 KB/s) and ≈ 15–25 ms with multi-block (R4b.9). 32 KB is ≈ 110 ms and
    ≈ 30–50 ms. AES1 GCM adds about 1 ms per 16 KB;
  - all tier 3 user parts together ≤ 8 KB.

- **R12.27 Example: GPIO sensor and Bluetooth at the same time** (owner note 2026-11-14; future,
  low priority, after Phase 15; not in the task queue). Simple background jobs combine tiers and
  need almost no user RAM:
  - **Periodic (a thermometer that the phone reads):** tier 0. A `timed` timer runs
    `thermo --log` once a minute (≈ 2 KB for under 1 s, R7b.4). It writes the sample to the SD
    log and to a GATT characteristic (Environmental Sensing) **held by the CPU2 stack**. CPU2 keeps
    the connection, answers reads and sends notifications by itself. User RAM while waiting:
    0 B. `bled` (≈ 6 KB) runs only for pairing.
  - **Continuous (GPIO edges or ADC streamed to the phone at 10–50 Hz):** tier 3. A small kernel
    bridge from the sensor driver to the GATT characteristic, like `btbridge` (16.1): state
    ≤ 64 B in the device-task union. User RAM: 0 B. Simpler alternative: a Berry script (7c) that
    stays resident at ≈ 6–8 KB. It is too short-lived between samples to be worth parking.
  - **Concurrency:** BLE radio timing is on CPU2, and the sensor is read by timer/DMA hardware
    (H.2), so neither masks interrupts for the other (L3, L4). The one shared resource is the
    pulse engine (DHT, IR, async Sub-GHz, R4b.7): while it is busy, a DHT read
    returns the cached value (H.2). Prefer I2C or 1-Wire sensors for always-on jobs.

### 12e. Placement, responsiveness and the card

- **R12.22 Two-ended placement (relocation without an MMU).** The Cortex-M4 MPU protects but
  does not translate. Data and stack hold absolute pointers (heap, frames, return addresses, GOT),
  so neither swap-in nor compaction can move an image. A base register (`r9` with
  `-msingle-pic-base`) would only relocate globals, not heap pointers, and is rejected for image swap.
  Instead, MM places processes so that a swapped image usually finds its range free again:
  - the foreground app (started through `FG_SET`, R4b.8) gets the **lowest** hole that fits (first fit);
  - all other user processes, including the launcher, shells, daemons and `timed` children, get
    the **highest** hole that fits (last fit);
  - so foreground launches come and go at the bottom, and parked images return to the top;
  - this is a rule in `alloc.c` with a flag passed by `exec`, and costs 0 B RAM.
  Tier 1 avoids the problem completely (R12.18).
- **R12.5 amended: when swap-out runs.** Two triggers, and both write only to the SD card:
  - **(a) on demand**, as before: `new_mem`, `do_fork` or `do_brk` can't allocate. The victim
    must have been eligible for ≥ 1 s;
  - **(b) reserve** (rule R0.8d): at idle priority, when free RAM is below the reserve (default
    16 KB, `svrctl(MMSWAPRES)`, 0 = off). The victim must have been eligible for ≥ 10 s (parked
    apps first, then idle shells, then the launcher if an app is in front). At most 256 KB per
    minute are written this way. A victim whose wake is already pending is skipped.
  This replaces Part 4's "no background swapping": the reserve is what makes L6 hold, and with
  (b)'s limits SD wear stays small. Typical use is 50–200 swap-outs a day of 8–32 KB, so ≤ 6 MB a
  day, which is nothing for a card's wear levelling.
- **R12.23 Swap-in priority.** A swap-in the user is waiting for (an app chosen in the Running list,
  the launcher after an app exits, L7) runs in the interactive class: it goes ahead of any
  reserve swap-out, between 4 KB chunks. Swap-ins for timer or signal wakes of background
  processes run at idle priority. A swap-in never preempts a foreground FS request (R0.8d).
- **R12.1 amended: only the SD card.** `MMSWAPON` MUST refuse any device that is not a partition
  on the SD card (WINCHESTER major), so swap never goes to internal flash, the installed region, the
  run-time cache or a RAM disk. A swap file is not allowed (as before). Swap causes **0 flash
  writes** (rule 0.2 and the owner rule on run-time flash writes in `next-tasks.md`).
- **R12.24 No swap device.** If the card has no swap partition, or `/etc/rc.conf` says `swap=off`:
  - tiers 0, 1 and 3 work unchanged (they need only the root FS, which is on the card anyway);
  - parking works, but the image stays in RAM;
  - when an app can't start for lack of memory, the launcher says "Not enough memory" and opens
    the Running list, so the user can end a parked app. Nothing is ended automatically.
- **R12.25 Card removed or failing.** The Flipper's card-detect line (as stock
  `furi_hal_sd_is_present`) or a swap I/O error:
  - MM turns swap off at once (no more swap-outs);
  - every swapped process is killed with `SIGKILL` when it would be swapped in (never run with a
    partial image), and the console and the Running list say "lost: SD card removed";
  - resident processes are not affected by swap. They may still fail if the root FS is gone,
    which is the same as without swap;
  - a swap-in read error is handled like a GCM tag mismatch (R12.12).
- **R12.26 Smaller swapper state.** MM does not keep a stack length per slot: on swap-out it takes
  `sp` with `sys_getsp()` (already used by `brk`), and the kernel keeps the saved PSP while the
  process is swapped, so `[sp, S.end)` is known on swap-in. That saves 64 B over Part 4. The
  per-slot write counters (16 × 4 B, needed against rollback, R12.10) and the boot nonce SHOULD
  live in SRAM2 (A.6). Total MM swap state ≤ 96 B.

### 12g. SD card wear (owner request 2026-11-17; milestone M3)

Swap can't avoid wearing the card, but it can write much less, and write in a shape the card's
controller (FTL) handles well. The analysis is in `reports/sd-swap-wear-analysis.md` (project
files). In short: typical use writes about 1 MB a day, which is harmless on a card with normal wear
levelling. The cases worth guarding against are a cheap card whose wear levelling only has a small
pool of free blocks (a card that was once full and never erased), and **ping-pong**: a parked app
with a short timer that the reserve writes out after 10 s and its timer brings back a minute later
(≈ 35 MB a day).

Owner rule for this section: only measures that cost **0 B of RAM** and **no noticeable CPU** on
the foreground path (the AmigaOS feel: the UI always answers, whatever the rest is doing).

- **R12.29 Write only what changed.** Sector 0 of a slot (R12.2) also holds a **CRC-32 of every
  4 KB chunk** of the image (192 KB / 4 KB = 48 × 4 B = 192 B, fits in the 512 B header):
  - swap-out first reads the slot's header (a read does not wear the card, ≈ 1 ms). If its magic,
    pid, `dstart`, `dlen`, `sstart` and `slen` match the image now, a chunk whose CRC from RAM equals
    the stored one is **not written**. If nothing changed, nothing is written, not even the header;
  - otherwise (other pid or layout, bad header) every chunk is written, as today;
  - the CRC is the one swap-out already computes (R12.28), restarted at each chunk, so the CPU cost is
    the same as today plus one sector read, and every chunk not written saves a polled SD write
    (≈ 5–10 ms of disk-task time per 4 KB);
  - swap-in checks each chunk against its own CRC (this replaces the one CRC of the whole image);
    a mismatch kills the process as R12.28;
  - the reserve (R12.5 b) works chunk by chunk as today: each step re-reads the header (MM's `mbuf`
    does not live between steps) and the new header is written last, from CRCs taken in that job
    (the process is blocked, so its RAM does not change while the job runs; `sw_touch` still
    voids the job);
  - a stale chunk can only survive a CRC-32 collision of a changed chunk (2⁻³² per compared chunk;
    at ~1000 compares a day, once in about 10 000 years per device). Accepted: it is the same
    strength as today's card-error check;
  - RAM: +0 B (header in `mbuf`, as today).
- **R12.30 The reserve leaves alone what will wake soon.** A process brought back from the card by
  its own wake (timer, signal) gets a flag (`SWAPHOT`, a spare `mp_flags` bit). The reserve swaps a
  `SWAPHOT` process out only after it has waited **≥ 120 s** (instead of 10 s); the flag clears
  when it has waited that long once. On-demand swap-out (R12.5 a) ignores the flag: when RAM is
  really needed, the foreground wins. A parked app with a 1-minute timer therefore stays in RAM
  unless memory runs short, and when it is written, R12.29 writes only its changed chunks. RAM +0 B,
  CPU: one flag test per candidate.
- **R12.31 Large, sequential writes.** Swap-out computes the chunk CRCs, then writes the header
  **first** and the chunks to be written in ascending order (no jump back to sector 0 at the end).
  The WINCHESTER task passes a request of several sectors to the HAL as **one CMD25** (up to 4 KB,
  the swap chunk; today it splits into 1 KB transfers), optionally with ACMD23 (pre-erase count).
  Same bytes over SPI, fewer commands and busy waits: the card sees one sequential write. The
  4 KB chunk stays the unit that other card requests can get in between (R12.23), so no foreground
  request waits longer than today. RAM +0 B.
- **R12.32 Partitions aligned to the card's erase unit.** `mkpart` (first-boot setup) and the PC
  image tool align every partition start to **4 MiB** (the usual SD allocation unit) instead of
  1 MiB, so the 3 MB of swap slots lie inside one erase unit. New installs only; an existing card
  keeps its layout. Install time only, RAM +0 B.
- **R12.33 Tell the card what is empty (install time only; DISCARD at run time: R12.40).** First-boot setup (`/etc/setup`
  stage 2) sends **ERASE** (CMD32/CMD33/CMD38, supported in SPI mode) over the new `/usr`, swap and
  `/home` partitions before `mkfs`, with a line on the screen ("Preparing the card..."). A card that
  was once full (camera, phone) gets back its whole free pool for wear levelling. HAL:
  `hal_disk_erase(first, last)`; WINCHESTER: a root-only ioctl (`DIOCERASE`) on a partition device;
  command: `erase /dev/hdN` (refuses a mounted partition). Never at run time: no erase on swap-in,
  kill or at every boot (on cheap cards an erase can itself trigger garbage collection, and a 64 MB
  erase can take seconds). RAM +0 B (kernel: a few bytes of stack in the WINCHESTER task).
- **Left out on purpose** (they cost RAM, see the report): a swap KB-written counter in `free -v`
  and a daily reserve budget (≈ 4 B in MM, within R12.26's budget but not 0 B), and rotating the
  slots over the 64 MB partition per boot (needs a stored offset; helps only very primitive FTLs).
  They can come back with the owner's yes.
- **Hibernation.** If a whole-RAM hibernation image is ever added (analysis
  `reports/hibernation-analysis.md`, level 2), R12.29, R12.31 and R12.32 apply to it too; frequent
  cycles should use the cold-boot level 1, which writes nothing.

Acceptance criteria:
- AC12.14 QEMU (`tests/swapwear`, counting sectors written through the WINCHESTER task): swap a
  32 KB process out, swap it in, park it again without touching its heap: the second swap-out
  writes **0 sectors**; change 1 byte of the heap: it writes the header and **one** 4 KB chunk.
  AC12.4/AC12.13 (one flipped byte on the card kills on swap-in) still pass, for a skipped chunk too.
- AC12.15 QEMU: a parked test app with a 30 s timer, reserve on and free RAM under the reserve,
  for 10 min: no reserve swap-out of it (swap trace); an `exec` that needs its memory still swaps
  it out at once.
- AC12.16 Device: the swap-in time of a 64 KB image (today 69 ms) and L1/L2 during a swap-out are
  no worse; a 4 KB swap write is one CMD25 (trace). Numbers in PORTING.md.
- AC12.17 Device: `/etc/setup` on a card filled with data before: ERASE runs on the three partitions,
  the screen shows the step, `mkfs` and the first boot finish; `erase` on a mounted partition fails
  with `EBUSY`.

### 12h. Swappable blocks for apps (owner request 2026-11-17; milestone M4)

An app written for it may keep **more working data than fits in RAM**: numbered 4 KB blocks on
the swap partition that it reads into, and writes from, buffers of its own. There is no MMU, so
this is not transparent paging: like overlays or EMS, the app asks for a block by number and gets
it in a buffer (a "frame"); pointers never point at data on the card. A library gives the
comfortable form (frames, LRU, record arrays), and in Berry the illusion is complete (`vbytes`).
Analysis: `reports/swappable-blocks-api-analysis.md` (project files), which also rejects the
fully virtual variant (MPU trap + instruction emulation in the kernel: kernel RAM, 100–300× slower
per access, a large privileged attack surface).

Use: working memory that lives as long as the process (map tiles, Sub-GHz history, NFC dumps and
nonce logs, the text of a large file in an editor, Berry data arrays). Data that must survive the
process or a reboot stays in files. Owner rule as in 12g: kernel 0 B, MM a few bytes, no
noticeable CPU on the foreground path.

- **R12.34 Where the blocks are, and who gets how much (owner, 2026-11-17: the app declares it;
  no fixed equal share).** The swap partition (R12.1) holds the 16 swap slots in its first 3 MB
  (R12.2). The rest is the **block space**, shared by all processes:
  - it is cut into **256 extents** of equal size (block space / 256, rounded down to whole blocks;
    MM gets the partition size at `MMSWAPON`, it already probes the end). 64 MB partition: ≈ 240 KB
    per extent; 4 GiB: 16 MB; four 4 GiB subpartitions: 64 per subpartition, 64 MB each;
  - a process gets block space **only by asking**: `blk_open(bytes)` (R12.38) reserves one
    **contiguous run of extents** (first fit), rounded up to whole extents, and the app's block
    numbers 0…N−1 are counted from the start of that run. If no free run is large enough the call
    fails with `ENOSPC` (or, with `BLK_ANY`, gets the largest free run and is told its size);
    nothing else is evicted or shrunk for it. A run never crosses a subpartition;
  - **reservation table in MM: 2 B per process slot** (first extent, number of extents; 16 × 2 =
    **32 B**). That is the price of free sizes instead of a fixed 1/16 (which cost 0 B); it is the
    only table. Per-request work stays a range check and one addition;
  - a reservation ends with `blk_close()`, `exec` (the program identity changes, R12.35) or exit.
    A forked child starts without one. **Resizing at run time:** a second `blk_open` with a
    smaller size gives the tail back at once (its extents go through R12.40 like a closed run); a
    larger one grows the run in place if the extents right after it are free, otherwise
    `ENOSPC` and the old run stays (moving data would cost card writes; the app can close and open
    again). Run-time sizes cost the same 32 B as sizes fixed at install time would, and need no
    record on the card; the price is possible fragmentation of the block space, which stays small
    with at most 16 runs in 256 extents (first fit, the largest free run shown by INFO);
  - limits: one process may hold at most **half** of the block space unless it runs as root (a
    rule, 0 B), so one greedy app cannot lock everybody out; the launcher's Running list and
    `free -v` show each process's reservation, so the user can see and end the holder;
  - a block is **1 header sector + 8 data sectors** (4.5 KB on the card, 4 KB for the app). The
    64 MB partition holds ≈ 13 800 blocks (≈ 54 MB of data) in all;
  - **layout on big cards (owner, 2026-11-17):** on a card of **8 GB up to below 32 GB**, first-boot setup
    puts swap **at the end of the card** and gives it **50 %** of the space after `/usr`, at most
    4 GiB − 1 MiB (R12.39: one partition stays below 4 GiB); `/home` comes right after `/usr`
    (also at most 4 GiB − 1 MiB); whatever lies between them stays free for later. The MBR entry
    stays `/dev/hd3` (MBR order need not be disk order): `/usr`, `/home`, free space, swap.
    Smaller cards keep today's 64 MB swap between `/usr` and `/home`. Examples (root ≈ 64 MB,
    `/usr` 2 GB): 8 GB card (≈ 7.4 GiB) → swap ≈ 2.7 GiB (≈ 2.4 GB of blocks to share),
    `/home` ≈ 2.7 GiB; 16 GB and larger → swap 4 GiB (≈ 3.6 GB of blocks), `/home` 4 GiB, the rest free;
  - **maximum swap on cards of 32 GB or more (owner, 2026-11-17):** swap is the primary partition
    `hd3` at the end of the card, split into **four MINIX subpartitions** `hd3a`…`hd3d` of 4 GiB −
    1 MiB each (≈ 16 GiB). `/etc/rc` gives MM `hd3a`; MM finds the other three itself (their minors
    follow `hd3a`'s, and each must have the swap type 0x82 in `hd3`'s subpartition table; `hd3` itself has the MINIX type 0x81 as a subpartition container, so `MMSWAPON`, R12.1, accepts a subpartition of the SD card whose own table entry is 0x82), and keeps their number (1 B, so MM
    +1 B). The 16 swap slots stay in `hd3a`; the 256 extents are 64 per subpartition (≈ 14 GB of
    blocks to share; one reservation at most one subpartition, ≈ 3.6 GB). A missing or short
    subpartition only removes its extents; swap itself needs only `hd3a`. With root, `/usr` 2 GB and
    `/home` 4 GiB this uses ≈ 22 GiB; the rest stays free for resource file systems (up to 16
    partitions of ≤ 4 GiB in all, ≈ 64 GiB, until the driver gets more minors);
  - why at the end: swap holds nothing across a reboot, so it is the one partition that can always
    be **shrunk or moved without losing data**. Its half is a reserve: when `/home` must grow, the
    swap start moves up (swap off, new MBR entry, `MMSWAPON` again; the slots and extents are
    recomputed from the new size), and `/home` takes the freed space. Growing the MINIX file system
    in place needs a small `growfs` tool (new zones and bitmap blocks; the simplest form is `mkfs`
    leaving room for the bitmaps of the largest possible size); that tool is a later task;
  - `mkpart` gets a way to place a partition at the end (e.g. `82:50%@end`), still never moving an
    existing partition; install time only, an existing card keeps its layout;
- **R12.35 Block header and validity.** Sector 0 of a block (on the card only, read and written
  through the last sector of `mbuf` like the swap header, R12.26): magic `GZBK`, slot, pid, boot
  stamp, the program's identity (`mp_dev`, `mp_ino`, `mp_ctime`), block number, a CRC-32 of
  **each of the 8 data sectors** (hardware CRC unit, as R12.28), and the space 12b needs.
  - A block is valid only if magic, slot, pid, boot stamp and program identity match the caller.
    Otherwise it is someone else's leftover and reads as **all zeros**: every block 0…N−1 exists
    and starts zeroed (sparse memory), so there is no allocate or free call in MM.
  - Process exit, `blk_close` or `exec`: the reservation is freed and nothing is written (0 writes,
    no erase; R12.33; DISCARD at idle, R12.40). Whoever reserves those extents next has another
    pid or program and cannot see the old blocks: they read as zeros. `fork`: the child has no
    reservation (blocks are not inherited).
  - **Boot stamp**: 4 B in MM (RTC seconds at boot mixed with a counter), incremented also when
    MM's `next_pid` wraps (30000 → 2), so a reused pid never sees old blocks. With 12b the boot
    nonce (R12.10) takes its place.
  - The per-sector CRC lets an app read part of a block (from 512 B) with every sector checked.
- **R12.36 The MM request.** A new `MEMINFO` request `MI_BLK` (no new call number). `m1_p1` points
  to a request descriptor **in the caller's memory** (`struct blkreq`: operation and flags, block
  number, first sector and sector count, buffer, status; ≈ 16 B). MM reads it with `sys_copy`, so
  MM keeps no request state of its own. Checks: blocks on (R12.38), number in range, sector range
  within the block, the buffer wholly inside the caller's data or stack segment (WINCHESTER checks
  again with `numap`), caller not `NOSWAP` in `plain` mode (R12.37). Then:
  - **GET** (whole block or a sector range): one contiguous read of the header and the sectors
    needed, **straight into the caller's buffer** (WINCHESTER with `PROC_NR` = the caller, as
    `sw_io()`; no FS, no buffer cache, no copy); invalid block → the buffer is zeroed; a CRC
    mismatch or read error → `EIO`. The process is **not** killed: the data is not its image, the
    app decides;
  - **PUT** of a whole block: CRCs from the buffer, header + 8 sectors as **one CMD25** (R12.31). No
    read before the write: the library knows which frames are dirty. Flag `BLK_IFCHANGED`: read the
    header first (≈ 1 ms) and write nothing if every sector CRC matches;
  - **PUT** of a sector range: read the header, write the new header, write the range (two writes;
    for records of 1 KB and less);
  - **OPEN** (bytes, flags) and **CLOSE**: reserve or free the caller's run (R12.34);
  - **INFO**: the caller's reserved blocks, the largest free run, the whole block space, block
    size, mode (`plain`/`crypt`), card present.
  Error codes: `ENOSYS` (blocks off or no swap partition), `ENOSPC` (OPEN: no free run that
  large), `EBADF` (GET/PUT without a reservation), `ERANGE`, `EIO`, `ENXIO` (card out,
  N.6), `EPERM` (R12.37).
- **R12.37 Responsiveness, isolation, card.**
  - **Foreground** (`FGAPP`): served at once; the wait is the app's own.
  - **Background** callers are served like the reserve (R12.5 b, R12.23): if a key came in the last
    second or a process waits for the CPU, MM sets `BLKWAIT` (a spare `mp_flags` bit) and does not
    reply; `sw_step()` serves one request per step at idle, after a swap-in the user waits for and
    before reserve swap-outs, re-reading the descriptor from the caller. Their PUTs count in the
    reserve's write budget (`sw.wkb`, 256 KB a minute), so a background app can never write more
    than the reserve could. A `BLKWAIT` process is in an MM call, so it is never a swap victim.
  - One request is at most ≈ 11 ms of disk-task time (PUT of 4.5 KB), and senders to MM are queued
    in order, so other MM calls (`kill`, `alarm`, `exit`) wait at most one block operation; FS,
    the screen and the buttons do not wait at all.
  - A process reaches only its own reservation (MM computes the position from the caller's table
    entry; the app gives only a block number, checked against the run's length). Raw `/dev/hd3`
    stays root-only.
  - `swap=plain`: blocks are on the card in clear like swap images, so set-uid, set-gid and
    `Secret: yes` images get `EPERM` (the R12.28 `NOSWAP` condition). `swap=crypt` (12b): per-block
    AES-GCM as R12.10a (nonce = boot nonce, slot, block number, the block's counter from its
    header; tag in the header; no plaintext CRCs). Known limit: no protection against rolling one
    block back to an older version of the same session (it needs the card pulled, edited elsewhere
    and put back; N.6 notices the pull, not the edit). Accepted; the manual says so.
  - Card pulled (N.6): the driver answers `ENXIO` while the card is out **and while a different
    card is in** (CID), until the original is back, and never touches the other card. MM treats
    `ENXIO` as "wait for the original", not as lost data (as swap does, R-N6.5): a **background**
    request stays in `BLKWAIT` and `sw_step()` retries it (one card-detect read per try); a
    **foreground** request gets `ENXIO` at once, so the UI never hangs on the card, and `libblk`
    keeps its dirty frames and retries on the next call. The process is **not** killed (its image is
    in RAM). When the original card is back, every block is valid again (headers match). A write cut
    in half shows as `EIO` on that one block.
  - Swapped or parked processes: their blocks stay on the card, nothing moves. `app_park()` and
    `app_hibernate()` (R12.18, R12.19) call `vm_flush()` first.
- **R12.38 Library, Berry, switch.**
  - `/etc/rc.conf` `blk=off|on`: before 12b the default is `off` (like `swap=plain`, R12.28); with
    12b it goes on together with `swap=crypt`. Blocks work with `swap=off` if the partition exists
    (MM opens it for blocks only).
  - **Declaring the size.** At run time: `blk_open(bytes, flags)` before the first block, `BLK_ANY`
    to take what is free, `blk_close()`. In the package: the manifest field `Blocks: 64M` (the most
    the app will ask for). `pkg install` compares it with the card's block space and warns ("needs
    64 MB of card memory, this card has 54 MB") but installs anyway, because the card can change;
    the launcher shows the same line when `blk_open` fails, and the app shows its own message
    (an app that can live with less uses `BLK_ANY`). MM does not read manifests.
  - libc `<blk.h>` (flash only): `blk_open(bytes, flags)`, `blk_close()`, `blk_info()`, `blk_get(id, buf)`, `blk_put(id, buf)`,
    `blk_putf(id, buf, flags)`, `blk_read(id, off, buf, len)`, `blk_write(id, off, buf, len)` (off
    and len in multiples of 512).
  - `libblk` (flash only, ≈ 1.5 KB): `vm_open(pool, size, framesize)` with frames of 1, 2 or 4 KB
    in the app's own memory, `vm_get(v, off, VM_RD|VM_RW|VM_PIN)` (a pointer valid until the next
    `vm_get` unless pinned), `vm_unpin`, `vm_flush`, `vm_drop` (no write), and `VA_GET(v, T, i)`
    for arrays of fixed-size records that never cross a frame. 8 B of metadata per frame, in the
    app.
  - Berry module `blk` (C, flash): `blk.vbytes(size)`, a bytes-like object whose `[]`, `get` and
    `setbytes` go through two 1 KB frames. Berry objects themselves stay in the VM heap (pointers,
    R12.22); only raw data goes to blocks.
  - **Rule for app authors** (manual): whatever the current screen or a real-time loop needs is in
    frames or plain RAM; blocks are for what is "behind the screen". Emulation (NFC, RFID,
    iButton) and radio, DMA or USB streams never wait for a block. Prefer 1 KB frames for records,
    4 KB for sequential data. Times (before R12.31): GET 512 B ≈ 1.2 ms, 1 KB ≈ 1.7 ms, 4 KB ≈ 5 ms;
    PUT 4 KB ≈ 6–11 ms; 1 MB sequential ≈ 1.2 s.
- **Later (not in this step):** asynchronous `PREFETCH` (one request in flight per process, the
  descriptor's status field set by MM when the read lands, optional new `SIGBLK`; 0 B in MM); a
  "discard on swap-out" hint for frame pools (+4 B per process); a soft write cap for the
  foreground app (+4 B in MM); a Merkle root against rollback in `crypt` (32 B per process). Each
  needs the owner's yes.

- **R12.39 Partitions past 4 GiB (owner, 2026-11-17).** The stock Flipper takes microSD cards up to
  256 GB (SDHC/SDXC), and the HAL already uses block addresses (CCS, `sd_block_addr`). The 4 GiB
  limit is only in the WINCHESTER bookkeeping: `struct device` keeps `dv_base` and `dv_size` as
  32-bit **byte** counts, so `drvlib.c` (`lowsec << SECTOR_SHIFT`) overflows for a partition that
  starts past 4 GiB, and `mkpart` therefore keeps everything below 4 GiB.
  - On ARM, `dv_base` holds the partition's **first sector** (32-bit: up to 2 TiB) instead of a byte
    offset; `dv_size` stays a byte count, so one partition is at most 4 GiB − 512 B (FS and swap
    positions are 32-bit byte offsets inside a partition, `io_position` taken as unsigned, as
    `disk.c` already does). Changes: `drvlib.c` (partition and subpartition setup and limits),
    `src/kernel/arm/disk.c` (`w_schedule`: sector = `dv_base` + offset sector, no more
    `dv_base >> SECTOR_SHIFT`; `w_init`: whole-disk size and clipping), the partition ioctls (`DIOCGETP`/`DIOCSETP`
    report and take the base in bytes as before while it fits, `EOVERFLOW` past 4 GiB; `size` stays
    bytes), and their users are checked in the same PR (`mkpart`, the selftest `board` module that
    reads `p.size`), with the unit written in `<minix/partition.h>`. Kernel RAM +0 B (same fields,
    another unit).
  - `/dev/hd0` (the whole card) stays clipped to 4 GiB as today: only partitions reach past it.
  - `mkpart`: drops the 4 GiB ceiling for partition **placement** (start and end anywhere up to
    the card's size, 2 TiB at most); each partition at most 4 GiB − 1 MiB; `-l` prints starts
    past 4 GiB correctly. `/etc/setup` uses it for the layout in R12.34.
  - More than four file systems later (space between `/home` and swap on very large cards): MINIX
    subpartitions of `hd4` (`hd4a`…), each below 4 GiB. Not in this step.
  - Tests: host `parttest` with partitions starting at 4 GiB, 8 GiB and 255 GiB; QEMU with a sparse
    64 GB disk image: `/home` and swap past 4 GiB, mkfs, mount, swap-out and swap-in, a block PUT/GET
    at the last block. Device: a 64 GB or larger card in the Flipper (AC12.25).

- **R12.40 Tell the card what is garbage (DISCARD; owner, 2026-11-17).** SD cards from version 5.1
  have **DISCARD** (CMD32/CMD33 for the range, CMD38 with argument 1), the SD form of TRIM: the
  card only marks the range as unused and does not erase it, so its controller stops copying dead
  data during garbage collection (less wear, faster later writes). R12.33 (no ERASE at run time)
  stays; DISCARD is the one run-time exception, because it is cheap.
  - Only if the card says so: the `DISCARD_SUPPORT` bit of the SD Status (ACMD13), read once at
    card init by the HAL (`hal_disk_discard(first, last)`, `ENOTTY` otherwise, busy wait with the
    card's timeout and card-detect checks as N.6). WINCHESTER: `DIOCDISCARD` on a partition device,
    callable by MM and root; QEMU: fills the range with zeros so tests see it.
  - **`blk_discard(id, n)`** (and `vm_drop()` for whole blocks): the app says it no longer needs
    those blocks. Always served in the idle class (`BLKWAIT`, R12.37), never on the foreground
    path; on a card without DISCARD it does nothing. Either way a discarded block's contents are
    **undefined** for the app (old data, or zeros on QEMU), and the library treats it so.
  - **After a reservation ends** (exit, `exec`, `blk_close`) and the process did at least one PUT
    (a spare `mp_flags` bit, `BLKUSED`): MM sets bit *i* of a 16-bit "to discard" mask (MM
    **+2 B**) and keeps table entry *i* as it is, so the run stays taken until `sw_step()` has
    discarded it at idle (up to 256 MB per step, after swap-ins and block requests, before reserve
    swap-outs); then the entry is cleared and the extents are free. A new process in slot *i*
    gets no reservation until then (`blk_open` waits in `BLKWAIT`, at most one idle step). No
    discard can ever touch a live reservation. On a card without DISCARD the entry is freed at once.
  - **At `MMSWAPON`** the whole block space and the swap slots are discarded once, at idle, in
    256 MB steps (progress kept in the reserve job's existing `sw.off`, 0 B). Nothing else is
    discarded.
  - Costs: MM +2 B (mask), kernel 0 B (a few bytes of stack
    in WINCHESTER), no flash writes, no writes to the card's data.

Acceptance criteria:
- AC12.18 QEMU `tests/blk`: GET without `blk_open` gives `EBADF`; after `blk_open(1 MB)` block 0
  and the last block read as zeros and `ERANGE` comes one past the end; `blk_open` larger than the
  free space gives `ENOSPC`, with `BLK_ANY` the largest free run; a non-root request for more than
  half of the block space gives `ENOSPC`; two processes reserve 3 and 5 extents and their runs do
  not overlap (`free -v`); after the first exits, a 4-extent request fits only where it should; PUT then GET of 4 KB and of a 1 KB range return the data; a byte flipped on the
  card (raw write as root) gives `EIO` on GET of that sector and not of the others; the process is
  not killed.
- AC12.19 QEMU: after the writer exits, a new process that reserves the same extents reads zeros;
  the same after `exec` of another program; after a pid wrap (forced through a test hook) too; a
  forked child has no reservation; a second process cannot reach the first one's run by any
  argument.
- AC12.20 QEMU (sector counter in WINCHESTER, as AC12.14): `blk_putf(..., BLK_IFCHANGED)` of
  unchanged data writes 0 sectors; a whole-block PUT is one 9-sector write; process exit writes
  nothing.
- AC12.21 QEMU: a background caller while keys arrive is answered only at idle; its PUTs stop at
  the reserve budget; a foreground caller is answered at once. `MI_SWAPTEST` 2 (card pulled):
  a foreground GET gets `ENXIO` and the process lives on; a background GET waits and completes
  when the card is back (QEMU `DIOCFAKECARD`).
- AC12.22 `swap=plain`: a `Secret: yes` test app and a set-uid image get `EPERM`; `blk=off` gives
  `ENOSYS`.
- AC12.23 Device: L1 and L2 within budget while a background app reads blocks in a loop and while
  the foreground app scrolls a 1 MB block-backed map; measured GET/PUT times in PORTING.md.
- AC12.25 Device (R12.39): on a card of 64 GB or more, first-boot setup places `/home` and swap past
  4 GiB as R12.34; `fsck`, swap-in of 64 KB and a block GET at the last block work; a card pulled and
  put back (N.6) carries on.
- AC12.26 QEMU (R12.40): a process PUTs 10 blocks and exits; at idle the trace shows one
  `DIOCDISCARD` of its run and nothing while keys arrive; its extents are free only after that;
  a new process's `blk_open` meanwhile waits one idle step and no discard ever touches a live run; on a card without DISCARD no
  ioctl reaches the HAL. Device: a card with DISCARD (SD 5.1) takes it, L1/L2 unchanged.
- AC12.24 Berry: a script fills a 2 MB `vbytes` and reads it back (CRC of the whole) with two 1 KB
  frames; VM heap growth ≤ 3 KB.

RAM budget (12h):
- kernel +0 B; MM **+39 B** (reservation table 32 B, boot stamp 4 B, discard mask 2 B, number of
  swap subpartitions 1 B; with today's ≈ 55 B of swap state this stays within R12.26's 96 B, SHOULD
  be in SRAM2) and two `mp_flags` bits (`BLKWAIT`, `BLKUSED`); FS and INIT +0 B;
- a process that does not use blocks: 0 B; one that does: its own frames (from 1 KB) + 8 B per
  frame + the 16 B descriptor;
- flash: MM ≈ 1–1.5 KB, libc ≈ 0.3 KB, `libblk` ≈ 1.5 KB, Berry `blk` ≈ 1 KB.
- Effect (analysis section 7): the RAM free after login does not change (const data is already in
  XIP flash; servers cannot use blocks), but the peak of data-heavy apps drops by 20–50 % (NFC
  −4…12 KB, Sub-GHz −3…6 KB, IR −2…4 KB, ports with history −2…8 KB), and parked images get
  smaller (faster L6, fewer swap writes).

### 12f. Notes for the system manual

- The four background tiers in plain words: "starts when needed" (0), "sleeps on the card" (1),
  "parked" (2) and "runs in the radio chip or USB driver" (3). Which built-in apps use which tier.
- The Running list: what `in RAM`, `on SD` and `waiting for memory` mean, and how to end an app.
- Swap is on the SD card only. First it ships unencrypted and off by default (`swap=plain` turns
  it on, with a checksum against card errors; apps marked `Secret: yes` and set-uid programs are
  never swapped). Later `swap=crypt` (12b) encrypts it with a key that is new at every boot and
  becomes the default. Nothing swapped survives a reboot, and removing the card ends the swapped apps.
- Which card to use with swap (12g): a branded card, best a "High Endurance" or industrial (pSLC)
  one, 8–32 GB, new or erased. First-boot setup erases the new partitions; `erase` does it by hand.
  Swap writes only the parts of an app that changed, so a parked app that sleeps costs the card
  nothing.
- How to turn swap off (`swap=off` in `/etc/rc.conf`) or change the reserve (`MMSWAPRES`), and the
  effect: parked apps stay in RAM.
- For app authors: R12.16–R12.21 (choose the cheapest tier, no pointers in a hibernation blob, no
  secrets in it, size targets).
- Swappable blocks (12h) for app authors: the `blk`/`libblk` API and the Berry `vbytes` object,
  the rule "what the screen needs stays in RAM", the times, that blocks die with the process and
  with a reboot, that `blk=on` before 12b stores them in clear, and the rollback limit of `crypt`.

Acceptance criteria:
- AC12.1 QEMU: start an app that needs more memory than is free while the launcher and a shell are
  idle. The app starts, `free -v` shows the shell and launcher swapped, and both work normally after
  the app exits (the launcher redraws; the shell's variables and history are intact).
- AC12.2 Device: the same with the launcher and the About app; free memory while the app runs rises by
  at least the size of the swapped images.
- AC12.3 `tests/swapsecret`: a process writes a known 64-byte marker to its heap and blocks in
  `wait()`; after a forced swap-out, the marker is not found anywhere on `/dev/hd3` (raw search as root).
- AC12.4 `tests/swaptamper`: flip one byte of a swapped slot on the card; on swap-in the process is
  killed with a console message, and nothing else is affected.
- AC12.5 Swap-in zeroes the gap: a process that inspects its gap after swap-in sees only zeroes
  (with the `free -t` trace off).
- AC12.6 A kill of a swapped process frees its slot without reading it; 100 swap cycles in a loop leave
  free memory and slot use unchanged.
- AC12.7 No regression in AC1.x and AC3.x tests.
- AC12.13 `swap=plain` (R12.28): AC12.1, AC12.5, AC12.6 and AC12.10–AC12.12 pass; flipping one
  byte of a swapped slot kills the process on swap-in (CRC-32); a `Secret: yes` test app and a
  set-uid image are never swapped. AC12.3 and AC12.4 apply to `swap=crypt` only.
- AC12.8 Tier 1: `tests/hibernate` saves a 1 KB blob, exits, is resumed by a timer and by the
  launcher, and gets the same blob back at a different load address; `/var/hib` is empty after.
- AC12.9 Tier 2: a stock port (e.g. the Sub-GHz frequency analyzer without an open stream, or the
  Snake game) is parked, swapped out by the reserve, brought back from the Running list with its
  state intact, within L6 (≤ 600 ms).
- AC12.10 Placement: after 50 random launch/park/resume cycles with two parked apps, no parked app
  is ever "waiting for memory" for more than one foreground session.
- AC12.11 `MMSWAPON` on any non-SD device fails with `EINVAL`; a full day of the AC12.9 cycle
  writes 0 internal-flash pages (flash write counter, Phase 2b).
- AC12.12 Card pulled while two apps are swapped: both are reported lost, nothing else crashes;
  with `swap=off`, parking keeps images in RAM and a launch that doesn't fit opens the Running list.

Responsiveness (R0.8d):
- AC12.R1 While swap-out of an idle 32 KB process runs, the foreground app meets L1 and L2.
- AC12.R2 Launch of an app that needs a swap-out first: L6 ≤ 300 ms when the free-RAM reserve
  covers it, ≤ 600 ms otherwise. Return to a swapped launcher: L7 ≤ 400 ms.
- AC12.R3 The foreground app and its daemon are never swapped (checked in the swap trace over a
  10 min random-use test).

RAM budget:
- kernel +0 B (one flag bit);
- MM: at most +96 B (counters, nonce, swap state; R12.26), SHOULD be in SRAM2;
- `libapp` park/hibernate, launcher Running list, two-ended placement: flash only, +0 B;
- driver: +0 B (reuse the bounce buffer, or encrypt in place);
- per process: 0.

Net effect: −8 to −15 KB for each idle shell or launcher swapped.

---

## Phase 13: NFC driver

**Depends on:** 4b; Phase 12 (swap) SHOULD be in place for RAM headroom.

- **R13.1** ST25R3916 on SPI1 + IRQ (`/dev/nfc`):
  - field control;
  - polling for NFC-A/B/F/V;
  - `transceive` (frame in, frame out, with CRC and parity options) moving data between the
    chip's 512 B FIFO and the user buffer directly;
  - timeouts on the chip's own timers;
  - exclusive with `/dev/lfrfid`.
- **R13.2** Listener (card emulation) mode: the chip's automatic NFC-A anticollision answers
  REQA/ATQA/SELECT by itself from registers. Later frames wake the user process through R4b.3,
  and the response deadline is met with the chip's frame-wait timers.
- **R13.3** A raw "transparent" mode for MIFARE Classic emulation (crypto1 needs bit-level
  parity), as stock does.

Acceptance:
- AC13.1 Reads the UID of NTAG215, MIFARE Classic 1K, DESFire, an ISO15693 tag and a FeliCa card.
- AC13.2 Emulated NTAG215 is read by a phone.
- AC13.3 Emulated MIFARE Classic 1K passes a reader's authentication (stock-level compatibility).

Responsiveness: AC13.R Card emulation answers reader frames within the ISO 14443 timing (the ST25R3916 FIFO and auto-responses do the time-critical part); a read result appears on screen within 300 ms of the card arriving.

RAM: kernel ≤ +200 B. Flash: ≈ +10 KB kernel.

---

## Phase 14: App ports IV: NFC

| Stock app | MINIX mapping |
|---|---|
| NFC: Read, Detect reader, Saved, Extra actions (MIFARE Classic keys, Unlock with reader…), Add manually | `nfc` app, group `nfc`; stock `lib/nfc` (protocols, pollers, listeners), dictionaries streamed from `/home/flipper/nfc/assets` instead of loaded whole |
| `mfkey` (key recovery on device) | separate `mfkey` command, runs as a normal process (swappable when idle) |

Acceptance:
- AC14.1 Card dumps saved by stock load, and vice versa.
- AC14.2 Dictionary attack on a MIFARE Classic 1K recovers keys at ≥ 80 % of stock speed.
- AC14.3 NFC app peak RAM ≤ 35 KB; the launcher is swapped meanwhile (Phase 12) and comes back.

Flash: ≈ 110 KB installed.

Responsiveness: see AC13.R; the NFC app's screens meet L1.

### 14.1 USB NFC reader (section G)

- **R14.1.1** USB CCID contactless reader profile `nfc-ccid`: slot = the RF field; card present
  → ATR built per PC/SC Part 3; APDUs passed to ISO 14443-4 cards (T=CL) by the NFC driver;
  pseudo-APDUs for memory cards: get UID/ATS, load key, authenticate, read/update binary for
  MIFARE Classic and Ultralight/NTAG. Card insertion/removal on the interrupt IN endpoint.
- **R14.1.2** HID-wedge profile `nfc-uid`: types the UID, formats as R11.1.2.
- **R14.1.3** Hybrid (G.3 option C): CCID framing and ISO 14443-4 APDUs (up to 261 B short
  APDUs; extended APDUs SHOULD) in the kernel, moved PMA → ST25R3916 FIFO → PMA by SPI DMA with no
  SRAM copy; MIFARE Classic pseudo-APDUs (Crypto-1) go to a helper started on demand, which exits
  after 10 s idle. USB input is bounds-checked and fuzzed as R11.1.6.
- **R14.1.4** Windows' inbox CCID driver and macOS work with the class alone; Linux `libccid`
  loads only listed readers, so the project's VID/PID is submitted to libccid and the docs give
  the `Info.plist` line for older systems.
- Acceptance:
  - AC14.1.1 `pcsc_scan` on Linux and the Windows smart-card service see the reader and the card;
    a payment card's PPSE select (`00 A4 04 00 0E 32 50 41 59 ...`) returns its answer; a MIFARE
    Classic 1K's UID is returned by `FF CA 00 00 00`.
  - AC14.1.2 `nfc-list` (libnfc with the PC/SC driver) lists a tag.
- Responsiveness: AC14.1.R APDU round trip overhead added by the Flipper ≤ 5 ms over the card's
  own time; card presence reported ≤ 200 ms.
- RAM: kernel within the shared mode `union` (≤ 64 B); helper ≈ 4 KB only while MIFARE Classic
  commands are used.

### 14.2 NFC canary: who reads me? (section M)

- **R14.2.1 Field detector:** the ST25R3916 external-field detection (and its low-power wake-up
  mode) reports "a reader field appeared" with time and duration. Pocket mode (M.1, tier 3): the
  chip watches alone and the CPU wakes only on a field; the alert vibrates. This is the "is
  something reading my pocket" alarm. The same detector (NFC IRQ PA2 → EXTI2) can wake the
  Flipper from Stop2 on a phone tap (R6.8 wake sources).
- **R14.2.2 Tag bait (optional, chosen in the app):** the Flipper answers as a **blank generic tag**
  with a random UID: NFC Forum Type 4 (ISO 14443-4) that returns "file not found" to every
  SELECT, or Type 2 with an empty NDEF area. It logs: anticollision, RATS and its parameters,
  each SELECT by AID with a name from a `const` table (e.g. `2PAY.SYS.DDF01` = a payment terminal
  looking for cards, transit and access AIDs), READ/WRITE attempts (writes refused), and the field
  timing. It never emulates a real card (M.1).
- **R14.2.3 Reader guess:** from the AID order and RATS parameters: "looks like: payment terminal",
  "phone (Android/iOS) reading NDEF", "access reader". Labelled as a guess.
- **R14.2.4** Frames are parsed in the kernel NFC fast path (as 14.1), bounds-checked (I2); events
  go into a 512 B ring lent by the FS cache.
- Acceptance:
  - AC14.2.1 A phone with NFC on, held to the Flipper in pocket mode, wakes it and vibrates
    within 300 ms; the log shows the field and the NDEF read.
  - AC14.2.2 With the bait, a payment terminal in test mode (or `pcsc`/libnfc script imitating
    one) shows the PPSE SELECT; no SELECT gets a non-error answer.
  - AC14.2.3 Idle current in pocket mode is within 20 % of normal idle (record the numbers).
- RAM: kernel ≤ +32 B (mode `union`), 512 B lent while armed; app ≈ 3 KB only while running.

---

## Phase 15: Bluetooth LE and RPC

**Depends on:** 3 (RNG), 4b, Phase 12 recommended.

- **R15.1** CPU2 bring-up:
  - read `SFSA` from `FLASH->SFR` (as stock does, R2b.1);
  - **reuse the stack already installed on CPU2.** The owner's Flipper already has a working
    stack (it paired with an Android phone under the official firmware), most likely ST's BLE
    light stack left by the stock updater. MINIX does not reflash CPU2 in this phase; it starts
    whatever R2b.10 found, and refuses BLE (with a clear message, everything else keeps
    working) if the probe found no stack, only FUS, or a stack type without BLE;
  - the transport and `bled` target the ACI/HCI command set of the BLE light stack, and use the
    extra features of the full stack only when R2b.10 reports it;
  - IPCC mailbox; HSEM protocol for flash, RNG, PKA and AES2 (old R9.3), including the
    flash-text-cache and installed-region writers.
- **R15.2** Transport task `ble`: HCI/ACI command and event exchange. Stack ≤ 768 B, measured.
- **R15.3** SRAM2 re-plan: the mailbox and event pool go into SRAM2a/2b (6–8 KB); the FS buffer
  cache shrinks to fit (about 11 blocks), with no SRAM1 cost.
- **R15.4** `bled` daemon, started on demand by `devd`:
  - GATT services: serial (for the mobile app), battery, device information, HID;
  - pairing with a PIN shown on the LCD;
  - bonding data (owner request 2026-11-15, section L): **SHOULD not be kept in CPU2's flash.**
    Configure the stack to keep its bonding database in RAM in SRAM2 (the area stock already
    reserves for it) and save it to a file on the SD card, `/etc/flipper/bt.keys` (root-only,
    0600), after pairing and on clean shutdown; load it when `bled` starts. Stock works this way
    (`bt_keys_storage`); check on the installed stack (R2b.10) that this mode is available. If it
    is not, the PR says how often the stack writes its NVM in normal use (rule on flash writes,
    `next-tasks.md`). AC15.6: pairing a phone and reconnecting after a reboot changes no
    internal-flash erase counter (R2b.11).
- **R15.5** `rpcd`:
  - the stock protobuf RPC (nanopb) over the second USB CDC and over BLE serial;
  - storage list/read/write/delete, app start, screen streaming (from R4b.8 read), input
    injection, system info, date/time;
  - runs as its own user, with access limited to `/home/flipper` unless root.
- **R15.6** Over-the-air package install via RPC lands in `pkg`'s incoming directory and follows R3.5.
- **R15.7 Phone link: the official Flipper mobile app (owner request, 2026-11-15).** Nothing can be
  installed on the phone, so MINIX speaks the app's own protocol: it is a BLE **peripheral** that
  offers the stock serial service and answers a **subset of the stock RPC** (protobuf, `flipper.proto`,
  `system.proto`, `storage.proto` from the stock tree). This works with the light stack (R15.1).
  - **Profile "phone", in order:**
    1. *Session start (needed by everything):* `system_ping`, `system_protobuf_version` (we report
       the stock major/minor we were generated from), `system_device_info` (the keys the app reads,
       taken from stock `furi_hal_info_get`, with our own `firmware_*` values and a `firmware_origin`
       that says MINIX), `system_power_info` (battery, from the BQ27220 fuel gauge, R6.1). Unknown requests
       get `ERROR_NOT_IMPLEMENTED`, never a disconnect, so the app keeps working with what exists.
    2. *Time sync (the reason for this item):* `system_set_datetime` and `system_get_datetime`, see
       the rules below.
    3. *Storage browsing:* `storage_list`, `storage_stat`, `storage_info`, `storage_read`, and later
       `storage_write`, `storage_mkdir`, `storage_delete`, `storage_rename` (as R15.5), limited to
       `/ext` with the permissions of `rpcd`'s user (R15.5). Writes go to the SD card only.
    4. *Later:* screen streaming and input injection (R15.5, R4b.8), `app_start` (R15.5),
       `system_play_audiovisual_alert` (the app's "find my Flipper"; plays through `alert`, F.2).
    Not answered: `system_factory_reset` and `system_reboot` mode `UPDATE` (`ERROR_NOT_IMPLEMENTED`;
    our updater is Phase 17), `system_update_request` (Phase 17 decides).
  - **Time rules (section I.1; owner, 2026-11-15: the clock stays UTC, for navigation by sunrise
    and sunset, I.5–I.6).** The stock `DateTime` message has no zone and no sub-seconds; stock keeps
    **local** time in its RTC, so the app sends the phone's **local** wall time (to be confirmed
    in the PR with a log of what the app sends, on Android and iOS).
    - On `set_datetime`, `rpcd` checks the fields (year 2000–2099, month, day of the month, H:M:S;
      `weekday` is ignored and recomputed), converts the local time to UTC with the system zone
      (`/etc/TZ`, `mktime` with the `TZ` rules), and only then calls `stime()`. **Local time is
      never stored**, not in the RTC and not in a file. Stock does not check the fields
      (`rpc_system.c` passes them straight to `furi_hal_rtc_set_datetime`); we do.
    - In the repeated hour of a fall-back day the local time is ambiguous; pick the occurrence
      closer to the current RTC (one subtraction).
    - **Zone check:** `rpcd` compares the phone's local time with our UTC clock: the difference,
      rounded to 15 minutes, is the phone's UTC offset. If it does not match the offset of
      `/etc/TZ`, the clock is **not** stepped (an hour's error would be 15° of longitude), and a
      notice (`notify_post`, category `info`) says "Phone is in UTC+n, Flipper in UTC+m: change
      the zone?" with two choices: open the zone settings, or "set the clock anyway" (for a clock
      that really is an hour off). This happens, for example, after a trip. If the RTC is not valid
      (backup domain lost, `RTC_ISR.INITS` clear) there is nothing to compare, and the time is
      converted with `/etc/TZ` and set.
    - **No zone yet** (first boot, before the wizard set `/etc/TZ`): the received time is kept only
      in `rpcd`'s RAM and offered to the wizard (Phase 18b) as "time from phone, zone: ?"; the
      clock is set once the user picks the zone.
    - **Small offsets are logged, not stepped.** The app's time has 1 s resolution and BLE adds
      up to one connection interval, so its error is about ±1 s. If |offset| ≤ 2 s the clock is
      left alone (a calibrated RTC is usually better than that, I.6) and the offset goes to
      `/var/db/chrono` marked as quality `phone, ±1 s`; NTP (Phase 19) and a manual time-signal
      check stay the precise sources. A larger offset steps the clock with `stime()`, which sends
      `SIGRTC` so `timed` recomputes all timers (F.2, R7b.1) and logs the step to `/var/db/chrono`.
    - The right to set the clock is narrow: `rpcd` is in group `clock`, which may call `stime()`;
      it gets no other root right.
    - `get_datetime` answers in local time (UTC converted with `/etc/TZ`), so the app shows the
      same time as the Flipper's own clock.
  - **Shared code with Phase 21.** The generated protobuf code (nanopb, `const` descriptors) and the
    framing of the serial service (length-delimited `PB_Main`, the flow-control characteristic) live
    in one shared library in flash, `libflipperpb` (R2b.9). `rpcd` uses it as the server, `flipinst`
    (R21.2) as the client. One generated `.pb.c` per `.proto` file; a program links only what it uses.
  - **RAM only while connected.** `bled` starts `rpcd` when a central subscribes to the serial
    service and `rpcd` exits on disconnect; when no phone is connected the phone link costs 0 B. In
    the "phone" profile (items 1–3) `rpcd` decodes one `PB_Main` at a time into a static buffer
    (largest message: `storage_read` chunk, 512 B, as stock), target ≤ 4 KB including its stack,
    measured in the PR. Streaming and input (item 4) may grow it to the 6–10 KB of R15.5.
  - **Flash writes: none at runtime.** Time goes to the RTC registers; `/var/db/chrono` and the
    notice are on the SD card; storage writes only touch `/ext`.
  - **Responsiveness:** `rpcd` runs at normal user priority, below the interactive queue (rule 0.8);
    a `storage_read` reply is sent in chunks so the UI never waits more than one chunk. Time sync
    takes one message and a few hundred instructions of conversion.

Acceptance:
- AC15.1 The official mobile app connects, browses `/ext`, mirrors the screen and sends button presses.
- AC15.2 qFlipper over USB does the same.
- AC15.3 BLE remote (HID) works with a phone and a PC.
- AC15.4 `make ramcheck`: SRAM1 static unchanged by BLE (±128 B); the FS cache is in the remaining SRAM2.
- AC15.5 Phone link (R15.7): the official app on Android and iOS connects, shows the battery and
  device info, and syncs the clock: with the Flipper in `CET-1CEST` and the phone in Warsaw, the RTC
  holds UTC afterwards (`date -u` matches a reference within 2 s); with the phone in another zone the
  clock is not stepped and the zone notice appears. Unimplemented requests return
  `ERROR_NOT_IMPLEMENTED` and the app stays connected. `rpcd` is not running when no phone is
  connected; RAM while connected ≤ 4 KB in the "phone" profile; no internal-flash erase counter
  changes (R2b.11).

Responsiveness: AC15.R No BLE disconnect over 30 min while the user browses menus and copies files; mobile-app button injection → screen update ≤ 100 ms over BLE (one connection interval); flash writes wait for CPU2's flash-activity grant (R0.8c).

RAM: kernel SRAM1 ≈ +1 KB (task stack + state), SRAM2 6–8 KB (taken from the FS cache);
`bled` ≈ 6 KB and `rpcd` 6–10 KB while running. Flash: ≈ +10 KB kernel, ≈ 45 KB installed.

## Phase 16: App ports V: Bluetooth apps

| Stock app | MINIX mapping |
|---|---|
| Bluetooth settings (on/off, forget devices, debug) | Part of `settings`, talking to `bled` over a pipe/socket-like device |
| BLE remote (keyboard, media, mouse, TikTok/YouTube remotes) | `bleremote` app using the HID service of `bled` |
| Mobile app pairing UI | `bled` UI hook in the launcher |
| Mobile app connection (time sync, device info, file browsing) | `rpcd` "phone" profile, R15.7 |

Acceptance: AC16.1 Pairing, unpairing and reconnect after reboot work with Android and iOS.
AC16.R BLE remote keypress → host event ≤ 30 ms p99 (connection interval 7.5–15 ms).

### 16.1 Bluetooth adapter: BLE mouse and keyboard → USB (native app `btbridge`, section F)

Owner request (2026-11-14): the Flipper acts as a Bluetooth adapter, for example for a mouse. It
connects as a BLE **central** to a BLE HID device (HID over GATT, "HOGP": mouse, keyboard, remote,
game pad) and presents it to the PC as a USB HID device. The PC needs no Bluetooth and no driver.

Limits, stated up front:
- **BLE only.** The STM32WB55 radio has no Bluetooth Classic (BR/EDR), so older Classic-only mice
  and keyboards can't be used. Most current mice with "Bluetooth" support BLE; check for
  "Bluetooth Low Energy" or "BT 4.0+ LE" on the device.
- **Needs the full BLE stack on CPU2.** ST's BLE *light* stack, which stock installs, supports
  the peripheral role only; central and GATT client need the **full** stack (ST's release notes
  for the installed version are the reference; R2b.10 prints which stack is installed).
  The full stack is larger, so `SFSA` moves down and the installed region shrinks by the
  difference (R2b.1 handles it at boot); it may also reserve more secure SRAM2 for CPU2, which
  comes out of the FS cache (R15.3). Both numbers are measured after the switch and recorded.
  Switching is a FUS stack update (R17.3 path, as qFlipper does) and is an **owner decision
  (R16.1.0)**; the full stack also keeps everything the light one does (mobile app, BLE remote).

Requirements:
- **R16.1.0** Decided by the owner (2026-11-14): install the full BLE stack. When: at the start
  of Phase 16, after Phase 15 works on the stack already installed (R15.1), as one FUS update
  (R17.3 path, as qFlipper does), with the before/after `SFSA`, installed-region size and SRAM2
  reservation recorded in the PR. One install, no run-time switching (rule: minimize flash
  writes). Until it is installed, `btbridge`, `sbctl` (16.4) and the BLE part of `camfind` (16.3)
  report "needs the full BLE stack" and exit.
- **R16.1.1** `bled` gains the central role: scan (filtered to HID service UUID 0x1812), connect,
  pair/bond (LE Secure Connections; passkey shown on or typed into the Flipper as the device
  asks), GATT client discovery of the HID service, Report Map read, and notifications enabled on
  input reports. Multi-role: the mobile-app connection (peripheral) may stay up at the same time.
- **R16.1.2** Bonding store: the stack keeps bonds as R15.4 says (in SRAM2, saved to
  `/etc/flipper/bt.keys` on the SD card; CPU2's flash only if R15.4 found that impossible);
  `btbridge` keeps only the list of known devices (address, name, last used) in
  `/ext/apps_data/btbridge/devices`. No bond data in RAM when the app is closed.
- **R16.1.3** USB presentation: the USB HID interface (R10.1) re-enumerates with the **device's
  own Report Map** as its report descriptor, so any HOGP device (mouse with extra buttons,
  keyboard with media keys) works without translation; boot-protocol mouse/keyboard is the
  fallback when the Report Map exceeds the USB descriptor buffer or fails to parse.
- **R16.1.4** Fast path for reports: input-report notifications are forwarded from the `ble`
  transport task straight to the USB HID IN endpoint in the kernel (report id + payload copied
  into the USB PMA, A.6), without passing through `bled` or `btbridge`. User space only sets up
  and tears down the bridge (`ioctl` on `/dev/hid`: bind connection handle N to interface M).
- **R16.1.5** Output reports (keyboard LEDs) go the other way: USB OUT → GATT write by `bled`.
- **R16.1.6** Background operation: once bridged, `btbridge` may exit; the bridge keeps running
  with the screen free for other apps, and a status-bar icon shows it. Reconnect to bonded
  devices on power-up is automatic when "bridge on boot" is set.
- **R16.1.7** Power: while bridged, USB is active, so the CPU does not enter Stop2 (R6.8); the
  bridge is powered by the PC. On battery without USB, the bridge is off.
- **R16.1.8** BLE and Stop2 (power analysis 2026-11-15): once CPU2 runs, Stop2 entry and exit use
  the stock HSEM protocol (RCC and entry-stop semaphores, `PWR_C2CR1.LPMS`, as stock
  `furi_hal_power_deep_sleep`). Unlike stock, MINIX does not need CPU2 to sleep, and advertises
  only on demand (app, pairing, phone link), slow interval (1–2.5 s) after 30 s fast.

Acceptance:
- AC16.1.1 A BLE mouse (e.g. any "BT 5.0" office mouse) and a BLE keyboard pair, then move,
  click, scroll and type on Linux, Windows and macOS through the Flipper's USB port.
- AC16.1.2 After a reboot the bonded mouse reconnects without pairing again.
- AC16.1.3 Keyboard Caps Lock LED follows the PC.
- AC16.1.4 With the bridge active, another app (e.g. Sub-GHz receive) runs in the foreground
  and the bridge's latency stays within AC16.1.R.

Responsiveness:
- AC16.1.R Added latency, BLE notification received by CPU1 → USB report ready in the PMA:
  ≤ 1 ms max. End to end (mouse event → host sees it): ≤ connection interval + 2 ms; with a
  7.5 ms interval that is ≤ 10 ms p99 (the target), measured with a USB analyser or `evtest`
  timestamps against a GPIO-triggered reference.

RAM: kernel ≤ +64 B (bridge binding table, 2 entries); `bled` +1 KB for the GATT client while
running; `btbridge` ≈ 3 KB while open, 0 when the bridge runs in the background. SRAM2 and flash
change with the full stack, as stated above. Flash: ≈ +2 KB kernel, ≈ 12 KB installed.

### 16.2 Bluetooth dongle mode: the PC drives the Flipper's BLE radio (section G)

- **R16.2.1** HCI pass-through: CPU2's BLE stack accepts raw HCI commands and ACL data through
  the IPCC mailbox (ST's "transparent mode"). The `ble` task relays them unchanged between the
  mailbox and USB: HCI commands on EP0 class requests, events on interrupt IN, ACL on bulk
  IN/OUT (Bluetooth USB class 0xE0/0x01/0x01). Fallback profile: the same stream with H4 framing
  on a CDC serial port.
- **R16.2.2** Stack: with the light stack only peripheral operation works, which is of little use
  to a PC; a useful dongle needs the full stack (R16.1.0) or ST's HCI-only stack. Which one to
  install was decided by R16.1.0: the full stack.
- **R16.2.3** While in dongle mode, MINIX's own `bled` is stopped (the PC owns the controller); the
  mobile-app link and the 16.1 bridge are unavailable until the mode ends.
- **R16.2.4** BLE only (no Classic, as 16.1). Windows' and macOS's acceptance of an LE-only
  controller is checked and documented; Linux BlueZ is the reference host.
- Acceptance: AC16.2.1 On Linux, `bluetoothctl` scans, pairs with a BLE device and reads its
  GATT services through the Flipper (`btusb`, or `btattach` on the serial fallback).
- Responsiveness: AC16.2.R HCI relay adds ≤ 1 ms per packet each way.
- RAM: kernel ≤ +64 B (relay state; data in the IPCC buffers in SRAM2 and the USB PMA). Flash
  ≈ +2 KB kernel.

### 16.3 `camfind` version 2: BLE scan (gadget, F.5)

- **R16.3.1** `bled` gains a passive observer scan (no scan requests) reporting address, RSSI,
  advertised name, manufacturer ID and service UUIDs. If the installed stack doesn't support
  scanning (light stack), the BLE part of `camfind` says "needs the full BLE stack" (R16.1.0).
- **R16.3.2** Scoring adds BLE manufacturer IDs and names of camera makers, and BLE locate mode
  (hot/cold on one address; random addresses are followed while their payload stays the same).
- AC16.3.1 A BLE-advertising device is found and located from 5 m with locate mode.
- RAM: `bled` +256 B scan list while scanning; `camfind` unchanged.

### 16.4 `sbctl`: remote for Storz & Bickel vaporizers (owner request, 2026-11-14)

Confirmed by the owner (2026-11-14): wanted. The protocol core comes early as task T6
(`docs/spec/next-tasks.md`); the BLE and UI parts follow Phase 16.

Target devices (owner, 2026-11-15): the owner has a **Venty**, so the qvap driver (Venty, and
Veazy, which uses the same protocol) is the first and the reference: it is verified on the
owner's device and all acceptance tests run on it. Volcano and Crafty stay in the design and
their tables are written, but they are marked "unverified" until someone tests them on real
hardware. The model table is the extension point, so a new model is a table plus tests.
Venty notes: it does not report the current temperature (only target, boost and superboost
offsets, heater mode, battery, charger, auto-off countdown), so the main screen shows the target
and the "setpoint reached" flag, and "Notify when ready" uses that flag (status byte 14, bit 1).

Owner request: port the vendor's Web Bluetooth app (app.storz-bickel.com, v3.4.1) as a native app
that controls the owner's own Volcano Hybrid, Venty, Veazy or Crafty/Crafty+ with the Flipper as
BLE central. The vendor code is proprietary and is **not** copied: the protocol comes from the
MIT-licensed reimplementation `storzandbickel-ble` (PyPI, 0.1.25), which documents the same app
version, and from checks on real devices. Findings and UUID tables: project report
`reports/storz-bickel-port.md` (to move to `docs/` with the manual).

Scope: temperature target, heater on/off, Volcano air pump, boost and superboost, battery and
charger state, LED brightness, auto-off time, vibration, °C/°F, Find my device, device info
(serial, firmware, hours), Volcano workflows. **Out of scope:** device firmware update (Volcano
bootloader; Venty/Veazy images are encrypted and fetched from the vendor's server by serial
number), the "Analysis" upload, and any vendor cloud or analytics call.

Requirements:
- **R16.4.0** Needs the central role and a GATT client, so it depends on the same owner decision
  as 16.1 (R16.1.0, full BLE stack). Until then `sbctl` says "needs the full BLE stack" and exits.
- **R16.4.1** `bled` exposes the generic GATT client that R16.1.1 adds (scan with a name filter,
  connect, discover by 128-bit UUID, read, write, write without response, notify) to apps through
  its socket; HID-specific parts stay in 16.1. No pairing: the devices use an open GATT. If a
  device or firmware asks for encryption, the R16.1.1 pairing path is used unchanged.
- **R16.4.2** Device drivers are three small tables in `sbctl` (const, in flash): Volcano
  (separate characteristics, 16-bit status registers), qvap = Venty and Veazy (one characteristic,
  command byte + write mask), Crafty (three services, project status registers, security-code
  write before auto-off). Discovered handles are cached per session (≈ 40 B), never persisted.
- **R16.4.3** UI on 128x64: large temperature (current on Volcano and Crafty; target on
  Venty/Veazy, which don't report the current one), target line, heater/pump/battery icons.
  Up/Down = target ±1 °C (hold: ±5 °C every 150 ms), OK = heater, Right = pump (Volcano) or
  boost → superboost → normal (others), Left = menu (workflows, favourite temperatures,
  settings, Find my, info), long Back = quit. Button feedback is local and immediate; GATT writes
  are coalesced (only the last value after 150 ms idle is sent) to keep radio traffic low.
- **R16.4.4** Known devices (address, model, name) and favourites in `/ext/apps_data/sbctl/`
  on the SD card. **0 internal-flash writes at run time.**
- **R16.4.5** Background, tier 0 only (12d), nothing resident: the device heats on its own, so the
  app may exit at any time. "Notify when ready": a `timed` interval timer runs `sbctl --poll`
  every 15 s (connect, read state, disconnect; ≈ 2 KB for < 1 s) and posts `info` with
  `notify_post()` when the setpoint is reached; it gives up after 15 min. Volcano workflow steps
  have deadlines, so in the background each step is a one-shot `timed` timer (R12.16); version 1
  MAY run workflows in the foreground only, with a progress bar.
- **R16.4.6** Safety: the app never disables the device's own auto-off; it only sets values the
  vendor app allows. Factory reset asks for confirmation. Register writes go through named bits,
  never raw values from the user. A "pump off" step retries the connection up to 5 times, and the
  workflow screen warns that a lost link leaves the pump to the device's own auto-off.
- **R16.4.7** Temperature limits come from the model table (Volcano 40–230 °C, Venty, Veazy and
  Crafty 40–210 °C) and are enforced before any write.
- **R16.4.8** Before implementation, the UUIDs and packet layouts are confirmed on real hardware
  (`nRF Connect` or `bluetoothctl` GATT dump); Veazy in particular. Differences go into the report.
  Venty: verified on the owner's device (2026-11-15, nRF Connect): name prefix `S&B VY`, service
  `00000000-5354-4f52-5a26-4249434b454c` with one characteristic `00000001-…` (notify, write,
  write without response, CCCD 0x2902), works without bonding. It also has the standard Device
  Information service 0x180A, which `sbctl` reads for model and firmware. Command bytes are
  checked on the device once the BLE parts exist.

Acceptance:
- AC16.4.1 On each owned model: connect, set the target, heater on/off, (Volcano) pump on/off,
  values on the device follow within one second.
- AC16.4.2 "Notify when ready" fires with no app open, and `make ramcheck` shows 0 B resident.
- AC16.4.3 No internal-flash write during a session (flash write counter unchanged).

Responsiveness: AC16.4.R Button → pixel ≤ 30 ms p99 (L1) independent of the radio; button →
GATT write sent ≤ 150 ms + one connection interval.

RAM: kernel 0 B; `bled` +1 KB GATT client (shared with 16.1, only while running); `sbctl`
≈ 3 KB while open, 0 B in the background (≈ 2 KB for < 1 s per poll). Flash: ≈ 10 KB installed.

### 16.5 BLE canary: who connects and what do they probe? (section M, owner request 2026-11-15)

- **R16.5.1 Bait (peripheral; works with the light stack):** `bled` advertises a connectable
  device with a generic name chosen in the app ("Speaker", "Lock", "Sensor" or the user's text),
  a random static address, and a small fake GATT table from `const` data (Device Information,
  Battery, and one vendor-specific service with our own UUID). It logs per connection:
  - the central's address and address type, connection parameters, MTU request, PHY updates;
  - every service and characteristic discovery, read, write (refused with "write not
    permitted", value logged as hex, at most 32 B) and notification subscription;
  - **pairing requests:** IO capabilities, bonding flag, Secure Connections, key size. The canary
    then answers "Pairing Failed: pairing not supported" (unless the honeypot of R16.5.6 is on);
  - disconnect reason and the reconnect rhythm.
- **R16.5.6 Honeypot (owner idea 2026-11-15; off by default, a switch in the app):** the bait
  accepts pairing and watches what the central does once it trusts the device.
  - Pairing as the central asks, with Just Works or a passkey shown on the Flipper's screen; the
    log records the method, Secure Connections or legacy pairing, key size and which keys each
    side distributes. Legacy (non-SC) pairing is flagged as weak.
  - **Keys are never used again:** LTK, IRK and CSRK live in `bled`'s session state and are wiped
    (zeroed) when the session ends or the app exits. Nothing goes to the SD bond file (R16.1.2) or
    to CPU2's flash; the only copy that can outlive the session is the optional analysis export
    below. If the installed stack cannot keep a bond without writing its own flash
    (check with the R15.4 findings), the honeypot pairs **without bonding** (Bonding_Flags = 0):
    the link is still encrypted, but the central distributes no identity key and cannot
    reconnect encrypted later. The app says which of the two modes is in use.
  - After pairing it logs: reads and writes to characteristics marked "encrypted" or
    "authenticated" (a second `const` GATT section that only opens after pairing, e.g. a fake
    "settings" and "firmware version" characteristic), notification subscriptions, writes that
    look like commands (hex, at most 32 B each), and attempts at an unknown handle or service.
  - **Identity behind the random address:** when the central distributes its IRK and identity
    address, the log shows that the random address it used belongs to that identity, and later
    reconnects in the same session that resolve to it are marked "same device". The IRK itself is
    never written to the event log (only to the key export below, if enabled); the identity address is stored hashed if the privacy setting
    (M.1) says so.
  - Reconnect behaviour: how soon and how often the central comes back, and whether it tries to
    re-encrypt with the session keys.
  - **Key export for analysis (owner decision 2026-11-15; a second switch, off by default):** at
    session end the session's keys (LTK, EDIV/Rand, IRK, CSRK, both addresses) are written to
    `/ext/apps_data/canary/ble/keys/YYYYMMDD-HHMMSS.keys` before RAM is zeroed. Use: decrypting a
    sniffer capture of the same session (Wireshark, the Nordic sniffer) and studying the central.
    The file **never grants anything, ever:**
    - `bled` loads bonds from `/etc/flipper/bt.keys` only; it has no code path that reads
      `/ext/apps_data/canary/`, and its bond loader rejects any file whose first line is the
      export header `# canary-ble-keys v1 ANALYSIS ONLY` (so a copied or renamed export fails too);
    - no tool converts an export into a bond, and any later bond-import tool must refuse the header;
    - the file is mode 0600, owned by the canary's uid, not by `bled`'s;
    - the honeypot's own identity is new each session (fresh random static address and IRK), so
      exported keys match no device the Flipper will ever present again.
    The app warns that the file lets anyone who has it decrypt that session's capture and
    recognise the central's identity address later, and offers "delete all canary logs" (M.1).
  - Personas for the honeypot are idle devices that pair and then do nothing but watch, each a
    `const` GATT table with our own IDs (M.1):
    - **Phone:** a generic phone-style peer (Device Information, Battery, a Current Time and a
      generic "notification" service) that answers reads but originates nothing. Many centrals
      probe a phone for contacts, messages or pairing-for-transfer; all of it is logged.
    - **Keyboard:** HID-over-GATT with a `const` Report Map but **never sends a key report**; logs
      `SET_PROTOCOL`, LED/output reports and any attempt to drive it.
    - **Mouse:** HID-over-GATT mouse that **never sends a movement or button report**; logs the
      host's HOGP setup.
    - **Smart lock** (optional): writes to its "unlock" characteristic are logged and acknowledged
      with an error.
    None of them ever acts: the M.1 rule (answer only the protocol minimum, never type, move,
    notify or drive) holds for every persona.
  - Security: R16.5.5 holds (RPC and serial off), so a paired stranger reaches only the fake GATT
    table. The honeypot ends after 30 min or on Back, and every link is then disconnected.
- **R16.5.2 Scan requests:** where the stack reports received scan requests (Bluetooth 5 "LE Scan
  Request Received" with extended advertising; check the ST stack), the bait also lists **active
  scanners** nearby that never connect. If not supported, the line is hidden.
- **R16.5.3 Central guess:** from the discovery order, MTU value and pairing capabilities: "looks
  like: iPhone", "Android", "Windows", "a BLE scanner app". Labelled as a guess.
- **R16.5.4 Observer (full stack, R16.1.0; shares the 16.3 scan):**
  - **Advertising flood:** counts proximity-pairing advertisements (Apple Continuity, Google Fast
    Pair, Microsoft Swift Pair) from changing random addresses; above a rate (e.g. > 20 new
    addresses per 10 s) it shows "advertising spam nearby" with its type.
  - **Tracker detector** (the F.5 idea, now scheduled): AirTag/Find My, SmartTag and Tile-style
    advertisements whose payload keeps showing up near you over ≥ 30 min across ≥ 2 places or
    movements (accelerometer-free heuristic: the RSSI of other devices changed), with the 16.3
    hot/cold locate. Pocket mode with the alert (M.1).
- **R16.5.5** The canary owns `bled` while armed: the RPC and serial services are off, so a
  stranger connecting to the bait never reaches RPC (`EBUSY` for apps that need `bled`).
- Acceptance:
  - AC16.5.1 nRF Connect on Android connects to the bait; the log lists its discovery, a read, a
    refused write and a refused pairing request with the IO capabilities the phone sent.
  - AC16.5.2 An iPhone and a Linux `bluetoothctl` connect; the guess names both.
  - AC16.5.3 (full stack) A second Flipper running a proximity-advertisement spam app triggers the
    flood alarm within 10 s; an AirTag carried for 30 min triggers the tracker alert.
  - AC16.5.4 No bond is ever stored (the bond file on SD, R16.1.2, is unchanged; CPU2 flash
    unchanged, read back by FUS checksum or the stack's NVM counters), also after a honeypot
    session.
  - AC16.5.5 Honeypot: an Android phone pairs (passkey on screen), reads the encrypted
    characteristic and writes to it; the log shows the pairing method, the key distribution, the
    resolved identity, and the write. After the app exits, the phone's next connection attempt
    fails to re-encrypt (keys gone), and a RAM dump of `bled` holds no non-zero key bytes.
  - AC16.5.6 With key export on, the `.keys` file decrypts a sniffer capture of the session in
    Wireshark. Copying it to `/etc/flipper/bt.keys` (as root) and restarting `bled` gives a load
    error and no bond.
- RAM: `bled` +256 B (connection log ring; the 16.3 scan list is shared) + ≈ 100 B session keys
  in the honeypot; app ≈ 4 KB while running.
  Flash ≈ 6 KB app, `bled` +2 KB.

## Phase 17: Trust: U2F, signed packages, updater, option bytes

**Depends on:** 15 (CPU2 enclave), 3.

- **R17.1** U2F/FIDO HID interface (R10.1), with ECDSA P-256 signing on the **PKA**:
  - operands in the PKA's own RAM (A.6);
  - the attestation and device keys wrapped by the CPU2 secure enclave (AES2 key slots), as stock;
  - an authentication counter in an append-only flash page.
- **R17.2** (old R9.1) Signed packages: SHA-256 of the tar, ECDSA P-256 verify on the PKA against a
  public key in flash next to the system image; unsigned packages need `pkg install -u`. Required
  for runtime installs into the installed region (R2b.5).
- **R17.3** Updater: a signed system image on the SD card is written to the system region by code
  running from the installed region, with interrupts masked and a CRC check after writing.
  Recovery is ROM DFU.
- **R17.4** Option bytes revisited (decision from 2026-11-14, now due): WRP for the system region,
  RDP level 1, PCROP if a secret needs it. Requires the owner's sign-off.

Acceptance:
- AC17.1 Registration and login on a FIDO U2F test site.
- AC17.2 A tampered package is refused.
- AC17.3 An update from SD survives power loss at a random point (worst case: DFU recovery works).

Responsiveness: AC17.R U2F sign completes ≤ 500 ms from the button confirmation (PKA ECDSA P-256 ≈ 10 ms; the rest is USB).

RAM: kernel ≤ +150 B; PKA work uses PKA RAM; the U2F app ≈ 3 KB. Flash: ≈ +10 KB kernel.

## Phase 18: App ports VI: U2F and the ecosystem

- **R18.1** `u2f` app (stock UI: waiting, confirm with OK, success).
- **R18.2** App catalog: package index over RPC (mobile app) and `pkg get`; categories as stock
  (Sub-GHz, RFID, NFC, Infrared, GPIO, iButton, USB, Games, Media, Tools, Bluetooth).
- **R18.3** Porting guide for community apps: `fam2pkg`, the `libfuri` coverage list, the RAM
  rules (chmem, no big static buffers, stream from SD).
- **R18.4** CI: build a list of popular community apps against `libfuri`; report which compile and run.

Acceptance: AC18.1 At least 10 community apps (games, tools) run unchanged except the build file.

---

## Phase 18b: First-boot wizard (owner request, 2026-11-14)

**Goal:** the first time the user turns the device on, a wizard walks them through the settings
the system needs from a person: hostname, region, date and time, location, and it shows today's
sunrise and sunset. Last on the roadmap, because it sets things that the other phases make
real (region: 8; hostname as BLE name: 15; GUI: 5; time: 2d and 6).
**Depends on:** 2d, 3 (root password at first boot, R3.4), 5 (`libfuri`), 6 (RTC), 8 (region
table), 15 (BLE name), section I.

- **R18b.1 Program.** `setup` in the installed region, run by `/etc/rc` when
  `/var/db/setup-done` is missing, before the launcher, as root. It runs on the LCD with the
  d-pad (`libfuri` widgets); on a tty (USB console) the same steps are plain text prompts. The
  marker file holds the wizard version, so a later firmware asks only the steps that are new.
- **R18b.2 Steps,** in this order; every step shows its current value, Back returns to the
  previous step, and every step except the root password can be skipped (a sensible default is
  kept and the summary says "skipped"):
  1. Welcome (and the root password of R3.4, if not yet set; it moves into the wizard here).
  2. **Hostname:** default from the OTP name, as stock; 1–8 characters `[a-z0-9-]` (I.4).
  3. **Region:** default from OTP; a list with the allowed transmit bands of each region; the user
     confirms they will use the device under that region's rules (I.4, R8.2).
  4. **Date, time and time zone:** zone from the `const` zone table (grouped by region); date and
     time entered in **local** time and stored in **UTC**. The field is pre-filled from the RTC
     with a note that, after a switch from the stock firmware, the RTC still holds the old
     local time (I.7). If the phone is connected over BLE or USB RPC, "Take time from phone" fills
     it in.
  5. **Location,** three ways (I.4, I.5):
     - a city from a `const` table in flash (≈ 250 cities: name, country, latitude, longitude,
       zone index; ≈ 6 KB); it also offers that city's zone if it differs from step 4;
     - latitude and longitude typed in (degrees and minutes, or decimal degrees);
     - **from the sun:** today's (or yesterday's) sunrise and/or sunset time as the user observed
       it or read it, giving an approximate position (I.5), with its accuracy shown; near an
       equinox it asks for the latitude as well.
  6. **Summary:** hostname, region, zone, local time and UTC, position, and for today at that
     position: **sunrise, sunset**, day length, and the start of civil and nautical twilight,
     computed by `libastro` from the location and the RTC. If the user entered sunrise/sunset in
     step 5, both the entered and the computed times are shown. OK writes everything.
- **R18b.3 Writing.** All files of I.4 and the RTC are written only at the end, each through a
  temporary file and `rename`, so a power loss mid-wizard leaves the old state and the wizard runs
  again. Then INIT gets `SIGHUP` (new `TZ`), `bled` gets the new name, and the launcher starts.
- **R18b.4 Re-run.** `setup` (all steps) or `setup hostname|region|time|tz|location|sun` (one
  step; `sun` only shows the summary), as root; Settings → System → Setup runs the same program.
- **R18b.5 Manual notes** (I.7) go into the manual chapter "First start".

Acceptance:
- AC18b.1 A fresh card shows the wizard on the LCD after boot; completing it with only the d-pad
  writes the four files of I.4 with the right owners and modes and sets the RTC to UTC.
- AC18b.2 Power cut during step 5: the next boot shows the wizard again; no file is half-written.
- AC18b.3 City "Warszawa", 2026-06-21: the summary shows sunrise and sunset within ±2 min of the
  NOAA solar calculator for that place and day (also for 2026-12-21, and for a place north of the
  Arctic Circle in June: "sun does not set").
- AC18b.4 From the sun: entering the NOAA sunrise and sunset times for Warszawa on 2026-06-21
  gives a position within 0.5° of longitude and 1° of latitude; on 2026-03-20 the wizard asks for
  the latitude.
- AC18b.5 As `nobody`, `cat /etc/location` fails; an app with `Location: yes` can read it.
- AC18b.6 `setup tz` alone changes the zone; `date` shows the new local time at once in a new
  shell; armed alarms (7b) ring at the new local time.

Responsiveness: AC18b.R The first wizard screen appears within L10 of power-on; every step
meets L1; the summary (with the sun computation) appears ≤ 200 ms after OK.

RAM budget: kernel 0, INIT 0; `setup` ≈ 8 KB while running (with the shared libc, `libfuri`,
`libastro`), 0 B after it exits. Flash (installed region): `setup` ≈ 12 KB, `libastro` ≈ 4 KB,
zone table ≈ 3 KB, city table ≈ 6 KB.

---

## Phase 19: Networking: a small TCP/IP stack over USB, a Wi-Fi module and weak links

**Goal:** IP connectivity to a PC, a Wi-Fi network or a second Flipper over the links the Flipper
has, with RAM spent only while the network is up. Decided by the owner (2026-11-15) from the
project report `tcpip-stack-analysis.md` (Polish, not in the repo), sections 1–12 and 13b–14.
**Depends on:** 4b (`poll()`, R4b.6), 10 (second CDC interface); 19c needs 15; 19g needs 16
(full BLE stack, R16.1.0); 19d needs 8.

**Scope (owner, 2026-11-15):** the Flipper is a gadget (door intercom, TV remote and the like), not
a network host. **No TLS/SSL on the Flipper**: an app that needs it runs it alone, as an exclusive
app with nothing else heavy running, or leaves it to the Wi-Fi module. No SSH server. No kernel threads for
networking (the stack is one event loop).

**Stack choice (comparison):**

| Option | RAM while up | Flash | API | Verdict |
|---|---:|---:|---|---|
| MINIX 2 `inet` (in the tree, 15.9 k lines) | ~66 KB as shipped (`BUF512_NR` 128 on 32-bit); 16–20 KB tuned; IPv4 only, Ethernet only, no DHCP | ~60 KB | MINIX `/dev/tcp`, `/dev/udp`, `/dev/ip` + `NWIO*` ioctls, used by the tree's `telnet`, `ftp`, `ping`, `ifconfig`, `tcpd` | Keep its **ABI and commands**, not its engine. Plan B only. (MINIX 3.4 also replaced it with lwIP.) |
| **lwIP 2.2.x** (BSD-3), `NO_SYS=1`, raw API | **≈ 9–10 KB** IPv4, MTU 576; **≈ 14–15 KB** with IPv6, MTU 1280 | 35–50 KB | Raw API in one event loop; MINIX `/dev/tcp` front end on top | **Chosen** |
| lwIP netconn / socket API | +4–8 KB | +10 KB | blocking sockets on lwIP threads | No: needs kernel threads and duplicates `/dev/tcp` |
| uIP, picoTCP, others | — | — | — | No (uIP: one-segment window; picoTCP: unmaintained) |

**Requirements:**
- **R19.1** `inet` (lwIP core, `NO_SYS=1`) is a **`udrv`/`devd` driver process**, not a classic
  MINIX server:
  - `devd` starts it on the first open of `/dev/tcp`, `/dev/udp` or `/dev/ip`; it exits when idle
    with no open descriptors (AC19.3);
  - it runs in a queue **below the interactive queue**; FS is never blocked on it, because `udrv`
    answers callers with SUSPEND and REVIVEs them later (PORTING.md, userspace drivers);
  - one `receive()` loop handles FS requests, frames from the link drivers and the lwIP timer;
  - the 250 ms lwIP timer runs only while a PCB exists, so an idle network does not wake the CPU
    from Stop2.
- **R19.2** Lean memory (A.6):
  - fixed pbuf pools in the server's data segment, no `malloc` after start;
  - default build: **IPv4 only, MTU 576**; IPv6 with MTU 1280 is a build option (`NET_IPV6=1`),
    for link-local use over USB;
  - 4 TCP and 4 UDP PCBs, TCP window and send buffer 2 × MSS;
  - received frames are copied once, by the link driver, into a pool pbuf (R4b.5). There is no
    kernel network buffer.
- **R19.3** ABI and API:
  - the system ABI stays MINIX 2's `/dev/tcp`, `/dev/udp`, `/dev/ip` and `<net/gen/*.h>` `NWIO*`
    ioctls, so the tree's `ping`, `telnet`, `telnetd`, `ftp`, `ftpd`, `ifconfig`, `tcpd` build;
  - one addition: `NWIOTCPLISTEN` takes a backlog (at most 2), so a server does not lose a
    connection between two accepts;
  - **libc** (shared, XIP, 0 B RAM per process) gains a thin BSD sockets layer over that ABI:
    `socket`, `bind`, `connect`, `listen`, `accept`, `send`/`recv`, `sendto`/`recvfrom`,
    `getsockname`, `setsockopt` (`TCP_NODELAY`, `SO_REUSEADDR`), `getaddrinfo` over a small DNS
    resolver. Flash ≈ 3–5 KB. Berry's `socket` module (7c) uses it;
  - sockets are not added to FS as a new file type.
- **R19.4** `poll()` on network descriptors: `inet` answers `DEV_SELECT` (R4b.6) for readable,
  writable and connection-ready. The tree's `telnet` and `telnetd` are changed from one forked
  process per direction to one process with `poll()` (a fork costs a ≈ 15 KB shadow copy here).
- **R19.5** (19a) **SLIP over the second USB CDC-ACM** interface: `slattach` on Linux and BSD.
  Development link; no new USB class code.
- **R19.6** (19b) **USB Ethernet: CDC-ECM and CDC-NCM** as two configurations of the composite
  device (Linux and macOS take ECM, Windows 10 1903+/11 takes NCM). No RNDIS.
  - ECM: one Ethernet frame per USB transfer, streamed from the 64 B packets straight into a pool
    pbuf; kernel RAM ≈ 64 B.
  - NCM: the NDP table may follow the datagrams, so a whole NTB is received before it is parsed;
    `dwNtbOutMaxSize` 2048, the NTB buffer is two pool pbufs in `inet` (not kernel RAM).
  - Kernel flash ≈ 5 KB.
  - `inet` serves addresses itself: a **one-lease DHCP server** (≈ 1–2 KB flash, ≈ 50 B RAM) and an
    **mDNS responder** for `<hostname>.local` (`/etc/hostname`). With `NET_IPV6=1`, link-local
    IPv6 + mDNS also works with no DHCP.
  - Internet over USB needs NAT on the PC (Linux masquerade, macOS Internet Sharing, Windows ICS).
    The user manual documents it; the Flipper does not route.
  - Main use: a **web interface for the gadget**: lwIP `httpd` with static pages in flash (XIP,
    0 B RAM), dynamic parts as small CGI-style handlers returning TSV or JSON Lines (section K):
    reminders, alarms, settings, files on the SD card.
- **R19.7** (19e) **Internet through an external Wi-Fi module** on a GPIO header UART (USART1 on pins 13/14 or LPUART1 on 15/16; LPUART1 when the IrDA module of R19.9 holds USART1). Two
  supported kinds, one `/dev/tcp` for programs:
  - **a Linux module (owner has one): PPP over the UART** (R19.10). The module runs `pppd` and NAT
    and is the Flipper's router; TLS, if ever needed, runs there. No extra code on the Flipper
    beyond PPP. Preferred.
  - **an ESP32 devboard: offload mode.** The ESP32 runs TCP/IP, SNTP and TLS; a daemon on the
    Flipper (`wifid`, ≈ 1–2 KB RAM, no lwIP) maps `/dev/tcp` to its text protocol (ESP-AT, or a
    small own firmware that also carries the `camfind` scan of R9.1.1; one firmware for both).
  - Time sync: SNTP (lwIP `sntp`, ≈ 100 B, or the module's own) sets the RTC, never flash.
  - `pkg get` over plain HTTP is acceptable: integrity comes from the R17.2 signature.
- **R19.8** (19c) **Bluetooth to a phone or PC:** SLIP or PPP framing over the BLE serial GATT
  service of `bled` (a small host script on Linux, an Android companion). BLE IPSP / RFC 7668 is
  **not planned**: phones do not expose it to apps.
- **R19.9** (19f, optional, **SD package** `net-irda`; owner decision 2026-11-15) **IP over infrared
  with an external IrDA module:**
  - The built-in IR receiver is a 38 kHz demodulator and cannot see IrDA SIR pulses, so this
    needs an IrDA transceiver module (e.g. Vishay TFDU/TFBS family) on GPIO pins 13/14 (USART1).
  - USART1's hardware IrDA SIR encoder/decoder (`IREN`) does the pulse shaping: 9.6–115.2 kbit/s,
    **0 B** of kernel RAM beyond the normal UART driver. USART1 is shared with the GPIO UART
    bridge; the usual ownership rule applies (`EBUSY` with the owner's name).
  - Framing is **PPP** with Van Jacobson header compression (lwIP `PPP_SUPPORT`, `VJ_SUPPORT`, 4 VJ
    slots), not SLIP: LCP echo detects a broken link and IPCP assigns the addresses. It uses the
    common PPP code of R19.10.
  - Peers: a second Flipper, or a PC through a plain USB-UART adapter with the same module (the PC
    runs `pppd` on a serial port). No IrLAP/IrLMP/IrLAN stack: Linux removed its IrDA subsystem in
    4.17, so a full stack would have nobody to talk to.
  - Packaging: built as a separate package installed on the SD card (`pkg install net-irda`), not
    part of the base image; `pkg install --flash` stays possible for owners who use it a lot.
  - Cost: flash ≈ 20–25 KB (PPP + VJ, in the package); RAM ≈ 1–1.5 KB inside `inet` while the link
    is up; no internal flash writes at run time.
  - Acceptance: AC19.9 Two Flippers with modules ~1 m apart `ping` each other and copy a 10 KB file
    with `ftp` and a matching `cksum`; the same against Linux `pppd` through a USB-UART adapter.
  - Background: project report `tcpip-stack-analysis.md` §13 (Polish, not in the repo).
- **R19.10** **PPP as the common framing for weak serial links** (Wi-Fi module UART, BLE serial,
  CC1101, the IrDA link of R19.9): lwIP PPP with LCP echo, IPCP, CHAP and **Van
  Jacobson header compression** (4 slots, not the default 16). RAM ≈ 1–1.5 KB over lwIP; flash
  ≈ 20–25 KB. SLIP stays only for R19.5.
- **R19.11** (19d) **PPP over CC1101 between two Flippers, with frequency hopping:**
  - a thin MAC under PPP: packets ≤ 255 B (FIFO refilled from the interrupt), hardware CRC,
    optional FEC with interleaving, CCA before transmit, short link-level acks;
  - **TDD slots** (50–100 ms) with a master and a slave; the master's packets carry the slot
    number; ≈ 1 ms guard per slot;
  - **fast hop:** at link start every channel is calibrated once and `FSCAL3/2/1` stored
    (3 B per channel, TI DN505); a hop writes `CHANNR` + the three values and strobes: ≈ 90 µs
    instead of ≈ 0.8 ms. Recalibrate in a gap after a large temperature change or every few
    minutes. The table lives in RAM only;
  - hop sequence from the pairing key (a permutation from AES-CTR on `AES1`); acquisition waits
    on one channel (worst case channels × slot length); 3 empty slots → reacquire;
  - **adaptive hopping:** both sides keep RSSI and loss per channel; the master broadcasts an 8 B
    bad-channel map and the sequence skips those channels;
  - the hop is done in the kernel PHY (A.1) from a hardware timer interrupt (≈ 10–20 µs of SPI
    every slot); the channel table and statistics are in a pinned buffer of the link process
    (R4b.5), the kernel holds ≤ 48 B of slot state;
  - R8.2 is applied by validating the whole hop table at arm time (every channel inside the
    region's band) and locking `CHANNR`/`FREQ*` to the hop path; the duty-cycle counter sums
    transmit time over all channels of the sub-band. **In the EU hopping does not raise the
    duty budget** (433 MHz ≈ 480 B/s, 868.0–868.6 MHz ≈ 48 B/s average); the region table may
    instead use LBT + AFA where the band rules allow it. US 902–928 MHz (FCC 15.247) allows real
    FHSS without a duty limit. The exact EU figures are checked against the current ERC/REC
    70-03 and EN 300 220 before they enter the table;
  - the link and `/dev/nfc` exclude each other while the link is up (shared SPI); other users of
    the bus get it between slots (as R0.8b);
  - delivery order: one channel + FEC + CCA, then LBT + channel change, then full FHSS with AFH.
    Each step works on its own.
- **R19.12** (19g) **Flipper-to-Flipper BLE Ethernet bridge** (owner idea, 2026-11-15): two
  Flippers, each a USB ECM/NCM device (R19.6) to its own PC, carry Ethernet frames between them
  over a **BLE L2CAP CoC** channel (full BLE stack, R16.1.0). The PC with Internet does NAT for
  the other one.
  - The bridge forwards frames at layer 2 between the USB function and the CoC channel; it does
    not need `inet` (a small `brd` process, two frame buffers of 1.5 KB, MTU 1500).
  - The BLE link is bonded and encrypted before any frame passes (R19.14).
  - Throughput is what BLE gives (tens of KB/s); the user manual says so.
- **R19.13** (old R19.7) **802.15.4 / Thread** on CPU2: owner decision, after Phase 15 (Deferred).
  CPU1 would attach IPv6 packets from IPCC as an lwIP netif; cost 2–4 KB more SRAM2.
- **R19.14** Security defaults:
  - no listening services unless enabled in `/etc/rc.net`;
  - each listening socket is bound to one interface; `telnetd`/`ftpd` only on the USB link
    (never Wi-Fi, BLE or radio) and only with the root password set (R3.4);
  - every daemon runs as its own user;
  - BLE and CC1101 links are paired and encrypted before IP or frames pass;
  - no internal-flash writes at run time (DHCP lease, ARP/ND, calibration, hop tables: RAM only;
    `/etc/rc.net` and Wi-Fi credentials on the SD card).

Acceptance:
- AC19.1 `ping` both ways over SLIP and over ECM (Linux) and NCM (Windows 11); `telnet` from Linux
  to the Flipper's `telnetd`; `ftp` of 1 MB with a matching `cksum`.
- AC19.2 Throughput over NCM ≥ 300 KB/s (USB FS with 64 B packets).
- AC19.3 With the network idle and no sockets, `inet` has exited and `free` shows the RAM back.
- AC19.4 The PC gets an address by DHCP and opens `http://<hostname>.local/` from the Flipper's
  `httpd`.
- AC19.5 A test program using only the BSD sockets layer connects, accepts two clients in a row
  and waits on a socket, `/dev/buttons` and a timer with one `poll()`.
- AC19.6 With a Linux Wi-Fi module on the UART: `ping` to an Internet host through PPP + NAT;
  SNTP sets the RTC.
- AC19.7 Two Flippers over CC1101 (R19.11): `telnet` session with VJ compression; with one channel
  jammed by a test transmitter the link stays up and that channel is marked bad; the duty
  counter never exceeds the region limit.
- AC19.8 Over BLE: `ping` from Linux through the GATT serial bridge (19c); two PCs `ping` each
  other through two Flippers on L2CAP CoC (19g).

Responsiveness: AC19.R While a 1 MB TCP transfer runs over NCM, and while the CC1101 link hops,
L1 and L2 hold (`inet` runs below the interactive queue; hop interrupts ≤ 20 µs).

RAM budget: kernel ≤ +150 B (ECM ≈ 64 B, hop slot state ≤ 48 B); `inet` ≈ 9–10 KB (IPv4) or
≈ 14–15 KB (IPv6) only while running, PPP +1–1.5 KB, hop table ≈ 0.4 KB in the link process,
`brd` ≈ 4 KB while bridging, `wifid` ≈ 1–2 KB. Flash: ≈ 40–50 KB (`inet`) + 20–25 KB (PPP) + 5 KB
kernel (USB Ethernet), in the installed region (or the run-time cache if networking is rarely
used).

---

## Phase 20: Continuous: speed, power, code modernization

Runs alongside the other phases. Two items moved forward: R20a.3 (SD multi-block, DMA) into
R4b.9 and R20a.4 (EXTI, tickless, Stop2) into R6.8.

### 20a. Speed and power

**Depends on:** none. Each item is independent.

- **R20a.1 [M8, A8]** Word and `LDM`/`STM` versions of `memcpy`/`memset`/`memmove` (kernel and
  libc), `flipclicks` word-wise, and SIMD zero-byte detection for `strlen`/`memchr`/`strchr`.
  Keep the C versions for QEMU checking.
- **R20a.2 [A6]** `__builtin_clz`/`ctz`/`bswap32` in FS `alloc_bit`, `mpu_cover`, `flipdev_tick`
  and SHA-256.
- **R20a.3 [M9, A11]** (SD multi-block and SPI DMA were pulled forward into R4b.9; what
  remains here is the CRC unit for `pkg get` and measuring the gain)
  - Multi-block SD reads and writes (`CMD18`/`CMD25`);
  - `CMD59` with the SPI hardware CRC16;
  - the CRC unit for `pkg get`;
  - SHOULD: SPI DMA, with an SPI2 owner flag shared by SD, LCD and swap.
- **R20a.5 Power tuning and measurement** (power analysis 2026-11-15): measure every component
  state on the device (external meter), wake latency from each source, and the battery scenarios
  of the analysis (idle, light use, desk clock, gadget day); fold the numbers into the manual.
  Wake sources that cost nothing extra become features: IR remote wake (the TSOP is always
  powered), phone-tap wake (R14.2.1), wake-on-radio (R8.5).
- **R20a.4 [M10, A5]** (pulled forward into R6.8 power modes and R4b.2 LPTIM1; what remains
  here is tuning and measurement)
  - EXTI interrupts for the buttons, with debounce;
  - tickless idle (no SysTick when no alarm is due);
  - SHOULD: Stop2 with LPTIM1 wake while idle.

Acceptance:
- 1 MB file copy at least 2 times faster than at the Phase 0 baseline.
- `pkg get` throughput not lower.
- Idle current measured lower on the device (record the numbers).
- All earlier tests pass.

Responsiveness: AC20a.R Every L1–L10 number equal or better than at the end of the previous phase; this phase is where budgets get tightened toward stock.

RAM budget: **0 B** (DMA uses existing buffers).

### 20b. Code modernization and isolation

- **R20b.1 [M6]** ANSI prototypes for `src/kernel/arm/`, `flipper/` and every port-modified file.
  Build those with `-std=gnu11 -Wall -Wextra -Wformat=2 -Werror=implicit-function-declaration
  -Werror=return-type`.
- **R20b.2 [M7]** `-flto` for kernel, servers and user programs, if R20b.1 is done for the files involved.
- **R20b.3 [M13]** `-fstack-usage` report per task, checked against the stack sizes in `table.c` by a build script.
- **R20b.4 [M12]** MM and FS run unprivileged with MPU regions (FS: its data plus SRAM2a/2b).
  Relocation, painting and swap I/O go through kernel calls. Remove the FS D-segment stretch (`klib.c:192`).
- **R20b.5 [A4]** SHOULD: PendSV-based switching; optionally keep the hardware frame on the process
  stack (−0.9 KB) once R1.2 is in place.

Acceptance:
- Build is warning-free for the listed directories.
- Flash shrinks with LTO (record the numbers).
- With MM and FS unprivileged, AC1.x still passes, and a deliberate wild write in an FS debug
  build faults in FS instead of corrupting the kernel.

RAM budget: **0 B** (R20b.5 up to −0.9 KB).

---

## Phase 21: Install MINIX on another Flipper (owner request, 2026-11-15)

**Depends on:** 16 (BLE central with passkey pairing, full stack R16.1.0), 17 (signed image R17.2,
updater image format R17.3), 18b (first-boot wizard on the target). Last phase, after 20.
Analysis: `reports/flipper-to-flipper-install-analysis.md` in the project files (2026-11-15).

Goal: a MINIX Flipper installs MINIX on a second Flipper (the owner may buy one), with no PC.
USB cannot do it: both Flippers are USB devices only.

- **R21.1 Fresh image, never a copy.** The installer writes the signed release image from our SD
  card (the R17.3 file), never the contents of our own flash: our U2F keys are wrapped by our CPU2,
  and passwords, settings and installed packages are ours. The signature (R17.2) is checked before
  the first byte goes out. The target starts in the first-boot wizard (18b).
- **R21.2 (21a, main path) Over BLE, acting as the stock mobile app.** Stock firmware takes firmware
  updates over BLE RPC (`rpc_storage.c` writes files to its SD card, `rpc_system.c`
  `system_update_request` starts the stock updater), and the stock updater swaps the CPU2 stack
  through FUS itself. The `flipinst` app:
  1. connects as BLE central to the stock BLE serial service and pairs; the target shows a PIN,
     the user types it on ours (this is the target owner's consent);
  2. checks the RPC `protobuf_version` and refuses unknown versions; the `.proto` files come from
     the stock tree (GPL, see the licence rule); the generated code is the
     shared library `libflipperpb` that `rpcd` also uses (R15.7);
  3. writes a package in the stock updater format to the target's SD card with RPC `storage_write`:
     manifest, `firmware.dfu` with the MINIX image, the full BLE stack (R16.1.0) with its
     `radio_address` and CRC, plus the MINIX files that live on the SD card;
  4. sends `system_update_request`; the stock updater installs the stack, then MINIX, and resets.
  On first boot MINIX sets its own option bytes (the stock updater forces stock `ob.data`).
  The build produces this package (`make stock-update-pkg`); the same package also works by moving
  the SD card into a stock Flipper and opening `update.fuf` in Archive (no radio).
- **R21.3 (21b, rescue) SWD over 3 wires**, for a target whose firmware is broken or has no SD card:
  our pin 2 (PA7) to target pin 10 (SWCLK), pin 3 (PA6) to pin 12 (SWDIO), GND to GND; 3V3 and 5V
  are not connected (both have their own battery). The header has no NRST.
  - Stock turns SWD off (pins analog) unless *Settings → System → Debug* is ON, also in stock DFU.
    The owner accepted this step (2026-11-15). Plan B: poll SWD during the window after a reset.
  - After attaching: halt, `DEMCR.VC_CORERESET`, `AIRCR.SYSRESETREQ`, so no target code runs;
    freeze the watchdogs in DBGMCU; check `DBGMCU_IDCODE` 0x495, the Flipper OTP magic, `SFSA`, OB.
  - If the CPU2 stack has to change: first a small flash-resident radio installer that drives FUS
    like the stock updater and keeps SWD on, then the system image (the full stack does not fit next
    to a finished MINIX). Optional variant: that installer receives the rest over BLE, so the cable is
    needed for a few seconds only.
  - Write order: erase page 0 first, program it last, so an interrupted run leaves no code to start
    and can simply be repeated over SWD.
  - Never write RDP level 2 (0xCC); stop on RDP1/WRP and ask; never touch `SFSA`, `SBRV`, `ESE`,
    `C2OPT`. Clear `OPTVERR` first (errata 2.2.9). Detach before the final reset (RDP1 blocks flash
    reads while a debugger is attached).
  - SWD transactions run in batches inside the GPIO driver (one message per batch, not per bit),
    in slices of at most 1 ms, below the interactive queue. Free-DAP or Black Magic Probe code may be
    reused.
  - Optional backup of the target's CPU1 flash (≈ 860 KB) to our SD card before erasing.
- **R21.4 SWD on MINIX is off by default** (owner, 2026-11-15: it is a protection): pins analog from
  the first instructions of the reset handler, a root-only setting turns it on (kept in an RTC backup
  register, no flash write per boot). Real readout protection is RDP1 (R17.4); ROM DFU by a held button
  stays as the recovery path (AC17.3).
- **R21.5** MINIX to MINIX uses the same BLE path with MINIX's own RPC (`rpcd`, Phase 15) and the
  signed image; no stock updater is involved.

Acceptance:
- AC21.1 A stock Flipper with a charged battery and an SD card becomes a MINIX Flipper over BLE with
  no cable, including the full BLE stack; the target shows the wizard.
- AC21.2 Pulling power from either Flipper at a random point of 21a or 21b leaves the target
  recoverable by running the installer again (21a: the stock updater resumes; 21b: SWD).
- AC21.3 A tampered image is refused before anything is written; an RDP2 write cannot be produced.
- AC21.4 Nothing from our own flash (keys, `/etc/shadow`, settings) appears on the target.

Responsiveness: AC21.R L1 and L2 hold during the whole transfer (BLE is on CPU2; SWD runs in ≤ 1 ms
slices below the interactive queue).

RAM: kernel 0 B; `flipinst` ≈ 3–5 KB while running (RPC buffer, nanopb), 0 B after exit; SWD batch
buffer 512 B only while 21b runs. Flash writes on our Flipper: none (reads from SD only; the
optional backup goes to the SD card). Flash: ≈ 8–12 KB for `flipinst` and the SWD primitive.

---

## Deferred (decided 2026-11-14)

- WRP on the system area, PCROP, RDP level 1 [A15]: after Bluetooth works. Scheduled as R17.4.
- nRF24L01+ on GPIO for 2.4 GHz dongle mice (G.3, question 3): optional, only if the owner confirms.
- Full BLE stack on CPU2 (R16.1.0, for 16.1, 16.3, 16.4): decided by the owner (2026-11-14), installed at the start of Phase 16, after Phase 15.
- Thread or Zigbee on CPU2 (R19.13): owner decision, after Phase 15.
- A FAP binary loader: not planned; apps are ported from source (D.2).
- External USB host chip (e.g. MAX3421E, SPI on GPIO pins 2/3/5, CS 4, INT 7; owner idea
  2026-11-15): would let the Flipper enumerate a device plugged into it (a pendrive or a cable
  with a chip inside) and report what it claims to be, e.g. a "charging cable" that is really a
  HID keyboard. The reverse of the 11.2 canary. Not scheduled; needs the module and a small host
  stack (control transfers and descriptor parsing only).

## Traceability

| Item | Phase |
|-------------|-------|
| S1, A1, S2, S12, S6, S3 | 1 |
| M3, M4, M5, M14, command bss | 2a |
| Flash-installed programs (owner request), shared libc, SFSA read | 2b |
| M1 | 2c |
| S4, S5, S7 (parsing), S9, A12, sticky bit | 3 |
| A3, A2, A17, A15 (ECC), A10, S10, S8 | 4a |
| Driver platform (DMA, poll, ms timer, pulse engine, foreground) | 4b |
| Stock apps: framework and porting | 5, 7, 9, 11, 14, 16, 18 |
| Stock hardware: power, RTC, sound, IR, iButton, GPIO | 6 |
| Native apps: reminder, alarm clock, timers, alerts (owner request) | 7b, section F |
| Gadget apps: hidden camera finder with IR meter (owner request); other ideas as suggestions | F.5, 9.1, 16.3 |
| GPIO sensor modules (DHT, DS18B20, I2C sensors, HC-SR04) and the `thermo` and `sonar` apps (owner request) | H, 7b.4 |
| Scripting (MicroPython/Lua question; owner request) | 7c |
| Native app: Self Test (memory, audio-visual, input, board; modules added by 8, 10, 13, 15) | 7b.3 |
| Native app: Bluetooth adapter for BLE mice and keyboards (owner request) | 16.1 |
| Native app: Storz & Bickel vaporizer remote `sbctl` (owner request; no device firmware update) | 16.4 |
| USB peripheral modes: RFID, iButton, IR, Sub-GHz / NFC reader / BT dongle (owner request) | 11.1 / 14.1 / 16.2, section G |
| USB canary: what an unknown host port tries to do (owner request) | 11.2 |
| Canary family: rules and table; BLE, NFC, Sub-GHz, 125 kHz, IR, Wi-Fi canaries (owner request) | section M; 16.5, 14.2, 9.2 |
| External USB host chip (MAX3421E) for inspecting plugged-in devices (idea) | Deferred |
| nRF24 module for 2.4 GHz dongle mice (optional, not confirmed) | G.3 question 3, deferred |
| Sub-GHz, LF RFID | 8 |
| USB HID, mass storage, CDC | 10 |
| Swap (Part 4), A13 | 12 |
| Background apps: tiers, park/hibernate API, Running list (owner request) | 12d–12f (R12.18 may land with 7b) |
| Swappable memory blocks for apps: `blk`, `libblk`, Berry `vbytes` (owner request) | 12h |
| NFC | 13 |
| BLE, RPC, A16 (HSEM) | 15 |
| A14 + S7 (signing), U2F, updater, A15 (WRP, PCROP, RDP) | 17 |
| A9 (FPU) | 5 (R5.7) |
| TCP/IP over USB, Wi-Fi module, BLE, CC1101 (FHSS); IrDA + PPP as an optional SD package (owner decision) | 19, R19.9 |
| M8, A6, A8, M9, A11, M10, A5 | 20a (parts pulled into 4b and 6) |
| M6, M7, M13, M12, A4 | 20b |
| Responsiveness (owner goal): rule 0.8, L1–L10 | every phase (AC*.R) |
| Time in UTC, Year 2038 (64-bit `time_t`, 32-bit unsigned storage), uptime wrap (owner request) | 2d, R6.2, section I |
| First-boot wizard, location, sunrise/sunset, celestial-navigation groundwork (owner request) | 18b, section I |
| Own versioning: name, semver 0.x → 1.0 at parity, ABI number, tags, CHANGELOG (owner decision) | section J, T7 |
| Byte pipes kept; `-t` structured text; desktop + modules in C/Berry/shell (owner discussion) | section K, T10 |
| `mined` as default editor, safe and VT100-complete (owner decision) | K.5, T8 |
| Official mobile app connects: time sync (local → UTC), device info, file browsing; shared `libflipperpb` (owner request) | R15.7, 21 |
| Install MINIX on another Flipper over BLE (stock mobile-app path), SWD rescue, SWD off by default (owner request) | 21 |
| SD card: clean flag, `fsck` at boot on the screen, sync before ship mode, card pulled out (owner request; no journaling) | section N, T17 |
| M2 | done in `5e3d0f0` (secured by R1.4, extended by 2b) |
