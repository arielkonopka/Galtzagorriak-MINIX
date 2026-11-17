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

## Two workers (owner, 2026-11-15): the local agent and the GitHub Actions worker
Two agents implement tasks in parallel: the owner's **local agent** (has the Flipper) and the **cloud worker** (`.github/workflows/claude-worker.yml`, instructions `.github/claude-worker.md`; no hardware). To never work on the same task:
- The cloud worker only takes **GitHub issues labelled `claude-task`** by the owner, never tasks straight from this file. It labels the issue `claimed:cloud` when it starts.
- The local agent, before starting a task, checks the open issues: a task whose issue is labelled `claude-task` or `claimed:cloud` is not its task. When it starts a task that has an issue, it labels that issue `claimed:local` (the cloud worker then ignores the issue and `@claude` comments on it).
- Good cloud tasks: host-testable code (QEMU, `make`, unit tests), tools, docs, spec-to-code work without timing needs. Device work (radio, USB, SD timing, `lat` numbers) stays local; the cloud worker lists such checks under "Needs on-device test" in its PR, and the local agent runs them.
- The cloud worker pushes a `claude/issue-N-…` branch; the owner or the review side opens the PR from it (one click on the link in the worker's comment), so CI runs. Reviews and squash-merges as for every PR. A fix on a PR: comment `@claude <what to fix>` (owner only).

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
- Acceptance: CC1101 `MARCSTATE` read back = SLEEP and ST25R3916 op-control = 0 at the end of `hal_init`; QEMU board unaffected; idle current before/after in PORTING.md if a meter is at hand (otherwise say so; the T15a idle reading works as a meter for this).
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
- **`After: off` (F.3, owner 2026-11-17):** a timer with `After: off` (or `timed --then-off`) powers the Flipper off again after its job, **only if this boot came from the RTC alarm** (wake-reason bit in a backup register), no button was pressed since and no user session is active; a Flipper that was already on stays on. Last of several close jobs; next timer < ≈ 4 min → Stop2 instead; `Timeout:` (default 60 s) ends a hung job and still powers off; USB attached → sleep; quiet boot (no splash, backlight, CPU2, launcher) when an `After: off` job is due, a button press makes it a normal boot. `Type: emergency` (owner's name for the rescue-signal case) = `After: off` + ignores do-not-disturb, never skipped (`Missed: fire-once`), at low battery `timed` halves its rate per halving of the charge (below 20 % every 2nd skipped, below 10 % every 2nd of the rest, ... max 1/64; level + counter in RTC backup bits, level passed in `TIMED_LOWBAT`), only for packages with `Timers: emergency`. `Type: wakeup` (alarm clock): below 10 % (configurable) the alarm is not run; instead one minimal notice that it was skipped (LED blink by default, or a short beep, `lowbat_notice=`), logged, shown by the alarm app later. The `timed` part is `lane:cloud` (QEMU: fake wake reason), the wake-reason bit and quiet boot are local.
- Acceptance: AC6.1, AC6.2; an alarm set 3 min ahead powers on a switched-off Flipper and rings within 1 s of its time (record the boot lead); Up+Back self test still works after a normal power-on. With `After: off`: off → RTC wake → job → off again; the same timer while the Flipper is on → job runs, Flipper stays on; a button press during the quiet boot → stays on.
- RAM: kernel ≤ +32 B. Flash ≈ +3 KB.

### T14d. Screen-off state and peripheral power (after T14b)
- Backlight: **default idle timeout 15 s** (owner 2026-11-15: 5 min is too long). The code default is still 300 (`hal_bl_state()` fallback in `flipper/hal/flipper/board.c`, comments in `flipdev.c` and `cmds/backlight.c`): change it to 15. Optional dim stage before off (off by default) with the LP5562 engine ramp from #81 (0 B RAM). Separate timeouts on battery and on USB.
- LCD power save (display off + all points on: the ST7567 keeps its RAM) a few minutes after the backlight goes off, on battery only; any button leaves power save with no redraw. Desk mode (R7b.2) keeps the LCD on.
- SD card power: with the screen off and the card idle for N minutes (no open writes, cache flushed, card not busy), switch ext3.3V off (PA3). Start re-initialising the card at the first button press, in parallel with the backlight ramp, so the ~300 ms card start is hidden. Header pin 9 also loses 3V3 then: never cut it while `/dev/gpio` has a user or 3V3 output is enabled.
- Acceptance: the first frame after a key in screen-off meets L1; opening an app from the SD card after a power-cut meets its L6 budget, or the PR says by how much it misses; idle current with screen off recorded.
- RAM: 0 B (a few bits in existing flags). Flash ≈ +1 KB.

Out of scope here (later phases): CPU2/BLE low-power coordination (Phase 16, HSEM protocol as stock), new wake sources (IR remote, NFC field detect, CC1101 wake-on-radio, iButton touch).

## T15a. Battery current on the Flipper's screen, early (spec R7.1 battery screen; owner request 2026-11-16): any time, needs only the I2C1 bus
The owner wants to check the idle current **without opening the Flipper**. A USB meter cannot do it: USB keeps the device awake and charging, and its resolution is ~10 mA. This is the first slice of T15 (its battery screen) and of T14c (the read side of the BQ27220 driver), done now so that T14a (#112, ~1.7 mA expected saving), T14b and T14d can be checked on the device.
- **Kernel, `flipdev`:** new minor 8 **`/dev/power`**, read only for now. A read talks to the BQ27220 (I2C1, 7-bit address 0x55, the bus `flipdev` already drives for the LP5562) and returns **one text line**, then EOF:
  `soc=87 mv=3912 ma=-6 avg=-5 sby=-5 temp=243 flags=0x0181`
  from the gauge's standard commands (check the codes against stock `lib/drivers/bq27220_reg.h`): StateOfCharge (%), Voltage (mV), Current (signed mA, negative = discharging), AverageCurrent (the gauge's own filter), StandbyCurrent, Temperature (0.1 K in the gauge, printed in 0.1 °C), BatteryStatus (raw). An I2C error gives `EIO`, so the app can say "no gauge" (QEMU: the device returns `ENXIO`, the QEMU board has no gauge).
  - **Read only. Never write the gauge** (no control subcommands, no data-memory access): its data memory is flash, and the stock firmware already configured it. T14c adds the charger and the write side later and keeps this line, adding fields at the end.
  - No polling and no cache: the read is done on demand in the task, on the task stack. 0 B static RAM. One read is 7 short I2C transfers at 100 kHz (~3 ms); fine for L1, but say the measured time in the PR.
  - Device node `/dev/power` (character, major 6 minor 8, mode 0444, group `operator` can read later; for now world-readable is fine: it is only battery data).
- **App `power` in `flipper/gui/`** (like `lights.c`), entry "Power" in `/etc/apps`, added to `GUIAPPS`. One screen for now, title "POWER":
  - **Now:** Current in mA with sign (large), refreshed every 1 s while the backlight is on. Below it: average mA, battery %, voltage, temperature, and "charging" / "on battery" from BatteryStatus (DSG bit).
  - **Idle:** the AverageCurrent read while the backlight had been **off for at least 30 s**, with its age ("Idle -5 mA, 2 min ago"). This is the number the owner wants: the screen's own backlight and the app's refresh are not in it. While the backlight is off the app reads only every 10 s (a wake every 10 s costs microamps) and keeps the last such reading. A key press only turns the backlight on (existing rule), and the screen already shows the idle value. Before the first such reading the line says "Idle: leave it 1 min".
  - If USB is connected (gauge current ≥ 0 or DSG clear) show "USB: unplug to measure" instead of the idle value: with USB the reading is not the idle draw.
  - Keys answer within L1: wait for a key **blocking**, with `alarm()` for the refresh (the read returns `EINTR`), never a `sleep()` + poll loop. Back quits. No other keys yet (the rest of T15 adds the menu).
  - Uses `read()` on `/dev/backlight` (`HAL_BL_OFF`) to know whether the backlight is off. No flash writes; the app keeps its values in its own RAM only (well under 1 KB).
- **Shell:** `cat /dev/power` prints the same line (the twin `pm` comes with T15).
- **Docs:** `docs/manual/power.md` (now marked planned) loses its **(planned)** mark and states what the app really shows; `PORTING.md` lists minor 8 in the `flipdev` table.
- Acceptance (agent, on the Flipper): (1) the line's %, mV and mA match the stock battery screen within 2 % / 1 mA (flash stock, note values, flash back; as T14c test 1, can be done once for both); (2) idle reading on battery after 1 min: record it in PORTING.md under "Power" for the current main (with T14a), and, if easy, for the build before #112 for a before/after; (3) with USB plugged the screen says so; (4) L1 for Back and for the backlight-on key while the app runs; (5) QEMU: the app opens, shows "no gauge", Back works, `M2 USER PASS` unchanged.
- Limits to say in the manual: the gauge measures in whole mA, so below ~2 mA the value is coarse (use the average, or the SoC drop over 24 h, or an external meter on the battery leads; `power-plan.md` section 3).
- RAM: kernel 0 B static. App < 1 KB while open. Flash ≈ +1.5 KB (driver ≈ 0.5 KB, app ≈ 1 KB).

## T15. Power management app `power` and `pm` (spec R7.1; owner request 2026-11-15): after T14c and T14d
The owner wants the user to control **how deeply and how soon the device powers down**, and every other power knob that makes sense. Read R7.1 for the full list.
- Kernel side first (small PR): a `struct pm_policy` packed into 2–3 free RTC backup registers (add them to the map in PORTING.md): sleep depth, idle-ladder delays (dim, backlight off, screen off, SD off, auto power-off), low-battery thresholds, wake-source mask (buttons, IR, NFC field, wake-on-radio), BLE mode, 5 V / 3V3 header mode, LED, battery profile, gentle charging, active profile per power source. `ioctl` get/set on `/dev/power`; the idle ladder and wake sources from T14b/T14d read it. List of sleep-blocker bits with the holding process for "who keeps it awake".
- `pm` command (shell): `pm`, `pm profile saver`, `pm set screen-off 120`, `pm who`, `-t` output.
- `power` GUI app in `flipper/gui/` (like `lights.c`), launcher entry "Power": battery screen, profile choice, each knob, "who keeps it awake", power off (with armed-alarm count), reboot.
- Persistence: the registers, plus a copy in the SD settings file (section L) restored at boot if the registers lost their magic. No internal-flash writes.
- Tests on the Flipper (agent): AC7.0; each wake-source switch really stops/starts that wake (e.g. pocket mode: Up does not wake, Back does); automatic power-off after a short test delay, then an alarm armed before it rings; `saver` vs `balanced` idle current recorded; L1 for every key in the app and from every sleep state; L6 for opening the app.
- RAM: kernel ≤ +16 B (if the registers are not enough); app ≈ 2 KB while open. Flash ≈ 6 KB.

## T16. Backlight gestures (spec R6.10; owner request 2026-11-16): small, any time after T14b; milestone M3, owner request so it may fill a gap earlier
Hold Left+Up 1.5 s: backlight forced off until held again; hold Right+Down 1.5 s: forced on until held again; the red LED blinks faster towards the threshold. Spec R6.10 has the details; the points that reviews check:
- Kernel only (`src/kernel/arm/flipdev.c`, `hal.h`, both boards): chord match at each button change in `btn_sample()`, the elapsed check at the 8 ms samples in `flipdev_btn()` (and the tick sampling on QEMU); the fire and the LED/vibro I2C in the task (`do_hardint`), never in the interrupt. Hold time counts from `k_input`.
- Flags in the free bits of `last_buttons` (bits 6-7, 26-28); the mode in BKP15R bits 14-15 (timeout field shrinks to 14 bits, clamp the `/dev/backlight` write; update the BKP15R line in PORTING.md); mode cleared at boot.
- `bl_want()`: forced on → 0; forced off → `BL_GONE`, `BL_SAVE` after `LCD_SAVE_S` on battery. `btn_sample()`: `SWALLOW` only for idle-off (and for the press that wakes a blank LCD), not for forced-off. `flipdev_idle()` agrees with `bl_want()` (no extra wakes in either mode).
- RAM table: 4 x 1 B entries + 1 B hold (50 ms units) + 3 B LED save = **≤ 8 B** (owner accepts ~10 B; raise `flipper/ram-budget.txt` by that, with the reason). `const` defaults copied at boot. `ioctl(BTNIOCGTAB)` (root; `EINVAL` for Back in a mask or < 2 buttons) and `ioctl(BTNIOCGEST, 0/1)` (foreground app only, cleared at `FGIOCSET`) in `<minix/fg.h>` or a new `<minix/btn.h>`. Action 3 (user gesture) is accepted in the table but does nothing yet (R7.2).
- `backlight force on|off|auto` and the mode in `backlight` with no arguments; the launcher passes `Gestures: no` from a `.desktop` entry. `/etc/rc`: if `/etc/flipper/gestures.conf` exists, a tiny helper (or `backlight gestures FILE`) loads it with `BTNIOCGTAB`; missing file: defaults, no message.
- No flash writes at run time (state in BKP15R and RAM only).
- QEMU test (in `m2test` or a kernel self-test under `BOARD=qemu`): set `qemu_fake_buttons` to Left+Up for 1.6 s → mode forced off in `/dev/backlight`; again → auto; Left+Up released at 1.0 s → unchanged; Left+Up+Back → unchanged; with `BTNIOCGEST 0` → unchanged. `M2 USER PASS` stays.
- Acceptance on the Flipper (agent): AC6.7; L1 unchanged with gestures on (100 presses, `lat`); the LED is back to its earlier colour after a cancelled and a fired gesture; idle current in forced-off equals idle-off (same Stop2 behaviour).
- Docs: CHANGELOG line, `docs/manual/buttons.md` (drop the "(planned)" marks), PORTING.md BKP15R.
- Later: the `gestures` app (R7.2, M4) edits the table from the SD file; no kernel change needed for it except action 3.

## T17. SD card safety: clean flag, `fsck` at boot on the screen, card pulled out (spec section N; owner request 2026-11-16): milestone M2
No journaling (owner decision). Spec section N has the details; the points that reviews check:

### T17a (`lane:cloud`, QEMU): FS flags, `fsck`, `/etc/rc`
- N.1: `s_pad` → `s_flags` (bit 0 CLEAN, bit 1 CHECK, the Linux `minix` / MINIX 3 values); in-core bits 14–15 of `s_pad`, masked out of disk writes; FS RAM **+0 B**. CHECK set at an unclean read-write mount; CLEAN = 0 written synchronously before the first other block of that device (hook in `cache.c` `rw_block`/`rw_scattered`). Read-modify-write of block 1 through the cache, never a stale in-core copy.
- N.2: CLEAN again in `do_sync` after a quiet sync period (no writes, nothing dirty, no open unlinked inode on disk); `SYNC` with a shutdown flag from MM's `do_reboot` marks every read-write FS clean; reads no longer dirty the inode for `ATIME` (`read.c`).
- N.3/N.4: `fsck` built and put in the base image; `-p`, `-v` (32-column lines, progress ≥ 1/s, reset cause in the header), BSD exit codes; spec maps only for `-i`/`-z`; streaming `chkmap`; link counts up to the highest used inode; zone windows with the per-inode window byte; `fsck_mem` from `/etc/rc.conf` (default 64 KB). `mkfs` and `tools/mkfsimg.py` write CLEAN.
- `/etc/rc` order of N.3 (root `fsck -p -v` first, exit 2 → hold the summary, `reboot`; `/usr` and `/home` before mounting; 4/8 → `mount -r` + notice). The Back skip may land as a stub that reads `/dev/buttons` non-blocking; the device check is T17b.
- The FS side of N.6 (`cache.c`: keep the buffer dirty on `ENXIO`, not "Unrecoverable write error"); QEMU can fake "card out" through the board's disk hook.
- INIT: `SIGPWR` → orderly halt (N.5).
- Tests: new `tools/sdcut.py` (kill QEMU mid-write and 90 s after the last write: ACN.1, ACN.2, ACN.4), `fsdamage.py` extended for ACN.3 with a sparse 2 GB image and peak memory; if util-linux `fsck.minix` is on the runner, cross-check. Add `sdcut` to the `powercut` CI job.
- No internal-flash writes at run time (flashcheck unchanged); `ramcheck` FS +0 B, INIT ≤ +16 B.
- Docs: CHANGELOG line (first boot after the update checks every file system once), `docs/manual/sdcard.md` drop the "(planned)" marks it covers, `man/man1/fsck.1` (`-p`, `-v`, exit codes).

### T17b (`lane:local`, Flipper): card detect and the device checks
- N.6: poll PC10 (EXTI 10 is the Up button) before every SD command and per tick while the card is selected or a buffer is dirty; `ENXIO` while out; CID + super block CRCs at boot (kernel ≤ +32 B); same card → resume, other card → never write; "SD card removed" / "Different card" on the screen with a short buzz; R12.25 swap waits for the same card.
- Device checks: ACN.5 (pull and reinsert during `cp`, then a different card), ACN.6 (unclean boot on the 3.8 GB card: LCD output readable, progress, ≤ 30 s, Back skips `/home`), ACN.7 (low-battery halt, no check at the next boot). Numbers into PORTING.md.

## T18. Less SD card wear from swap (spec 12g, R12.29–R12.33, R12.10a; owner request 2026-11-17): milestone M3
Owner rule: only measures with **0 B RAM** and no noticeable CPU on the foreground path (responsiveness first). Analysis: `reports/sd-swap-wear-analysis.md` (project files). Swap is off by default (`swap=plain`), so all of this changes nothing until swap is turned on, except R12.32/R12.33 at first-boot setup.

### T18a (`lane:cloud`, QEMU): MM, `mkpart`, setup
- R12.29 in `src/mm/swap.c`: chunk CRCs (48 × 4 B) in the slot header; swap-out reads the header, writes only changed 4 KB chunks, writes nothing if nothing changed; swap-in checks per chunk; the reserve job re-reads the header each step and writes it last. MM RAM **+0 B** (`ramcheck`), header stays in `mbuf`.
- R12.30: `SWAPHOT` flag (spare `mp_flags` bit) set on a swap-in by a wake, reserve waits ≥ 120 s for it, cleared after one such wait; on-demand swap ignores it.
- R12.31 MM side: CRCs first, then header, then chunks in ascending order.
- R12.32: `mkpart` (and the PC image tool, if it places partitions) aligns to 4 MiB.
- R12.33 userland: `erase /dev/hdN` (root, refuses a mounted partition, `man 8 erase`), called by `/etc/setup` stage 2 before `mkfs` with a line on the screen; until T18b lands the ioctl answers `ENOTTY` and setup goes on without it (message, no failure). QEMU board: `DIOCERASE` fills the range with the erased value so the test sees it.
- Tests: `tests/swapwear` for AC12.14 (count sectors written through the WINCHESTER task; QEMU disk hook), AC12.15 (30 s timer, 10 min, swap trace); `tools/swaptest.py` and AC12.13 still pass.
- Docs: CHANGELOG line, `docs/manual/` swap chapter (which card, `erase`, "a parked app that sleeps costs the card nothing"), 12f notes.

### T18b (`lane:local`, Flipper): disk driver
- R12.31: `disk.c` hands a multi-sector request to `hal_disk_write` as one CMD25 up to 4 KB (not 1 KB pieces); optional ACMD23. Measure AC12.16: swap-in of 64 KB (today 69 ms) and L1/L2 during a swap-out; trace that a 4 KB swap write is one CMD25.
- R12.33: `hal_disk_erase(first, last)` (CMD32/CMD33/CMD38, busy wait with the card's erase timeout, card-detect checks as N.6) and the root-only `DIOCERASE` ioctl on a partition device. AC12.17 on a card filled with data first.
- Kernel RAM +0 B. Numbers into PORTING.md.

## T19. Every Unleashed LF RFID protocol, decoders and encoders (spec 9.0, R9.0.1–R9.0.9; owner request 2026-11-17)
Owner request outside the milestone order (H-lane priority 4, "owner requests"). Today 1 of 25 protocols works (EM4100 RF/64); spec 9.0 has the gap table. Reuse the Unleashed code verbatim (GPLv3).

### T19a (`lane:cloud`, host + QEMU): the library and its tests
- R9.0.1 `src/lib/lfrfid/` with the 25 protocols, `fsk_demod`/`fsk_osc`, `t5577`/`em4305` block builders and the toolbox pieces; thin `furi` shim. Note the Unleashed commit in the file header.
- R9.0.2 `make lfrfidtest` (host) for all 25 protocols, encoder → decoder, jitter, both polarities, no cross-decodes; added to the `build` CI job.
- R9.0.3 user side that needs no new kernel call: `rfid` prints the protocol and rendered data from today's 512-edge capture (enough for the ASK protocols: EM4100/32, /16, Electra, FDX-B at 125 kHz, Viking, Jablotron, PAC/Stanley, Gallagher, Radio Key, GProxII, Noralsy); `rfid write PROTO HEX…` for every protocol (T5577; EM4305 where Unleashed has it); `rfid emulate PROTO` for those whose frame fits `RFIDIOCEMU` (≤ 512 segments); `.rfid` files for every protocol. The PR lists the device steps under "Needs on-device test".
- Measure the decoder heap (`protocol_dict_alloc` of all 25) and say it in the PR (budget ≤ 6 KB, freed at exit). No resident RAM, 0 run-time flash writes.
- Docs: CHANGELOG line, `docs/manual/` RFID chapter (protocol list), `man` page of `rfid`.

### T19b (`lane:local`, Flipper): the kernel and the device checks
- R9.0.4 `RFIDIOCCARRIER`: 62.5 kHz/25 % (PSK) and 134.2 kHz/50 % besides 125 kHz/50 %.
- R9.0.5 streaming capture (ring in the caller's pinned buffer, `poll`, `EOVERFLOW` on overrun).
- R9.0.6 long emulation (run list, ring refilled in the DMA half/complete interrupt, or a cheaper mechanism of the agent's choice with the same limits).
- Then switch `rfid` to ASK/PSK alternation and streaming, and long emulation for the FSK/PSK protocols.
- R9.0.7 Hitag µ / Hitag S write targets, R9.0.8 RAW read, R9.0.9 `lfrfid` app (protocol name, rendered data, "Add manually" for all 25), in that order, after the above.
- Device checks AC9.0.2–AC9.0.4 (results table in PORTING.md, `lat` numbers in the PR); kernel RAM ≤ +32 B in total.

## T20. Swappable memory blocks for apps (spec 12h, R12.34–R12.38; owner request 2026-11-17): milestone M4
Analysis: `reports/swappable-blocks-api-analysis.md` (project files). Owner rules: kernel 0 B, MM ≤ +39 B, no noticeable CPU on the foreground path, writes only for changed data, nothing at process exit. Needs plain swap (12a, `sw_on`, `sw_io`, `sw_step`) on main, which it is.

### T20a (`lane:cloud`, QEMU): MM, libraries, Berry
- R12.34/R12.35 in `src/mm/swap.c` (or a new `src/mm/blk.c` sharing `sw_io`): 256 extents after the 16 slots, reservation table (2 B per slot) with `blk_open`/`blk_close`, first fit, grow in place, shrink, half-of-space limit for non-root, header through the last `mbuf` sector, validity check, boot stamp (+4 B, bumped on pid wrap in `forkexit.c`). `make ramcheck`: MM ≤ +39 B, kernel +0 B.
- R12.36: `MEMINFO` request `MI_BLK` with the descriptor in the caller's memory; GET / PUT / partial PUT / `BLK_IFCHANGED` / INFO; buffer bounds checked against the caller's segments.
- R12.37: `BLKWAIT` deferral of background callers into `sw_step()` (order: user-awaited swap-in, then blocks, then reserve), shared write budget `sw.wkb`; `NOSWAP` → `EPERM` in `plain`; `ENXIO` without a kill on card loss.
- R12.38: `blk=` in `/etc/rc.conf` (default `off`) and `/etc/rc`; `<blk.h>` in libc; `libblk`; Berry module `blk` with `vbytes` (when the Berry VM is on main; otherwise a follow-up in the same task).
- R12.39 first (the layout needs it): `dv_base` in sectors in `drvlib.c`/`disk.c`, partition ioctls, `mkpart` placement past 4 GiB (each partition < 4 GiB), host `parttest` cases at 4/8/255 GiB, QEMU sparse 64 GB image. Kernel RAM +0 B (`make ramcheck`).
- `mkpart` + `/etc/setup` (R12.34): on cards of 8 GB up to below 32 GB swap = 50 % of the space after `/usr`, placed at the end of the card, at most 4 GiB − 1 MiB each for swap and `/home` (`82:50%@end`, MBR entry still hd3), `/home` before it; smaller cards unchanged; new installs only. QEMU tests with sparse 8 GB and 64 GB images. `growfs` is a later task, not this one.
- Cards ≥ 32 GB (R12.34): swap = `hd3` at the end with four 4 GiB swap subpartitions `hd3a`–`hd3d` (`mkpart` writes the subpartition table), MM uses `hd3a` for slots and spreads the block areas 4 per subpartition (+1 B). QEMU with a sparse 64 GB image; a missing `hd3c` only disables those 4 areas.
- R12.40 MM side: `BLKUSED` bit, 16-bit discard mask, idle discard in `sw_step()`, `blk_discard()`/`vm_drop()`; QEMU `DIOCDISCARD` zero-fills.
- Tests AC12.18–AC12.22 and AC12.24, AC12.26 in `tests/blk` and the M2 self-test; a test hook to force a pid wrap.
- Docs: CHANGELOG line, `man 3 blk`, `man 3 libblk`, manual chapter for app authors (12f), devguide note.

### T20b (`lane:local`, Flipper): measurements only
- AC12.25: a 64 GB or larger card, setup and use past 4 GiB.
- R12.40 HAL: read `DISCARD_SUPPORT` (ACMD13) at init, `hal_disk_discard()` (CMD32/33/38 arg 1, timeout, card-detect), `DIOCDISCARD`; AC12.26 on an SD 5.1 card.
- AC12.23: GET/PUT times (512 B, 1 KB, 4 KB) and L1/L2 with a background block reader and a foreground 1 MB map scroll; numbers in PORTING.md.

## T21. Launcher icons and Pine-style look (spec R5.6a, AC5.4–AC5.7; owner request 2026-11-17)
Owner request outside the milestone order: the launcher may show an icon of at most two characters (8×6 px) before each app. The rest of R5.6 (manifests, categories) stays in M4.

### T21a (`lane:local`, owner 2026-11-17: today, before other local work; host + QEMU + device), issue #229
- `/etc/apps` third field (R5.6a.1): empty, `@name`, `#` + 12 hex digits, or 1–2 characters; the old two-field form keeps working. Parsing in one pure function with a host test (`make icontest` or inside an existing host test target, in the `build` CI job): every form, malformed hex, 3 characters, `:` and `/` cases, label/user/command truncation unchanged.
- Built-in icon table (R5.6a.2), original 8×6 drawings, a host tool that renders them 8× enlarged into a PNG attached to the PR; base `/etc/apps` and base `postinst` lines with icons; `mkpkg.py -i ICON` writes `Icon:`.
- Drawing (R5.6a.3) with `gui_bits8()`; with icons off byte-identical frames (AC5.5).
- `gui_click()` in `gui.c` (R5.6a.4): release of a key pressed alone and held < ~1 s; page keys (Left, Right) use it.
- Setting (R5.6a.4): `/rom/etc/launcher.conf` with `Icons: on` and `Hints: on`, Left opens the settings page, write on Back only when changed.
- Pine-style look (R5.6a.7): title counter, 8 rows + scroll mark, hint line on every screen, notices on the message line for 2 s (timed read via `poll`, no periodic tick), settings page with `[on]` values. Enlarged PNG screenshots (from a QEMU frame dump) of the list and the settings page in the PR.
- QEMU test in the M2 self-test (or a launcher test driven with fake buttons, as the `libfuri` test does) for AC5.4–AC5.7; test labels may be Basque (`Kanpaia` with `@bell`, `Ordua` with `@clock`, `Kaixo` with `Zz`).
- RAM (R5.6a.5): give the launcher's `.bss` and the heap of 16 entries with icons before/after in the PR (≤ +8 B `.bss`, ≤ 104 B heap). 0 run-time flash writes (`flashcheck`), BASE_VER bump (new `/etc` file).
- Docs: CHANGELOG line; new `docs/manual/launcher.md` (the list, the hint line, icons and hints on/off, the `/etc/apps` line with the icon forms and the built-in names, a picture of the icons); devguide 4.4 (`/etc/apps` format, `Icon:` for packages, postinst example with an icon); `mkpkg.py` help.

## T22. Every app in the launcher's style (spec K.2 "Screen style", owner 2026-11-17)
The owner wants every app reached from the launcher to look like it: title bar with an optional state on the right, hint line, selection bar, `[value]` settings, scroll mark, notices on the message line. Starts after T21a, which brings the `gui.c` helpers.

### T22a (`lane:local`, after T21a), issue #230: plain `/dev/lcd` apps
- `gui.c` helpers of K.2 if T21a did not add them all: `gui_title2()`, `gui_hints()`, `gui_note()` (timed key read via `poll`), `gui_scroll()`.
- Align `info`, `lights`, `power`, `lfrfid`, the `console` menu (the text console itself stays an exception) and `selftest` (its test screens may stay full-screen; its menus follow). Each screen names exactly the buttons that act.
- QEMU frame dumps (enlarged PNG) of every changed screen in the PR, before and after. RAM: no app's `.bss`/heap grows by more than its new hint strings' pointers (strings `const`); give the numbers.
- Docs: CHANGELOG line; manual chapters of those apps updated with the new screens; devguide 4.x "Screen style" section with a small example (Basque strings welcome, e.g. hints `OK Hasi  Back Itzuli`).

### T22b (`lane:cloud`, QEMU): `libfuri` modules, after #201 merges
- Hint line drawn by Submenu, VariableItemList (`[value]`), Popup, DialogEx, TextInput from their own button labels; title bar state on the right; scroll mark in Submenu and VariableItemList. `GuiLayerWindow` height unchanged for custom views (stock `elements_button_*` stay).
- Relink and check `furihello`, `remind`, `alarm` (frame dumps). Shared-library flash and per-app RAM before/after in the PR (no growth per app beyond strings).
- Berry `gui` binding (R7c) and desktop `window` records (K.3) get the same parts when they are built; their tasks link to K.2.

## T26. Data modems between two Flippers: RFID, Sub-GHz, NFC, BLE (spec section P; owner request 2026-11-17)
Owner request outside the milestone order (like T19). Analysis and design: spec section P. The second Flipper of the two-device tests runs Unleashed, which has no such modem: MINIX-to-MINIX rows need a second MINIX Flipper for the session (`twoflip.py --peer-minix`, devguide part 2, 6.12).

### T26a (`lane:cloud`, host + QEMU): DONE in the PR that added section P
- `src/lib/modem` (frames, CRC, ARQ, carousel, RFID line code P4, CC1101 presets, band table, duty counter), `make modemtest` (ACP.1, ACP.2) in `tests/host`; `fmodem` with the RFID backend on today's `/dev/rfid` and `fmodem info`; `fmodem info` in the M2 self-test; `twoflip.py` `modem` rows and `--peer-minix`; manual chapter "Data modems", `fmodem(1)`.

### T26b (`lane:local`, Flipper): the device side
- RFID first, no kernel change: two MINIX Flippers back to back, `twoflip.py --peer-minix modem-rfid` (or 6.12 "By hand"); write the measured B/s of `fast` and `robust` and the best spacer into PORTING.md and the manual's table. If P4 fails on the real comparator, tune Q or the dwell in `fmodem.c` and the decoder tolerance in `p4.c` (host tests must stay green).
- RP.5c reverse channel: field-gap timestamps while `RFIDIOCEMU` runs (ring in the caller's pinned buffer, `poll`), kernel ≤ +8 B; then the RFID ARQ mode in `fmodem`.
- RP.8 + RP.7, with Phase 8 R8.1 part 2 / R8.2: `/dev/subghz` transmit in packet mode, `SGIOCPHY` computed by `src/lib/modem/cc1101.c` compiled into the kernel, the region band table (check every number of spec P.4 against the current ERC/REC 70-03, EN 300 220 and FCC 15.249 texts and the stock firmware's region data first, and say in the PR what was checked), the duty counter (7 slots, ≤ +24 B), CCA before transmit; kernel ≤ +32 B in total. Then the Sub-GHz PHY in `fmodem` (ARQ, W = 4) and ACP.R with `lat` during a transfer.
- Never enabled by default without the device checks; Sub-GHz transmit is RF, so this whole item is local-only (Autonomy rules).

### T26c NFC (M6, after Phase 13) and T26d BLE (M7, after Phase 15)
- P.6: ISO 14443-4 frames between an NFC-A initiator and a card-emulating target, S(WTX) answered by the driver. P.7: one L2CAP CoC, bonded and encrypted, `fmodem pair -m ble`.

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

## Lanes and milestones (owner, 2026-11-15 23:51 Warsaw): the strategy in practice
The order of work is `docs/spec/strategy.md`. **Requirements are closed** as of 2026-11-15: the spec's phases 0–22 are the scope. New ideas still go into the spec, but with a milestone tag (M1–M9), and only the current milestone's items become tasks.
- Every task of the current milestone has a GitHub issue with a lane label: `lane:cloud` (GitHub Actions worker, QEMU-testable) or `lane:local` (the local agent, needs the Flipper), plus the milestone label (`M1`, …). The tracking issue of the milestone lists them in order.
- **Current milestone: M1 Foundation** (Phases 2a, 2b, 2c, 2d; ABI 2; ≈ 0.5.0), tracking issue #105. Cloud: #99 2d (first), #100 R2a.5 (parallel), #101 2c and #102 R2b.9 (after #99). Local: #103 T14a, #104 T14b (R4b.2 ms timer), then the "Needs on-device test" checks of the cloud PRs.
- Local agent priority inside a milestone: (1) what the other lane waits for (device checks, timer API), (2) the milestone's `lane:local` issues, (3) responsiveness regressions, (4) owner requests, (5) polish. T-tasks not in the milestone wait for a gap.
- **Swap in two steps (owner, 2026-11-16):** plain swap (spec 12a, 12e, R12.28; `swap=plain`, off by default, CRC-32, `Secret: yes` never swapped) is a `lane:cloud` item of M3 and may start in M2 when a cloud slot is free, since it needs only M1. Park/hibernate and the Running list go with the launcher in M4. Encryption (12b) stays in M6: AES1 driver `lane:local`, GCM glue and QEMU software GCM `lane:cloud`.
- Directory ownership (to avoid conflicts): local owns `src/kernel/arm/`, `flipper/hal/`, the kernel device tasks, `flipper/m0/`, radio code; cloud owns `src/lib/`, `src/commands/`, `src/mm/`, `src/fs/`, `flipper/tools/`, `flipper/tests/`, pure-logic app cores, `docs/manual/`. A PR that crosses the line says so in its first line.

## Autonomy (owner, 2026-11-15 23:55 Warsaw): never wait for the local agent
The owner wants maximum autonomy. The cloud lane and the review side keep working when the local agent is away (no connection, owner asleep).
- **The local agent counts as away** when it has pushed or opened no PR for 2 h, or has not answered a review question for 2 h. Nobody needs to announce it.
- **While it is away the review side:** keeps up to **2** `lane:cloud` issues of the current milestone running (adds `claude-task` from the owner's account, in the issue order of the tracking issue); when the milestone has no cloud issue left, opens the next milestone's cloud issues early (strategy.md section 4: Phase 3, `poll()` core, `libfuri` R5.1–R5.2, `fam2pkg`, Berry VM, stock protocol decoders, manual chapters); reviews and squash-merges cloud PRs as usual.
- **Code part of `lane:local` work** may also go to the cloud worker when it can be written and checked in QEMU or on the host (e.g. a driver's protocol logic, a timer API with a QEMU backend). It merges only **off by default** or with no change on the Flipper path, and its PR lists the device steps under "Needs on-device test".
- **Never without the local agent:** enabling a new hardware path by default, option bytes, CPU2/FUS, anything whose acceptance is a `lat` number, a current reading or RF.
- **Device-check backlog:** every "Needs on-device test" of a merged PR is a checkbox in the issue labelled `device-checks`. When the local agent is back it does that list first (priority 1 above), then its `lane:local` issues.
- Cost guard: at most 2 cloud worker runs at a time; a cloud issue that fails twice goes back to the owner with what blocked it.
