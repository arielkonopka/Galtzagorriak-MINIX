# Next tasks (owner approval 2026-11-14 20:29Z)

Do these before new features: 1–3 in order, 4–5 after them or alongside.
Each task goes on its own branch, branched from **main** (not stacked), and gets a PR naming the task number.
The usual gates apply: `ramcheck` (0 B unless stated), QEMU `M2 USER PASS`, and `lat` numbers before/after in PORTING.md.

## Standing rule (owner, 2026-11-14 20:41Z): few flash writes at run time
When optimizing RAM or flash, keep **run-time** writes to internal flash to a minimum. Writes at build or install time (`make base-flash`, `pkg install --flash`, Phase 2b installed region) are allowed, but should **also be as rare as possible**: only write pages whose contents changed (compare before erase), append instead of rewriting, and don't compact or reformat unless space runs out. Every PR that writes flash at run time must say how often it writes in normal use and why that write can't happen at build or install time. Reviews check this.

## Standing rule (owner, 2026-11-15 22:01Z): docs follow behaviour
The user manual lives in `docs/manual/` (start: `install.md`). A PR that changes something a user sees or does (a command, its options or output, a file or setting in `/etc`, the SD card layout, a build target or image name, the flashing, update or recovery steps, the launcher or an app, a hardware feature that starts or stops working) updates the matching manual chapter **in the same PR**, or says in its description why no change is needed. A feature still only in the spec stays marked **(planned)** in the manual until its PR lands; that PR removes the mark. If no chapter covers the feature yet, a short note in the PR description is enough; the review side turns those notes into chapters. Reviews check this together with the `CHANGELOG.md` `Unreleased` line.

## CI gates (owner, 2026-11-15): what GitHub checks on every PR
GitHub Actions runs these on every PR; a red check means the PR is not ready for review. Run them locally before pushing when you can.
- `build`: Flipper images; `make BOARD=flipper ramcheck` (hard gate, `flipper/ram-budget.txt`); QEMU `M2 USER PASS`; **flashcheck**: the flash pages erased during the M2 run must not exceed `flipper/flash-budget.txt` (`python3 tools/flashcheck.py flash-budget.txt m2 m2.log`). The budget is a ratchet: lower it in the PR that saves erases, raise it only with the reason in the same commit (the flash-writes rule above). `make BOARD=flipper latcheck` must compile.
- `powercut`: `tools/powercut.py` (AC2b.5) in QEMU, all cut points.
- `pr-rules`: code changes add a `CHANGELOG.md` `Unreleased` line (skip: label `no-changelog`); user-visible changes touch `docs/manual/` or the PR description has a line `Manual: <why no change is needed>` (skip: label `no-manual`).
- L1–L10 are **not** measured in CI: QEMU's timing is not the Flipper's. `lat` numbers on the device stay in PORTING.md as before.

## T1. S13: `umap()` bounds (critical security): DONE, #14 merged (1c5dd2d)
Spec: `docs/reviews/umap-bounds-finding.md` (add as R4a.9).
- D/S: the whole range `[vir, vir+bytes)` must lie in `[D.mem_vir, S.mem_vir+S.mem_len)` (clicks). T: inside T.
- Source-only fallback to **T** for reads from user memory (rodata literals). Never for destinations.
- `mm/exec.c gather()`: try T when D fails.
- Tests (`flipper/tests/umapk.c`, package `tests`): kernel address in `read()`/`write()` → EFAULT; buffer running past the stack top → EFAULT; `write(1, "literal", n)`, `execl("/bin/echo", "echo", "ok", NULL)`, `system()` still work.
- RAM: 0 B.

