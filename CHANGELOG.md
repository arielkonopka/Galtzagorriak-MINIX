# Changelog

Galtzagorriak, a MINIX 2.0.0 port for the Flipper Zero. Versioning rules: spec section J
(`docs/spec/galtzagorriak-spec.md`). Newest release first. Feature PRs add one line under
`Unreleased`; the release PR turns it into a version section.

## Unreleased

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
