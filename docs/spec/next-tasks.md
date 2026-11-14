# Next tasks (owner approval 2026-11-14 20:29Z)

Do these before new features: 1–3 in order, 4–5 after them or alongside.
Each task goes on its own branch, branched from **main** (not stacked), and gets a PR naming the task number.
The usual gates apply: `ramcheck` (0 B unless stated), QEMU `M2 USER PASS`, and `lat` numbers before/after in PORTING.md.

## Standing rule (owner, 2026-11-14 20:41Z): few flash writes at run time
When optimizing RAM or flash, keep **run-time** writes to internal flash to a minimum. Writes at build or install time (`make base-flash`, `pkg install --flash`, Phase 2b installed region) are fine. Every PR that writes flash at run time must say how often it writes in normal use and why that write can't happen at build or install time. Reviews check this.

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

## Still open from reviews
- #6/#7 (IR): TX on the pulse engine (no busy-wait in a kernel task), and no 1–2 KB static `ir_buf`. Rebase on main first; both PRs conflict.
- Phase 4a items from the #2 review: IWDG kicked from the clock task with ~2 s timeout, watchdog→DFU only in dev builds, reset cause kept. HSEM around the RNG before CPU2/BLE.