## T2. Flash writes as idle work (R0.8c): L4 40 ms → ≤ 20 µs, L6 1.26 s → ≤ 300 ms
**First, avoid the write (standing rule above).** The best fix for the 40 ms/1.26 s is not doing run-time cache writes at all for the programs people actually run:
- The base userland and the launcher apps go into flash at **build/install time** (Phase 2b R2b.5/R2b.6: `make base-flash`, `pkg install --flash`). They are never written by exec.
- The run-time cache becomes a fallback for other programs only. Copy a program only after it has run from RAM at least **3 times**, never more than one page per 100 ms, and never while L8 applies. Count runs in a small fixed MM table (≤ 64 B, inside the 0 B budget by trimming elsewhere, or reuse existing MM state); nothing is written to flash for the counting.
- The boot-time directory write (~44 ms) must go. Write the directory only when something changed, never as a matter of course at every boot.
- Report in PORTING.md how many flash pages one day of normal use writes (target: 0 when nothing new is installed).
What remains of the writing then follows the rules below.
Today the flash text cache is written inside exec, with interrupts masked for ~40 ms per page, and about 44 ms at boot for the cache directory.
- Exec never waits on the cache. The first run of an uncached program loads its text into RAM and runs.
- A low-priority worker (MM idle work or a kernel idle hook) copies the text to the cache afterwards, **one page (erase + program) per step**, with the scheduler free between steps. Nothing writes while L8 applies (none yet; leave the hook).
- Interrupts must not stay masked across an erase. CPU1 stalls on flash fetches anyway, but SysTick, USB and buttons must stay unmasked, and their handlers plus the vector table must run from SRAM *or* tolerate the stall. Pick the cheaper option and document it. Measure the stall with `lat` (add a "flash stall" event per R0.8e).
- If any directory write is still needed at boot, it moves to the worker.
- Acceptance: L4 ≤ 20 µs and L6 ≤ 300 ms over 20 first runs, AC4a.3 (no lost USB console characters while a page is written), L9 ≤ 25 ms per page and ≤ 1 page/100 ms.
- RAM: 0 B static. The worker reuses MM's `mbuf` or the process's RAM text, which stays until the copy is done.

## T3. L1: button → frame, max 967 ms (budget 30 ms p99 / 50 ms max)
- First find out where the time goes. Use the `lat` ring (button IRQ → `/dev/buttons` reply → LCD write) under the stated load (background `cksum` of a 1 MB SD file, USB output).
- Likely suspects: the SD polled transfer holding the disk task (a kernel task, so above every user process) for whole multi-block reads (R0.8b: chunk ≤ 4 KB, then a preemption point); the launcher swapped or shadowed; flash writes (T2).
- Fix the cause. If it is R0.8b, split SD/exec copies into ≤ 4 KB chunks. The "interactive" queue (R0.8a, R4b.8) can come later.
- Acceptance: L1 p99 ≤ 30 ms, max ≤ 50 ms over 100 presses under load.

## T4. L5: REVIVE → run, max 905 µs (budget 100 µs)
Likely the same cause as T3 (a long kernel path) or the shadow swap (flipclicks of a large image). Measure, then fix or document.

## T5. sh: "out of string space" then SIGSEGV
With `CHMEM_sh` 12 KB, `cat /bin/* /bin/*` should fail cleanly (an error message, exit status ≠ 0) instead of crashing. Fix it in sh. RAM: 0 B.

## T6. `sbctl` protocol core (owner wants the app, 2026-11-14 23:14 Warsaw): after T1–T5
Spec: 16.4. The BLE parts wait for Phases 15–16 (R16.1.0 decided: full stack), but the protocol core does not need the radio, so it can be written now and tested on the host.
- `flipper/apps/sbctl/sbproto.c/.h`: pure C, no I/O, no malloc. Model tables in `const` (flash): Volcano, qvap (Venty/Veazy), Crafty. Functions: detect model from advertised name; encode a request (set target, heater, pump, boost mode, brightness, auto-off with the Crafty security-code write first, find-my) into a list of (characteristic UUID, bytes, with/without response); decode a notification or read into one `struct sb_state` (≤ 64 B). Temperature limits per model enforced in the encoder (R16.4.7).
- Host unit tests (`make -C flipper/apps/sbctl test` with the host compiler): golden byte vectors per model for every request and for sample status packets, including the Venty "0x8000 = not reported" case and the Crafty "ready" bit 0x0100.
- Protocol source: the MIT library `storzandbickel-ble` 0.1.25 (`protocol.py`), not the vendor's JS. Credit it in the file header; GPLv3 project, MIT is compatible.
- Priority (owner, 2026-11-15): the owner has a **Venty**. Write and test the qvap model (Venty/Veazy) first: connect sequence (0x02, 0x1D, 0x30/0x06), 0x01 status decode and write masks, 0x06 settings, 0x0D find-my. Volcano and Crafty tables go in too but are marked `/* unverified */` in the table and in the test names.
- No UI and no `bled` code yet. RAM on target: 0 B (code in flash, state owned by the caller). Flash ≈ 3 KB.

