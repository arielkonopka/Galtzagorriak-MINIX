# Review of PRs #1–#6 (stacked), 2026-11-14

Reviewed at heads: #1 4e9c835, #2 a7f4d84, #3 0ec7053, #4 1ae4383, #5 780b8fd, #6 4095d0d.
Criteria: correctness, security, RAM (owner: never increase RAM), responsiveness (spec rule 0.8, L1–L10), spec conformance.
RAM was measured by building main (81506e0) and #6 with `make BOARD=flipper minix`. Kernel data+bss went from 17 556 B to 18 960 B (+1 404 B). Of that, 1 024 B is `ir_buf` (#6) and 256 B is the note table (#5). MM, FS and INIT are unchanged.

| PR | Verdict |
|---|---|
| #1 Flash cache after DFU, tty, cut | **Pass** |
| #2 reboot -d, watchdog, bell | **Pass**, with follow-ups |
| #3 memops, SD FIFO/CRC/CMD18 | **Changes requested**: owner chose the exec-side fix over CHMEM_sh +4 KB |
| #4 PendSV shadow kick, /dev/random, /dev/zero, CMD25 | **Pass**, with follow-ups |
| #5 /dev/gpio, /dev/vibro, /dev/speaker | **Pass**, with a small note |
| #6 /dev/ir | **Changes requested** (blocking) |

## #1: pass
- The DFU reset runs at most once (the SFTRSTF guard). It runs before .data/.bss, so it costs no RAM.
- The tty fix is O(1) on the input path.
- `cut` splits attached arguments correctly.
- Non-blocking: RMVF at every boot throws away the reset cause. Store it, for example in an RTC backup register, so that IWDG and brown-out resets can be reported.

## #2: pass, with follow-ups (required by Phase 4a)
- **R4a.4 / AC4a.4:** the spec asks for a timeout of about 2 s, refreshed from the clock *task*. This PR uses 10 s, refreshed from the SysTick *ISR*. A kernel task stuck in `for(;;)` with interrupts on keeps the ISR kicking, so the watchdog never fires and AC4a.4 fails. Move `hal_wdt_kick()` into `clock_task`/`do_clocktick` and shorten the timeout when Phase 4a lands.
- **IWDG reset → DFU:** this is good for development. On a user's device, though, a watchdog reset leaves it sitting in DFU with a blank screen, which looks bricked. Stock firmware reboots normally. Gate "watchdog → DFU" behind a dev flag (for example a build option or a backup-register bit set by `reboot -d`), and boot normally otherwise.
- `hal_halt` behaviour (kick on halt, stop on panic) matches R4a.4. OK.
- The bell shares TIM16 with `/dev/speaker` (#5). A `^G` during a tune cuts the tune's current note after `HZ/6`. This is minor.

## #3: changes requested (owner decision 17:10Z: no RAM increase)
- memcpy/memmove/memset look correct: alignment, overlap direction, and LDM/STM register lists. They are interruptible on the M4 (ICI), so there is no L3/L4 impact.
- The SD retry and CRC logic is sound. Resuming at the failed sector is correct for CMD18. No interrupts are masked in the SD path.
- **RAM:** `CHMEM_sh` goes from 12 KB to 16 KB, which is **+4 KB per running shell** (login shell, subshells, scripts). It fixes a real E2BIG, but it breaks the "never increase RAM" rule. A better fix is to stop the exec frame needing heap room after a glob. Options: build it on the stack, free the glob vector before `execve`, or let MM read argv directly. Alternatively, keep 12 KB and grow the reserve only on E2BIG.
- **Owner decision:** keep `CHMEM_sh` at 12 KB and fix this on the exec side. Everything else in #3 passes. Because #4 and #5 are stacked on #3, they merge once #3 is fixed.

## #4: pass, with follow-ups
- Idle masks interrupts only for a handful of instructions around `wfi`, which is fine for L4. The PendSV kick does not count as a tick. The tick-coalescing behaviour is unchanged.
- `flipwords` is correct for 16-byte multiples (whole clicks).
- `/dev/zero` writes to `user_phys`, which `numap` validated, and `count` is clipped to `io_nbytes`. OK. `/dev/random` is 0444.
- **RNG and CPU2:** the comment says "CPU2 is not started, so no semaphore", but the spec reuses the owner's working CPU2 BLE stack, which shares the RNG. Take HSEM semaphore 0 around RNG use (ST AN5289) before BLE comes up.
- **Stack:** `u32_t w[16]` (64 B) is added to the MEM task stack (256 B, measured 120). Re-check `kstack_used` on the device.

## #5: pass
- GPIO is limited to the eight header pins (R6.4: internal pins unreachable). OK.
- Note timing is driven by the tick. Nothing busy-waits.
- `u8_t nb[256]` sits on the PRINTER task stack (768 B, measured 176). That fits. Decoding notes straight into `note_hz/note_ticks` would save it.
- The note table adds +256 B of static RAM. That is acceptable for the feature.

## #6: changes requested
1. **Kernel stack overflow (memory corruption).** `ir_send()` declares `u32_t d[IR_MAX]`, which is **1 024 B on the PRINTER task stack, and that stack is only 768 B** (`table.c`). The frame extends past the stack bottom into neighbouring kernel memory, and `phys_copy` writes into exactly that far end. It works on the device by luck. Do not put the buffer on the stack. Better still, follow fix 2.
2. **Freezes the whole system during IR send (rule 0.8).** `ir_send()` busy-waits in the flipdev task for the whole frame. An NEC frame takes about 70 ms, and an `ir tx raw` with 256 durations of up to 1 s each can take **minutes**. Kernel tasks outrank every process, so UI, buttons, the shell and the USB console all stop (L1 30 ms and L2 2 ms are broken). Any member of group `gui` can do this to the device. Per R6.6, transmit on the pulse engine instead: a timer update/CC interrupt or DMA into the timer's ARR/CCMR that switches the carrier. Then suspend the writer and REVIVE it when the frame is done, the same way as `/dev/speaker`.
3. **RAM.** `ir_buf` adds **1 KB of permanent kernel bss**. The spec budget for the pulse engine is about 64 B. Receive into the reader's buffer (numap'd while a reader is suspended), or into a small ring that drains to the reader. Do not keep a 1 KB static frame.
4. Minor: frames that arrive while `ir_done` is set are dropped silently. With a reader that is already suspended, the receive path could deliver directly instead.
