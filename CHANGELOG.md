# Changelog

Galtzagorriak, a MINIX 2.0.0 port for the Flipper Zero. Versioning rules: spec section J
(`docs/spec/galtzagorriak-spec.md`). Newest release first. Feature PRs add one line under
`Unreleased`; the release PR turns it into a version section.

## Unreleased

- (R2b.9, ABI 2) One shared C library in flash: `minix.bin` carries libc once (18 KB at 192 KB into flash, after the system image), and every program calls it in place instead of carrying its own copy. The 96 base programs' text shrinks from 593 KB to 257 KB (the base package from 770 KB to 470 KB; the programs flashed by `make base-flash` from 136 KB to 92 KB), so more programs fit the installed region and the text cache, and a program run from RAM needs less of it; each process keeps about 0.6 KB of libc data (errno, stdio, malloc state), on average 0.26 KB more than before. The installed programs now start at 256 KB. Programs and packages fit only the libc of their own build: exec refuses others (ENOEXEC), `pkg` refuses a package with another `Libc:` id, and `free -v` shows the library. Flash `minix-base.bin` and rewrite the card from the same build. Base 3.71.
- (R8.1, part 1) Sub-GHz listening: `echo 433920000 > /dev/subghz` tunes the CC1101 (stock bands, stock OOK 650 kHz receive settings, the RF path switch as stock) and `cat /dev/subghz` shows the signal strength in dBm; receive only, no transmit path exists yet. Base 3.70.
- (T16, R6.10) Backlight gestures: hold Left+Up 1.5 s to keep the backlight off (keys still work in the dark), Right+Down 1.5 s to keep it on; the same again returns to normal. The status LED blinks red faster and faster while you hold, a short buzz confirms. Also `backlight force off|on|auto`; a reboot gives the normal light back. Apps that need those keys together can turn gestures off (`BTNIOCGEST`). Base 3.69.
- (Phase 2d, ABI 2) Time past 2038: `time_t` is 64-bit for programs (files store times as unsigned 32 bits, 1970 to 2106-02-07; file systems made before mount unchanged); the clock keeps UTC and local time follows `TZ` (`date -u`, `date ... Z` sets UTC, `MMDDCCYY` dates); the uptime counter no longer misbehaves after 414/828 days; `putenv()` now works with `getenv()`; `find`, `login`, `tar` and `mkfs` fixed for 64-bit times, and `make check`/`ramcheck` run a Year-2038 audit. Programs and packages of ABI 1 no longer run: rewrite the card and flash `minix-base.bin` of the same build. FS −72 B RAM. Base 3.68.
- Security review, weekly run 1 (`docs/security/2026-11-16-fs.md`): a damaged file system on the SD card no longer stops the whole system: a bad zone number in an indirect block reads as a hole, freeing an already free zone or inode only prints "run fsck", and a super block whose bit maps do not cover its inodes and zones is refused at mount instead of corrupting the inode table. QEMU test `tools/fsdamage.py`.
- (R4b.9) SD transfers check their CRC16 with the STM32WB's CRC unit instead of in software (a 512-byte sector: ~150 instead of ~10 000 cycles), after a self-test at start; on a mismatch the software CRC stays.
- The LCD is written at 8 MHz instead of 4 (a frame in about 1 ms instead of 2: the CPU is free sooner for everything else).
- Fixes from the device check: `/dev/adc` read nonsense (VREF+ floats on the Flipper): it now turns the internal 2.5 V reference buffer on for the reading, as the stock firmware does; the range is 0–2.5 V, `>` above it, and the line ends with `ref=` (the measured reference). Fault reports showed `addr=e000ed34` for every memory fault on the device: the address is now read before the fault status is cleared. The About screen's uptime counts. Base 3.67.
- Security review of the kernel (`docs/security/`, weekly from now on): a USB host or the header UART flooding the console can no longer freeze the Flipper or make the watchdog reset it (the TTY task works on input in 2 ms slices, and echo to a USB host that does not read is dropped instead of waited for); a damaged SD card can no longer hang the boot with a looping extended partition table or hold the CPU for seconds when it stops answering. PC tests for hostile input: `make -C flipper/tests/host` (partition tables, the USB stack) and `tools/conflood.py` (QEMU).
- (R6.1) Charging settings written to `/dev/power` (root): `profile full|care` (charge to 4.208 V or about 4.0 V), `gentle on|off` (1 A instead of the stock 2 A), `otg on|off` (5 V on the GPIO header); the profile and gentle charging are kept in an RTC backup register. `/dev/power` also shows `vreg=`, `ichg=` and `otg=`. Base 3.66 (`/dev/power` 0644). Once a minute the kernel checks the battery: on a charger, charging pauses outside 0–45 °C (gauge temperature; resumes between 2 and 43 °C); below 3.3 V no new flash erases start.
- Fix (#108): a program started right after an update, with memory short, could get garbage argument pointers and crash once (seen as `signal 11 ... addr=e000ed34` in `ed` and `wc`): exec copied the text to the flash cache through the buffer that held the new stack image.
- About screen: one fact per line (name and version, MINIX base, machine and ABI, host, uptime); the version line no longer runs off the screen. Base 3.65.
- (R6.4, part 1) `cat /dev/adc` shows the voltage in mV on the GPIO header pins with an ADC input (2, 3, 4, 7, 15, 16) that are left analog (`gpio N off`, the default), and the supply (VDDA): 16x hardware oversampling, calibrated by the chip's internal reference. Base 3.64.
- (R6.3) Timed vibration: writing a number of milliseconds (2–999) to `/dev/vibro` buzzes that long, ended by the kernel's ms timer (also from Stop2), so the program need not wait; `0` and `1` still turn it off and on.
- (R6.7, part 1) iButton: reading `/dev/ibutton` with a key on the contacts gives its 8-byte ROM (family, serial, CRC checked); no key: ENODEV. 1-Wire timing in the kernel with interrupts masked at most 70 µs per bit. Base 3.63.
- (R4a.5, part) Flash ECC: an uncorrectable error stops the system with "flash ECC: uncorrectable error at ADDRESS" instead of an unexplained exception; a corrected single-bit error is reported in one line (a page wearing out).
- (R4a.6) Stack overflow detection: a DWT watchpoint guards the kernel stack and the running task's stack; writing past either stops the system with "stack overflow in TASK" instead of corrupting memory silently.
- (R4b.8) The app started from the launcher owns the screen and the buttons (`FGIOCSET` ioctl, `<minix/fg.h>`): frames from other programs are dropped, their button reads wait. Holding Back for a second ends the app (SIGTERM, then SIGKILL after 3 s), as on the stock firmware. Base 3.61.
- (Phase 4a: R4a.1, R4a.2, R4a.3, R4a.7) Kernel hardening: interrupts are masked with BASEPRI, so a fault inside a kernel critical section is reported as what it is; a program whose code is in flash cannot execute from its RAM (stack, heap), and a 32-byte guard at the end of its heap stops a runaway stack with SIGSEGV before it overwrites the heap; flash writes keep the MPU on (only the target page opens, for the kernel); the USB console checks the length of SET_LINE_CODING. `m2test` checks NULL, the stack/heap guard and (with the text in flash) execute-from-stack.
- (T14c, part 2a) `halt` powers the Flipper off for real (the charger's ship mode, as the stock firmware): screen and backlight go dark, the battery is disconnected; hold Back (or plug in USB) to start it again. With USB connected it waits for the cable to go.
- (T14c, part 1) The charger's state: `/dev/power` gains `chg=` (0 not charging, 1 pre-charge, 2 fast, 3 done) and `vbus=` (the charger's VBUS_STAT), and the Power app says "charged" or "charging". The charger's ADC converts on demand instead of every second; with power on the USB connector the Flipper stays out of Stop2 so USB can start. Base 3.60.
- (T14d, part 1) On battery, 2 minutes after the backlight has gone off the screen goes into power save as well (blank); any key brings the same picture back at once. With USB connected the screen stays on.
- (R4b.2) The millisecond timer works in QEMU too (CMSDK APB timers), so its users (`poll()` timeouts, R4b.6) can be tested in CI; the idle loop asks `hal_can_stop2()` before trying Stop2.
- (T15a) Battery on the screen: `/dev/power` (the BQ27220 fuel gauge, read only: charge, voltage, current, average and standby current, temperature, USB) and a **Power** app in the launcher with the current now and the idle current (`docs/manual/power.md`). Base 3.59 (its postinst adds `/dev/power` and the launcher entry on older cards).
- (T14b) Deep sleep when idle: with nothing to do and no USB cable, the Flipper stops its clocks (Stop2) and wakes on a button or its timer (at most about 1 s later); buttons now interrupt instead of being sampled on the clock tick, debounced by a new millisecond timer (LPTIM1). `lat` shows the time in Stop2 and why it was not taken. The UART console on the header loses input while asleep: keep USB attached to use it.
- (T14a) Less current at idle: the core runs from the SMPS step-down converter, and the Sub-GHz and NFC chips sleep from boot (they idled with their crystals running).
- QEMU board: SysTick starts at the lowest priority, so a tick during early boot can no longer enter the kernel before the first process (it locked up about 1 boot in 30 on a busy host: the random `powercut` failures in CI). The Flipper already did this.
- (Phase 2c) `vfork`: the shell, init, the launcher and pkg start programs without copying themselves first, so commands still start when memory is short (`vfork(2)`). Base 3.58.
- The LCD backlight dims 10 % a second from 5 s before its timeout (default 15 s, spec #89) and then fades out (0.5 s); the next key or button brings it back at once (when it is off, that press only turns it on). Reading `/dev/backlight` ends after its 4-byte state.
- A second worker in GitHub Actions (`claude-worker`): the owner labels an issue `claude-task` (or writes `@claude` in a comment) and Claude Code implements it on a branch, checked with the same build, ramcheck, M2 and flash gates; coordination with the local agent by the labels `claimed:cloud` / `claimed:local` (`docs/spec/next-tasks.md`).
- CI gates: flash pages erased in the QEMU M2 run are checked against `flipper/flash-budget.txt` (`tools/flashcheck.py`), the power-cut test (`tools/powercut.py`) and the latency trace build run on every PR, and a `pr-rules` check asks for the CHANGELOG line and the manual update (labels `no-changelog`, `no-manual`); the toolchain's .deb files are cached.

## 0.1.1 (2026-11-15): tasks and fixes after Phase 1

Tasks T2, T4, T7, T8, T9, T11, T12, T13 (core) and T4b steps 1-2, parts of Phases 2b and 4a, infrared,
the backlight timeout, the first chapter of the user manual and CI with downloadable images.
No phase is complete yet, so the minor number stays (spec J.1). ABI 1 (unchanged). Base package 3.55.
Static RAM (ramcheck): kernel 14768 B, MM 10172 B, FS 27196 B, init 764 B.
Images: attached to the GitHub release `v0.1.1`; install as in `docs/manual/install.md`.

- GitHub Actions workflow `build`: every push and PR builds the Flipper images, runs `ramcheck` and the QEMU M2 self-test; images are a download (artifact), and a `v*` tag attaches them to its release.
- The user manual starts in `docs/manual/`: installation, first boot, updating, recovery and returning to the stock firmware (`install.md`).
- (T4b, step 2) SD card data moves by DMA while the disk task sleeps: programs keep running during card I/O (waits during writes: max 77 ms, was 2.2 s).
- (T4b, step 1) While the SD card is busy after a write, the disk task deselects it and lets the other kernel tasks run: infrared and buttons no longer wait for card writes.
- The LCD backlight fades out (0.5 s) after 5 minutes without a key and on at the next key or button (that press only turns it on); `backlight [LEVEL [SECONDS]]` sets brightness and timeout (kept over resets). Base 3.55.
- The base package's postinst checks its edits of `/etc/rc` and says so when one failed (seen once: `ed` crashed during an upgrade).
- (T13) `camfind` core, host-tested: the devboard line parser, camera/IoT scoring with an OUI table from the IEEE registry, Wi-Fi motion correlation, locate mapping, IR modulation (Goertzel) and the Sub-GHz/cellular sweep tables.
- (T12, L.3-L.4) Settings fall back to their defaults in `/rom`: `cfopen()` in libc, `cfcat`, `settings diff|reset`; the launcher's app list and the motd use them. Base 3.53.
- (T11, R2b.12) `pkg bind` rebinds programs copied back (new inode) to the text already in flash; compaction drops records whose file is gone (FS `INOCHK`); exec also checks a CRC of the start of an installed program's text. Base 3.52.
- (I.1, minimal) The clock starts from the RTC at boot (QEMU: the host's time); while it is not set, no program is bound to internal flash (bindings use ctimes). Base 3.51.
- (T12, L.2) The default settings are a read-only image in internal flash (`make base-flash`), mounted on `/rom` (`/dev/rom`); `/etc/rc` reads `rc.conf` from the card, else from `/rom`. Base 3.50.
- `/dev/mem` and `/dev/kmem` read only where the board has memory (RAM, flash below CPU2's, ROM/OTP) and write only RAM: `dd if=/dev/mem` past the end is a read error instead of a kernel panic.
- (T11, R2b.11) The run-time text cache erases at most 32 flash pages a day (`pkg budget`, `fcache_budget=` in `/etc/rc.conf`); `free -v` shows the erase counts.
- (Phase 2b, AC2b.5) Power cuts during installs, tested in QEMU (`tools/powercut.py`): a cut inside a directory record no longer blocks every later install, and a cut during compaction no longer loses installed programs.
- (Phase 2b, R2b.7) The installed-programs directory no longer stays full after base upgrades: compaction drops dead records whose text a live record still has.
- (Phase 2b, AC2b.R2) `pkg flash` / `pkg install --flash` write one flash step per MM call: exec and fork no longer wait for a whole install (L6 10.9 ms during a 105 KB install, was up to ~1 s). Base 3.48.
- (R0.8b) The SD task lets other kernel tasks run after every 1 KB, so an infrared frame no longer fails while the card is read; kernel 288 B smaller.
- (Phase 4a) The RNG and its clock choice are taken with the hardware semaphores CPU2 uses (ready for the radio stack).
- (T8 follow-up) `mined`: one allocation per line, runs in 12 KB; a paste or file insert that does not fit stops with a message instead of ending the editor; old cards get `EDITOR`/`VISUAL` from the base package.
- USB console: output no longer stops for good after a refused packet; the console task never waits for a host that does not read (writers are suspended instead).
- (Phase 4a, R4a.4) Watchdog 2 s, kicked by the clock task (a hung kernel task now resets); a watchdog reset goes to DFU only after `reboot -d` (dev mode), otherwise the board boots normally; `ver` shows the reset cause.
- (Hardware #9, R6.6) Infrared: `/dev/ir` and `ir` (all stock protocols, remote files); send timed by TIM2 interrupts, no busy-wait, kernel +64 B.
- (T8) `mined` is the default editor: memory-safe (B1-B10), VT100/xterm keys, terminal size detection, `.save` when the terminal goes away, safe writes.
- (T9) Pipes no longer write their data or inode to the SD card; freed blocks are not written either.
- (T4) Background flash work waits while a process is ready or a key was pressed; `lat` explains slow wakeups (L5).
- (T2 follow-up) The flash text cache records a program only after its flash copy matches the text's CRC; MM back to its RAM budget, kernel and FS 36 B smaller.
- (T7) Version strings: `uname` reports Galtzagorriak, boot banner, `ver`, ABI stamp in programs and packages.

## 0.1.0 (2026-11-14): Phase 1 complete

First numbered release. Covers everything up to Phase 1. ABI 1.

System
- MINIX 2.0.0 kernel, MM and FS on the STM32WB55 (Cortex-M4); root file system on the SD card.
- Real `init`, getty, login, `/bin/sh` and about 80 commands; launcher with apps; Console app on the LCD.
- Position-independent user programs (separate I&D) and the flash text cache: program text runs from internal flash.
- Memory protection with the MPU; each process is confined to its own memory.
- Packages with `pkg`; the base package can be updated over the USB cable.
- Fork switches 26× faster; exec without heap; faster SD with CRC checks and multi-block writes.

Hardware
- `/dev/random` (hardware RNG), `/dev/zero`, `/dev/gpio` (header pins), `/dev/vibro`, `/dev/speaker`; `gpio` and `beep`.
- Watchdog, `reboot -d` (to DFU), console bell.

Security (Phase 1)
- Closed the local-root holes: sigreturn checks, unprivileged frame access, relocation bounds,
  flash-cache entries keyed by file owner, aligned message pointers, no whole-RAM MPU fallback (R1.1–R1.7).
- `umap()` checks the whole buffer against the process's memory (S13).
- No getty root-shell fallback; `passwd` overflow fixed; SHA-256-crypt passwords.

Measurement
- `make ramcheck` with `flipper/ram-budget.txt`; `latcheck` kernel and `lat` with the L1–L10 baseline in `flipper/PORTING.md`.
- Static RAM: kernel 17556 B, MM 10684 B, FS 27856 B, INIT 1748 B.

Known issues: L1 (button → frame) max 967 ms, L5 max 905 µs, first runs write the flash cache (40 ms with interrupts masked); tasks T2–T5.