## T7. Own version: Galtzagorriak 0.1.0 (owner decision 2026-11-14): small, after T1–T5, may go alongside T6
Spec: section J. Version 0.1.0 is already tagged (`v0.1.0`); this task puts it into the code.
- `include/minix/gzversion.h` with `GZ_NAME`, `GZ_MAJOR/MINOR/PATCH`, `GZ_RELEASE "0.1.0"`, `GZ_ABI 1`, `GZ_BASE "MINIX 2.0.0"`. Keep `OS_RELEASE`/`OS_VERSION`.
- Build writes `gzbuild.h` with `GZ_BUILD` from `git describe` (empty at a tag, else `+N.gHASH[-dirty]`), rewritten only when it changes.
- `uname()`: sysname `Galtzagorriak`, release `GZ_RELEASE`, version `M2.0.0` (J.2 table). Fix `test3.c` to accept it.
- Banner: `Galtzagorriak 0.1.0+N.gHASH (based on MINIX 2.0.0), ABI 1`, then the Prentice-Hall line.
- `ver` command in the base package (J.2).
- ABI stamp (J.3): `elf2mnx.py` sets `a_version = GZ_ABI`; MM exec: 0 → 1, mismatch → `ENOEXEC` before allocating. `mkpkg.py` writes `Abi:` (programs: N, else `any`); `pkg install` refuses a mismatch with the J.3 message.
- `make versioncheck` (J.4), run together with `ramcheck`.
- Tests: `uname -a` output; an executable with `a_version = 99` fails with ENOEXEC and the system keeps running; a package with `Abi: 99` is refused; old programs (`a_version = 0`) still run.
- RAM: 0 B (strings in flash). Flash: ≤ 1.5 KB including `ver`. No run-time flash writes.
- Add the `## Unreleased` line for each later feature PR from now on (J.4).

