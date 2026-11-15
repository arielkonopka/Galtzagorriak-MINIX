# Changelog

Galtzagorriak, a MINIX 2.0.0 port for the Flipper Zero. Versioning rules: spec section J
(`docs/spec/galtzagorriak-spec.md`). Newest release first. Feature PRs add one line under
`Unreleased`; the release PR turns it into a version section.

## Unreleased

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
