# 3. Writing a driver

Today every driver is part of the kernel. The Flipper's own hardware is served by one task,
`flipdev_task` in `src/kernel/arm/flipdev.c`, as minors of major 6. A new piece of hardware
normally becomes **a new minor of that task**, not a new task: each task costs a stack in RAM
(the flipdev task's is 768 bytes), a process slot and a table entry, while another minor costs a
few bytes of state and its code in flash.

User-space drivers that start when their device is opened and exit when idle are designed but
**(planned)** (spec Phase 4b, `udrv`/`devd`; 4.8 below).

The best way to learn the pattern is to read one complete, recent driver alongside this chapter:
`/dev/adc` (R6.4, on-demand measurement), `/dev/ibutton` (R6.7, timing-critical bit-banging),
`/dev/vibro` (R6.3, the ms timer) or `/dev/ir` (R6.6, interrupt-driven rings). Each came as one
PR with its HAL functions, `/dev` node, manual section and CHANGELOG line.

## 3.1 The pieces of a driver

| Piece | Where |
|---|---|
| the hardware access (registers, pins, timing) | `flipper/hal/flipper/board.c`, declared in `flipper/hal/hal.h` |
| the same functions for the emulator (stubs or a simulation) | `flipper/hal/qemu/board.c` |
| the device semantics (what read and write mean, permissions, blocking) | a minor in `src/kernel/arm/flipdev.c` |
| the `/dev` node on new cards | `flipper/rootfs/proto` |
| the `/dev` node on existing cards | `flipper/rootfs/base.postinst` (and raise `BASE_VER`) |
| a command or app that uses it | `flipper/cmds/` or `flipper/gui/` |
| the description | the header comment of `flipdev.c`, `docs/manual/devices.md`, chapter 3 of Part 1, `CHANGELOG.md` |

The HAL keeps the kernel free of register addresses, and the QEMU board keeps CI building and
testing every path. A HAL function that the emulator cannot do returns a "not present" value
(`-1`, or an IRQ number of -1), and the driver turns that into `ENXIO` or `ENODEV`.

## 3.2 Adding a minor

1. Pick the next minor number, add `#define FOO_MINOR` and raise `LAST_MINOR`
   (`flipdev.c`). Document it in the header comment: what a read and a write do, sizes,
   blocking, errors.
2. Handle it in `do_rw()`. The caller's buffer is already mapped: `user` is a physical address
   that passed `numap()` (for reads into the user) or `numap_src()` (for writes from it, which may
   be constant data in flash). Copy with `phys_copy()`; **never** dereference a user pointer
   (security invariant I1). Return the byte count, an error, or `SUSPEND` (3.4).
3. Check the operation: return `EINVAL` for a write to a read-only device, too-short buffers and
   bad values. Validate every byte that comes from the user before it reaches a register.
4. Add the node to `rootfs/proto` with the right owner, group and mode: `c--660 0 10 6 N` for
   devices launcher apps may use (group `gui`), `c--444 0 0 6 N` for read-only status anyone may
   read, `c--600` for root only. Choose the narrowest that works.
5. Add the same node in `base.postinst` for cards made before your change (look at the
   `/dev/adc` block), and raise `BASE_VER` in `minix.mk`.

## 3.3 Interrupts

- Register a handler with `put_irq_handler(irq, handler)` and `enable_irq(irq)` once, at the
  start of the task. The handler runs in interrupt context: `int handler(int irq)`, returning 1.
- **Do the minimum there**: read the hardware, put a value in a small ring, and wake the task
  with `interrupt(PRINTER)` when the task has work (as `ir_int()` does at half a ring). The task
  then gets a `HARD_INT` message and does the rest in `do_hardint()`.
- `cause_sig()`, replies and copies to users are task work, never handler work.
- **Interrupt slots are full**: `NR_IRQ_HANDLERS` is 8 and all eight are used (clock, UART,
  USB, buttons, IR receive, IR transmit, ms timer, SD DMA). A new handler means raising it
  (+4 bytes) with a reason, or sharing a handler between lines as the buttons do.
- Masking interrupts (`lock()`/`unlock()`) is allowed for at most **20 µs** (budget L4). The
  one measured exception is the iButton bit slot (≤ 70 µs). Anything longer must be restructured:
  a timer interrupt per step, or DMA.

## 3.4 Blocking: SUSPEND and REVIVE

A read that has to wait (a button, an IR frame) returns `SUSPEND`. FS then leaves the caller
blocked and the task must remember who it was (`rd_caller`, `rd_proc`, the address and count).
When the data arrives, usually in `do_hardint()`, the task copies it and sends
`reply(REVIVE, caller, proc, count)`.

- Support `O_NONBLOCK` (`m_ptr->TTY_FLAGS & O_NONBLOCK`): return `EAGAIN` instead of
  suspending.
- Handle `CANCEL` (the caller got a signal): forget the waiter and stop anything you started for
  it (the speaker and IR show how). The reply is `EINTR`.
- Decide what a second waiter gets. One reader at a time with `EIO` for the second, or `EBUSY`
  for exclusive hardware, are the patterns used so far.
- A revived process goes to the head of the user queue (R4b.3); budget L5 (REVIVE to running)
  is 100 µs.

## 3.5 Time without busy waiting

- **The ms timer** (R4b.2, `mt_arm()` in `arm/klib.c`): a kernel client asks for a call of its
  function in 1–999 ms. It runs on LPTIM1, which keeps counting in Stop2, so it also ends things
  while the CPU sleeps (the timed vibration does exactly this). Add a client: a new `MT_*`
  number in `src/kernel/const.h`, raise `MT_N`, add the function to `mt_func[]`. Call `mt_arm()`
  with interrupts off. The function runs in interrupt context.
- **The clock tick** (60 Hz): `flipdev_tick()` is called from the clock interrupt for things
  timed in ticks (notes, the backlight, the end of an IR frame).
- **Busy waits** are only acceptable for microsecond protocol timing that no peripheral can do,
  bounded and measured (the 1-Wire slots). A wait of milliseconds in a task blocks every other
  kernel task and every user process: use the ms timer, or yield (`hal_disk_yield()` in the SD
  driver shows how a long poll gives the CPU back).

## 3.6 Power: sleep blockers and on-demand hardware

The idle task puts the CPU into Stop2 when nothing needs the fast clocks (T14b). A driver that
needs them (running DMA, a peripheral clocked from the PLL) sets its bit in `pm_busy` (`PM_*`
in `const.h`) while it works and clears it after, as the SD card does for a DMA transfer. Some
devices are asked directly instead (`flipdev_idle()`). Forgetting to clear the bit keeps the
whole device awake; forgetting to set it lets the CPU stop mid-transfer.

Power hardware up for the operation and down after it when that is fast enough (the ADC does
it per read). Budget the average current: the floor of the whole device is about 0.5 mA, and
every always-on peripheral eats into it ([power plan](../../spec/power-plan.md)).

## 3.7 RAM in drivers

The rules of the RAM chapter apply with extra force in the kernel, whose static RAM is counted by
`make ramcheck` against `flipper/ram-budget.txt`:

- tables `const` (flash);
- **share buffers between devices that cannot run at once**: the speaker's notes and the IR rings
  are one union (`sbuf`), and one gets `EBUSY` while the other is in use;
- keep state in bits of existing words where it is natural (the foreground slot lives in
  `last_buttons`);
- prefer hardware that holds data (FIFOs, the LED chip's program engines, RTC backup registers)
  over RAM copies (spec rule 0.7);
- stack use counts too: check `free -k` after your change, and keep large locals out of the
  task (the 1-Wire and ADC code read into small locals and copy out).

If the budget must grow, raise the image's line in `ram-budget.txt` in the same commit with a
comment that says why, and expect the review to ask for something given back.

## 3.8 User-space drivers **(planned, spec Phase 4b)**

The design, from `PORTING.md` and spec 4b, for drivers that are not needed all the time
(Bluetooth, Sub-GHz, NFC/RFID, extra USB classes):

- a kernel task `udrv` forwards requests for `/dev/X` to the driver process for X, answers the
  caller with `SUSPEND` and revives it when the driver answers, so the kernel never waits on a
  user process;
- `devd` (root) starts the driver from `/etc/drivers` with its own uid when its device is opened,
  and the driver exits after N seconds idle, so its RAM is free the rest of the time;
- interrupts reach the driver as `HARD_INT` messages, with the line masked until it acknowledges;
  its registers are reachable through an extra MPU window for that driver only; DMA only into its
  own memory (R4b.5);
- when a driver dies, `udrv` fails its waiting requests with `EIO` and releases its interrupt and
  window; the next open starts it again.

Until that exists, keep radio and protocol logic that can live in user space in user space
(protocol cores like `cfcore.c`, `sbproto.c`), with only the timing-critical part in the kernel.
Continuous streams that must not stop for scheduling (tier 3 of the background model) stay in
the kernel by design.