## T8. `mined`: default editor, memory-safe and VT100-complete (owner decision 2026-11-14): after T1–T5
Spec: K.5. `src/commands/mined` (MINIX 2.0 original) is not built for the Flipper today; only `ed` is. Review of main @ `63d008c`. Note: user code is built with `-fsigned-char` and has no stack canaries, so a stack overflow can reach a return address.
Bugs and vulnerabilities to fix (all 0 B RAM):
- **B1 (critical)** `get_line()` (`mined1.c:1369`) copies up to `\n` with no limit: into `text_buffer[1024]` (.bss) on load, and into `line_buffer[1024]` **on the stack** in `file_insert()` (`mined2.c:727`, `:735`; used by insert-file and paste). Fix: `get_line(fd, buf, size)`; longer lines are split at `MAX_CHARS - 1` and the status line says "N long lines split".
- **B2** `file_name[80]`: `main` → `load_file` → `copy_string(file_name, argv[1])`; `get_file()` truncates only the last path component. Reject a too-long path with a message.
- **B3** `input()` (`mined1.c:1859`) reads prompts into 80-byte stack buffers with no limit. Fix: `input(buf, size)`, bell when full.
- **B4** unbounded string building: `bottom_line()` `buf[80]` (`mined1.c:600`), `file_status()` `msg[120]` (`:1713`, truncates only after writing), `change()` `mess_buf[80]` (`mined2.c:1125`, `"%s %s by:"`). Fix: `build_string(buf, size, fmt, ...)` and a bounded copy everywhere.
- **B5** `compile()` (`mined2.c:1363`): in `[...]` the `exp_buffer` bound is checked only after a whole range is expanded (writes up to ~93 ints past the end with `[!-~!-~…]`); the list length lives in the low 8 bits, so a list > 255 entries spills into `NEGATE` and makes `check_string` step backwards; with `-fsigned-char`, bytes ≥ 0x80 corrupt opcodes (`NORMAL + c`) and end in `panic`. Fix: check before every store, limit lists to 255 ("List too long"), use `(unsigned char)`.
- **B6** clipboard `scratch_file()` (`mined2.c:989`): predictable `/tmp/mined.<pid><letter>`, `access()` then `creat()` (follows symlinks, race), later yanks only `creat()`, mode 0644; `/tmp` is `777` without sticky bit (`flipper/rootfs/proto:46`). Fix: `unlink` own file, then `open(O_CREAT|O_EXCL|O_WRONLY, 0600)` on every write, random name part from the hardware RNG; sticky bit on `/tmp` in `proto` (as R3 for `/usr/tmp`).
- **B7** terminal hangup (USB or BLE disconnect): `read()` returns 0 → `panic` → `XT()` → `ask_save()` → `getchar()` → `panic` … unbounded recursion, unsaved work lost. Fix: on `read()` 0/`EIO` and on `SIGHUP`, if modified, write `<file>.save` (or `~/mined.save`), mode 0600, no prompts, then exit.
- **B8** `WT()` (`mined1.c:478`) truncates the original with `creat()` before writing: a full SD card loses both versions. Fix: write `<dir>/.<name>.tmp`, copy the mode, `rename()`. If the file has another owner or `st_nlink > 1`, keep today's in-place write.
- **B9** bytes 0x80–0x9F (C1 controls, e.g. 0x9B = CSI) and file names in the status line go to the terminal raw. Fix: show them in reverse video like `^X` (ASCII mode), filter file names the same way.
- **B10** `substitute()` (`mined2.c:1226`) calls `length_of(text_buffer)` before writing the `\0`. Fix: terminate first.
VT100 (0 B RAM):
- **V1** `escfunc()` only knows `ESC [ A/B/C/D/H` on `CHIP == ARM_CM`; Delete (`ESC [ 3 ~`), PgUp/PgDn (`ESC [ 5/6 ~`), End insert `~`, and DECCKM arrows (`ESC O A…D`) / PF1–PF4 (`ESC O P…S`) insert letters. Fix: one CSI/SS3 parser independent of `CHIP` (parameters up to a final byte 0x40–0x7E; **unknown sequences are dropped whole**), `const` map: arrows (both modes), Home (`[H`, `[1~`, `OH`), End (`[F`, `[4~`, `OF`), PgUp, PgDn, Delete, Insert (ignored), PF1–PF4 → file status, search forward, search back, mark. Remove the `INTEL`/`M68000` branches.
- **V2** both 0x08 and 0x7F delete left, also in prompts; delete-under-cursor is `ESC [ 3 ~` (and the existing Ctrl keys).
- **V3** screen size: run-time `xmax`/`ymax` from `ws_col`/`ws_row` (columns capped at 80; buffers stay 80 B). If the size is 0 (the Flipper tty starts with zeros, `tty.c:128`): `ESC 7`, `ESC [ 999;999 H`, `ESC [ 6 n`, read `ESC [ r ; c R` with `VMIN=0, VTIME=2`, `ESC 8`; store it with `TIOCSWINSZ`; no answer = 80×24. `^E` (redraw) measures again.
- **V4** the LCD console sets its own `winsize` (32×10) at start.
RAM (measured, cross-compiled with the user-program flags: `.text` 14 435 B, `.data` 1 082 B, `.bss` 7 385 B, static 8 467 B):
- `key_map` → `const` (−1024 B); `exp_buffer` 1024 → 512 ints (−2048 B, overflow handled by B5); `screen` 1896 → 512 B (−1384 B; a full redraw becomes 4 writes). Target static RAM ≤ 4.5 KB.
Integration:
- Add `mined` to `CMDS` in `flipper/minix.mk` with a measured `CHMEM_mined` (`free -t on`, target ≤ 12 KB; the original `-S 64k` is not usable). `EDITOR=mined`, `VISUAL=mined` in `/etc/profile`. Update `man/man9/mined.9` (key table, line limit, `.save`). CHANGELOG line.
- Host tests like T6: mined built for the host with a small tty shim; regressions for B1–B10 and the key parser.
Acceptance:
- A1 a file with a 5000-character line loads, inserts and pastes without a crash, lines split with a warning.
- A2 a 200-character path, 200 characters typed into every prompt, a 79-character file name on a write error: no crash.
- A3 an 80-character `[!-~!-~…]` pattern gives an error, no write past the buffer (host test with canaries).
- A4 `flipper/tests/`: another uid plants a symlink at the clipboard name; mined does not use it, the target is untouched, the clipboard is mode 0600; `/tmp` has the sticky bit.
- A5 USB unplugged while editing with changes: `<file>.save` exists, mined exits cleanly.
- A6 writing to a full card leaves the original intact and shows an error.
- A7 a file with 0x9B and `ESC` bytes shows markers; the terminal does not react.
- A8 xterm, PuTTY and minicom over USB CDC: arrows in both DECCKM modes, Home, End, PgUp, PgDn, Delete, Backspace, PF1–PF4 work; no key leaves characters in the text.
- A9 a 60×20 terminal with no size set: mined detects it; `stty size` then prints `20 60`.
- A10 static RAM ≤ 4.5 KB; `CHMEM_mined` recorded. A11 still one `write()` per key; no visible echo delay over USB CDC.
- Optional, separate PR after this one: UTF-8 (continuation bytes have width 0, cursor moves and deletes whole sequences, prompts accept UTF-8, `-a` switch for 8-bit terminals; 0 B RAM, ≈ +0.5 KB flash).

## T9. Check that pipe data never reaches the disk (small, any time)
Pipes are inodes on `PIPE_DEV` with data in the block cache (`src/fs/pipe.c:57`). Check whether pipe blocks can be written to the SD card (on `sync`, `update`, or cache eviction). If they can, keep them out of write-back (they are discarded when the pipe is empty or closed). Report the answer in PORTING.md; fix only if writes happen. RAM: 0 B.

## T10. Structured text `-t` and the desktop protocol (spec K): design rules now, code later
- From now on, every new command or app that prints records supports `-t` (K.1); existing commands get it when touched. The shared helper goes into libc with the first user.
- The desktop and module protocol (K.2–K.4) is implemented with Phase 5 (`libfuri`, launcher) and 7c (Berry `gui`, `pipe`, `tsv` modules); alarms stay on `timed` + `alert` (F.2).
- No separate PR now; PRs in those phases name K.

