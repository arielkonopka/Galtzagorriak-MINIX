# Next tasks (owner approval 2026-11-14 20:29Z)

Do these before new features: 1–3 in order, 4–5 after them or alongside.
Each task goes on its own branch, branched from **main** (not stacked), and gets a PR naming the task number.
The usual gates apply: `ramcheck` (0 B unless stated), QEMU `M2 USER PASS`, and `lat` numbers before/after in PORTING.md.

## Standing rule (owner, 2026-11-14 20:41Z): few flash writes at run time
When optimizing RAM or flash, keep **run-time** writes to internal flash to a minimum. Writes at build or install time (`make base-flash`, `pkg install --flash`, Phase 2b installed region) are allowed, but should **also be as rare as possible**: only write pages whose contents changed (compare before erase), append instead of rewriting, and don't compact or reformat unless space runs out. Every PR that writes flash at run time must say how often it writes in normal use and why that write can't happen at build or install time. Reviews check this.

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

## Still open from reviews
- IR (#46, merged 2026-11-15 by owner decision; supersedes #6/#7): **win back the kernel +64 B** (budget 15168 → 15104). Also `lat` while a frame goes out, a test against a real TV/remote, and the flipdev stack high-water after a long frame (`ir_write` keeps 64 B on it).
- mined (#45): one allocation per line so `CHMEM_mined` fits 12 KB; `EDITOR`/`VISUAL` for existing cards via the base postinst.
- Phase 4a items from the #2 review: IWDG kicked from the clock task with ~2 s timeout, watchdog→DFU only in dev builds, reset cause kept. HSEM around the RNG before CPU2/BLE.
- T2 (#24, merged by owner decision 2026-11-14: responsiveness first): **win back the MM +68 B** (`runs[]` + `job`). Options: 4-byte `runs[]` entries (28-bit key from crc^owner + 4-bit count), one shared hex table, trim `hole[]`. Also **check the flash copy before `J_RECORD`** (SHA-256 as in finst.c, or the text CRC), so a file rewritten during the copy never lands in the cache under the old key.
- Phase 2b (#37/#38): AC2b.R1 (L6 from the installed region), AC2b.R2 (`lat` during a 50 KB install), AC2b.5 (power-cut test), MM stack high-water after bind + compaction (estimated 500–600 of 1024 B), first-boot bind time in PORTING.md; old cards need `pkg bind /bin/*` in `/etc/rc` (have pkg/update add it, or document it).
- T3 (#27): L1 p99/max still ~0.75–0.92 s; long-press and repeat events (stock Press/Short/Long/Repeat) when R4b.8 lands.