## T11. Fewer run-time flash writes (spec R2b.11, R2b.12; owner request 2026-11-15): after T1–T5
- R2b.11: run-time cache erase budget per 24 h (`FC_BUDGET`, default 32 pages), counter in a free RTC backup register (0 B RAM; add it to the register map in PORTING.md), total erase counters, `free -v` line, `svrctl` + `fcache_budget=` in `rc.conf`.
- R2b.12: `pkg bind` rebinds a stale record by SHA-256 instead of letting exec re-copy the text.
- Acceptance AC2b.9, AC2b.10. RAM: 0 B (4 B in MM only if no backup register is free; say so in the PR).
- Report in PORTING.md: flash pages erased in one day of normal use (target 0).

## T12. Default settings in ROM, changes on the SD card (spec section L): after T11
- L.2: `mkfsimg.py` builds the read-only `/rom` image from `flipper/rootfs/etc` (≤ 32 KB), `make base-flash` places it in the installed region; memory-driver minor `/dev/rom` (read-only, XIP); `/etc/rc` mounts it on `/rom`.
- L.3–L.6: `cfopen()` in libc, `cfcat`, `settings reset/diff`; switch `/etc/rc`, the launcher and `/etc/setup` to it. The launcher menu item can wait for the Settings app.
- Acceptance ACL.1–ACL.3. RAM: 0 B static. Flash writes: only at `base-flash`/DFU.
- BLE bonds on the SD card (R15.4) are done in Phase 15, not here.

## T13. `camfind` core (owner request 2026-11-15 20:23 Warsaw): core DONE, #75 merged (dcfd0db); the ESP32 firmware moved to "Deferred to the end" (#78)
Spec: F.5, 9.1 (R9.1.1–R9.1.9), 16.3. The app needs Phases 6 (UART bridge, `/dev/adc`), 8 (CC1101) and 7b (GUI), which are not done, so this task does the parts that need none of them, the same way T6 does for `sbctl`.
- `flipper/apps/camfind/cfcore.c/.h`: pure C, no I/O, no malloc, caller owns all state.
  - Line parser for the devboard protocol (`AP`, `STA`, `RATE`, R9.1.1), robust to garbage and cut lines.
  - Scoring (R9.1.2): `const` sorted OUI table (3-byte prefix + vendor index) built by `tools/mkoui.py` from the IEEE registry CSV plus a hand-kept camera-maker list in the repo; SSID patterns; locally administered MAC → "unknown". Levels: likely camera / IoT module / other.
  - Motion correlation (R9.1.8): the still/move window test on 250 ms rate samples, ratio of medians and correlation; verdict "watches this room" when ≥ 2 in both rounds.
  - Locate mapping (R9.1.5): smoothed RSSI → pitch, beep rate, bar; reference on OK.
  - Goertzel on ADC blocks (R9.1.7): steady vs 50/60/100/120 Hz vs kHz PWM.
  - Sub-GHz tables (R9.1.3): band steps and the cellular mode (LTE B20 832–862, B8/GSM 880–915 MHz), peak hold and burst counter on an RSSI stream.
- Host unit tests (`make -C flipper/apps/camfind test`, host compiler): parser fuzz cases, OUI lookups, a recorded or synthetic camera rate trace that passes the motion test and a phone trace that fails it, Goertzel on synthetic signals, burst counter on a synthetic RSSI trace.
- `esp32/camscan/`: moved to "Deferred to the end" below (owner decision 2026-11-15, #78).
- No UI, no drivers yet. RAM on target: 0 B (code in flash, state owned by the caller; budgets in R9.1.x apply when the app is built). Flash ≈ 6 KB for the core + ≈ 1.5 KB OUI table.
- Hardware facts for later (from the 2026-11-15 schematic check): IR TX LEDs are not readable by the MCU; header ADC pins 3, 4, 7, 15, 16 (pin 2 = external IR TX).

## T14. Power management, part 1 (owner request 2026-11-15 22:33 Warsaw): T14a any time, T14b–T14d in order
Source: the power budget analysis of 2026-11-15. **Analysis, code places and the PR plan with on-device tests: `docs/spec/power-plan.md`** (owner: the agent tests each PR on the Flipper). The facts each step needs are copied here. Spec: R6.1, R6.8, R4b.2, F.3, R20a.4; these steps pull them forward and correct F.3. Responsiveness first: no step may make L1–L10 worse (`lat` before/after in PORTING.md). No run-time flash writes and no option-byte writes in any step.

Hardware facts (official main-PCB schematics, stock `furi_hal_power.c`, `furi_hal_rtc.c`, `furi_hal_os.c`, `scripts/ob.data`):
- Power tree: BAT → BQ25896 BATFET → SYS. SYS → LM3281 (always on) → 3.3V (MCU, LCD, CC1101, ST25R3916 logic, buttons). SYS → second LM3281 with EN = **PA3 (PERIPH_POWER**, 10k pull-up, so on by default) → ext3.3V: **microSD and header pin 9 only**. SYS → FAN48610 5 V boost (always on) for the NFC/RFID board. TSOP75338 IR receiver on SYS through 10 Ω (0.35 mA, always on, no control). BAT → RS3221 LDO → **MCU VBAT** (RTC and backup registers survive power-off).
- **BUTTON_BACK (PC13) is also the BQ25896 QON input.** The charger INT and the gauge GPOUT are not routed to the MCU (test pads only): USB plug and low battery must be polled over I2C.
- EXTI lines in use or reserved: buttons 3 (OK, PH3), 6 (Down, PC6), 10 (Up), 11 (Left), 12 (Right), 13 (Back); IR RX PA0 (0), CC1101 GDO0 PA1 (1), NFC IRQ PA2 (2), iButton PB14 (14). Header pins with a free EXTI line: only PA4 and PA7.
- Stock option bytes have `IWDGSTOP=1`: the IWDG keeps counting in Stop2. Our IWDG is 2 s.
- Stock idle current is 1.5 mA (Flipper blog, FW 0.82). Battery 2100 mAh.

### T14a. Zero-RAM power fixes at boot (small, any time)
- Enable the SMPS in step-down mode after the clock setup (stock: `LL_PWR_SMPS_SetMode(LL_PWR_SMPS_STEP_DOWN)` in `furi_hal_power_init`; read RM0434 for the SMPS clock source and the order against the PLL). Halves the run current.
- Park the radios: SPI1 pins, CC1101 CS (PD0) and NFC CS (PE4) high. CC1101: `SRES` strobe, wait for CHIP_RDYn, then `SPWD` (after a power-on reset it sits in IDLE at ~1.7 mA). ST25R3916: direct command `SET_DEFAULT`, then op-control register 0 (power-down). Use the stock register values (`furi_hal_subghz_init/sleep`, `furi_hal_nfc_init` + `furi_hal_nfc_low_power_mode_start`; GPL, fine).
- Unused pins to analog mode; `DBGMCU` stop/standby clocks off unless the debug flag is set.
- Acceptance: CC1101 `MARCSTATE` read back = SLEEP and ST25R3916 op-control = 0 at the end of `hal_init`; QEMU board unaffected; idle current before/after in PORTING.md if a meter is at hand (otherwise say so).
- RAM: 0 B. Flash: about +300 B.

### T14b. EXTI buttons, tickless idle, Stop2 (R6.8, R4b.2, R20a.4)
- Buttons on EXTI (lines above), debounce only while a button is down. `flipdev_tick` stops polling buttons.
- LPTIM1 on the LSE as the one-shot wake timer. Before sleeping, `idle_task` computes the next due time (clock alarms, `REVIVE`/ms timers, the backlight timeout), arms LPTIM1 and stops SysTick; after waking it adds the slept time to the uptime and `realtime` and restarts SysTick.
- **Sleep blocker:** a 32-bit mask in the kernel (4 B). A driver sets its bit while it needs fast clocks (SPI/I2C/DMA transfer, pulse engine armed, speaker PWM, USB attached, UART console active). Stop2 only when the mask is 0 and nothing is runnable; otherwise WFI as today.
- Stop2 entry/exit as stock `furi_hal_power_deep_sleep` (without the CPU2 semaphores, CPU2 is not started): PLL → HSE → HSI16 with `STOPWUCK=1`, `LPMS=Stop2` (also write `PWR_C2CR1.LPMS` as stock does), `SLEEPDEEP`, `WFI`; after: HSI16 → HSE → PLL, RTC shadow resync. **Serve a button interrupt on HSI16 at once** and finish the HSE/PLL switch afterwards, so the crystal start-up never adds to L1.
- **Watchdog:** without touching option bytes. Either reprogram the IWDG to its longest period (LSI/256, 4095 ≈ 32 s; wait for PVU/RVU) before a long sleep and back to 2 s after waking, with LPTIM1 waking every ~25 s to kick it; or always wake every ~1.5 s. Pick one and say which.
- Statistics (≤ 16 B): time in Stop2, number of wakes per source; printed by `/dev/power` (T14c) or a `lat` line.
- Acceptance: AC6.R (button wake meets L1: ≤ 50 ms max, measure p99/max over 100 presses from Stop2), the clock does not drift more than the LSE over 1 h of idle, the IWDG never fires in 8 h of idle, USB console still works (USB attached blocks Stop2), QEMU path unchanged (QEMU has no Stop2: WFI only).
- RAM: kernel ≤ +24 B. Flash ≈ +1.5 KB.

### T14c. `/dev/power` and real power-off with alarm wake (R6.1, F.3, AC6.1, AC6.2)
- BQ27220 and BQ25896 drivers on I2C1 (stock `lib/drivers/bq27220.c`, `bq25896.c`). `/dev/power` read: SoC %, voltage, current, temperature, charging state, VBUS, plus the T14b statistics. Charger ADC **one-shot on demand**, not continuous (stock leaves it continuous).
- Polling (no interrupt lines): every 1 s while the screen is lit, every few minutes in Stop2 (SYSDWN → `SIGPWR` to INIT; USB plugged → start the USB stack). No flash writes below a low-voltage threshold.
- **Battery profiles (R6.1, owner 2026-11-15):** `full` / `care` (end-of-charge voltage 4.208 V / ~4.0 V via `VREG`, as stock `bq25896_set_vreg_voltage`), kept in an RTC backup register. **`ICHG`, the input current limit and ICO stay at the stock values and are never raised** (owner: within the manufacturer's limits). One exception, owner 2026-11-15: **gentle charging** (battery care, off by default) lowers `ICHG` to ≈ 1 A; the driver clamps any request to ≤ the stock value. Otherwise only pause charging while Sub-GHz transmits and outside 0–45 °C (gauge temperature). Test: `ICHG` read back = stock with gentle off, ≈ 1 A with it on, charging current from the gauge matches.
- **Power-off = BQ25896 ship mode**, as stock `furi_hal_power_off`: enable the RTC alarm output (Alarm A, open-drain, active low) **on PC13**, **switch ext3.3V on** (stock: "shutting down with ext 3V3 off is causing LSE to stop"), short vibro, then the ship-mode command. With USB attached ship mode cannot cut SYS: sleep instead.
- **Alarm while off:** the RTC (on VBAT) pulls PC13 = Back = charger QON, the charger turns the BATFET on and the Flipper boots. This replaces F.3's "Standby or Shutdown". At boot, read the wake reason (RTC ALRAF, backup register) **before** reading boot key combos (Back looks pressed until the flag is cleared), then turn the alarm output off and send `SIGRTC` as F.2 says.
- **Boot lead for alarms (owner, 2026-11-15):** a start from off takes QON (~1 s) + L10, so at power-off `timed` arms Alarm A **early** by the boot lead and keeps the real due time in an RTC backup register (0 B RAM). After the boot `timed` waits for the real time, so the alarm rings on the second, not seconds late. The boot lead is measured at each start from off (RTC time at the first frame minus the alarm time, plus a 2 s margin) and kept in the same backup register; first default 10 s. `Missed` (F.2) applies only if the boot ends after the real time.
- Acceptance: AC6.1, AC6.2; an alarm set 3 min ahead powers on a switched-off Flipper and rings within 1 s of its time (record the boot lead); Up+Back self test still works after a normal power-on.
- RAM: kernel ≤ +32 B. Flash ≈ +3 KB.

### T14d. Screen-off state and peripheral power (after T14b)
- Backlight: **default idle timeout 15 s** (owner 2026-11-15: 5 min is too long). The code default is still 300 (`hal_bl_state()` fallback in `flipper/hal/flipper/board.c`, comments in `flipdev.c` and `cmds/backlight.c`): change it to 15. Optional dim stage before off (off by default) with the LP5562 engine ramp from #81 (0 B RAM). Separate timeouts on battery and on USB.
- LCD power save (display off + all points on: the ST7567 keeps its RAM) a few minutes after the backlight goes off, on battery only; any button leaves power save with no redraw. Desk mode (R7b.2) keeps the LCD on.
- SD card power: with the screen off and the card idle for N minutes (no open writes, cache flushed, card not busy), switch ext3.3V off (PA3). Start re-initialising the card at the first button press, in parallel with the backlight ramp, so the ~300 ms card start is hidden. Header pin 9 also loses 3V3 then: never cut it while `/dev/gpio` has a user or 3V3 output is enabled.
- Acceptance: the first frame after a key in screen-off meets L1; opening an app from the SD card after a power-cut meets its L6 budget, or the PR says by how much it misses; idle current with screen off recorded.
- RAM: 0 B (a few bits in existing flags). Flash ≈ +1 KB.

Out of scope here (later phases): CPU2/BLE low-power coordination (Phase 16, HSEM protocol as stock), new wake sources (IR remote, NFC field detect, CC1101 wake-on-radio, iButton touch).

## T15. Power management app `power` and `pm` (spec R7.1; owner request 2026-11-15): after T14c and T14d
The owner wants the user to control **how deeply and how soon the device powers down**, and every other power knob that makes sense. Read R7.1 for the full list.
- Kernel side first (small PR): a `struct pm_policy` packed into 2–3 free RTC backup registers (add them to the map in PORTING.md): sleep depth, idle-ladder delays (dim, backlight off, screen off, SD off, auto power-off), low-battery thresholds, wake-source mask (buttons, IR, NFC field, wake-on-radio), BLE mode, 5 V / 3V3 header mode, LED, battery profile, gentle charging, active profile per power source. `ioctl` get/set on `/dev/power`; the idle ladder and wake sources from T14b/T14d read it. List of sleep-blocker bits with the holding process for "who keeps it awake".
- `pm` command (shell): `pm`, `pm profile saver`, `pm set screen-off 120`, `pm who`, `-t` output.
- `power` GUI app in `flipper/gui/` (like `lights.c`), launcher entry "Power": battery screen, profile choice, each knob, "who keeps it awake", power off (with armed-alarm count), reboot.
- Persistence: the registers, plus a copy in the SD settings file (section L) restored at boot if the registers lost their magic. No internal-flash writes.
- Tests on the Flipper (agent): AC7.0; each wake-source switch really stops/starts that wake (e.g. pocket mode: Up does not wake, Back does); automatic power-off after a short test delay, then an alarm armed before it rings; `saver` vs `balanced` idle current recorded; L1 for every key in the app and from every sleep state; L6 for opening the app.
- RAM: kernel ≤ +16 B (if the registers are not enough); app ≈ 2 KB while open. Flash ≈ 6 KB.

## Still open from reviews
- IR (#46): `lat` while a frame goes out, a test against a real TV/remote, and the flipdev stack high-water after a long frame (`ir_write` keeps 64 B on it). (The kernel +64 B was won back in #49.)
- T3 (#27): L1 p99/max still ~0.75–0.92 s; long-press and repeat events (stock Press/Short/Long/Repeat) when R4b.8 lands.
- R2b.12 (#72): measure L9 during a forced installed-region compaction on the Flipper (it now asks FS `INOCHK` per record) and write the number in PORTING.md. Optional: `in_lookup`'s head-CRC check uses 2 `lseek` + 4 x 64 B `read` per exec; one 256 B read into a buffer free during exec would halve the FS round trips if L6 shows it.
- I.1 (#70): the stock firmware keeps the RTC in local time; spec I.1 wants UTC. Fix with the time-zone setting (18b wizard / `setup tz`); until then `date` is off by the zone offset (bindings are not affected).
- Backlight (#81), polish: fade **on** as well as off (owner wish: no snapping). Off fades in 0.5 s by LP5562 engine 1; add a short up-ramp (about 150 ms) on wake with the same engine program.
- base postinst (#79), nit: `chmod 755 /rom` runs on every install and may print EROFS when /rom is mounted; make it quiet or run it only after `mkdir`.
- Build (found while writing `docs/manual/install.md`): in a fresh clone `art/galtzagorri.png` can be newer than the committed `hal/common/splash.c`, so `make` regenerates it and then needs Pillow and `/usr/share/fonts/X11/misc/6x10-ISO8859-1.pcf.gz` (xfonts-base); without them the build fails. Make the splash rule opt-in (e.g. `make splash`) so a normal build needs neither; then drop them from the manual's prerequisites.
- Flipper `NR_SUPERS` is 4 and full since T12 (root, /rom, /usr, /home): any further mount needs a fifth slot (RAM) or an on-demand /rom.

## Deferred to the end (owner decision 2026-11-15, #78): Wi-Fi / ESP32 devboard work
The owner cannot find the Wi-Fi devboard for now. Everything that needs it or ESP-IDF waits until the rest of the plan is done (roadmap Phase 22):
- `esp32/camscan/`: the devboard firmware of R9.1.1 + R9.1.8 (ESP-IDF, GPL, passive only: no association, no deauth, no injection). Channel hopping, `AP`/`STA` lines, `CH`, `TRACK`, `RATE`. Build with `idf.py build`; the PR says how it was tested (ESP32 board, real camera if available). The Flipper-side parser and motion test are already in `cfcore` (#75).
- R9.2.4 `wifi` canary (extends the same firmware).
- R19.7 ESP32 offload mode (Wi-Fi module networking). Phase 19 parts that do not need the devboard (USB, BLE, CC1101, a Linux module on the UART) keep their place.
- `camfind` v1 ships without Wi-Fi meanwhile (AC9.1.3: "Wi-Fi unavailable", the other scans run).
