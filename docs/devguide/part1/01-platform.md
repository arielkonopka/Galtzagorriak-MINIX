# 1. The platform as a programmer sees it

Read this chapter before writing anything bigger than a one-screen tool. Almost every surprise
on this system comes from one of three facts: there is no MMU, RAM is about 100 KB for
everything, and the device must answer a button press within a few tens of milliseconds whatever
else runs.

## 1.1 The machine

| | |
|---|---|
| CPU | STM32WB55, Cortex-M4 at 64 MHz, Thumb-2 only. FPU present but not used by programs yet: code is built with `-mfloat-abi=soft` (the opt-in FPU is spec R5.7, **(planned)**). |
| RAM | 192 KB SRAM1 plus two 10 KB pieces of SRAM2. Kernel, MM, FS and INIT take their share; about **105 KB is free in one piece after boot**, shared by every user process. |
| Flash | 1 MB, of which MINIX uses the lower part: the system image (at most 192 KB), the shared C library (one copy for every program), the *installed region* with program text and the default settings, and the *run-time text cache*. The top belongs to the radio CPU and is never touched. |
| Processes | `NR_PROCS` is 16: MM, FS and INIT take three, so at most **13 user processes** exist at once, shells and the launcher included. |
| Clock | `HZ` is 60: the scheduler, `alarm()` and `sleep()` count in ticks of 16.7 ms. A millisecond timer exists in the kernel (R4b.2) but not yet for programs (`setitimer` subset, **(planned)**). |
| Screen and buttons | 128x64 monochrome LCD, six buttons (Up, Down, Left, Right, OK, Back). |
| Storage | The SD card holds the whole file system: `/` (32 MB), `/usr`, swap (reserved), `/home`. |

## 1.2 A process without an MMU

The Cortex-M4 has an MPU, which protects memory but does not translate addresses. So:

- **Every process lives at real addresses.** MM gives it one contiguous block of RAM for data,
  bss, heap and stack. Two processes never see the same address for different things, and a
  pointer is a physical address.
- **Isolation is still real.** The MPU lets a process touch only its own block (and read the
  flash). A stray pointer gives that process a signal 11, not a corrupted kernel. This holds
  for every user process; there is no "run without protection" mode (spec invariant I3).
- **The block cannot move or grow in place freely.** The heap grows up and the stack down
  inside a fixed reserve set when the program is built (`chmem`, see 1.4). When that reserve is
  used up, `malloc` returns NULL; when the stack runs into the heap, the program faults.
- **The largest hole decides what can start**, not the total. `free` prints both; `free -v`
  lists the holes. `exec` places programs at the top of RAM and fork shadows at the bottom, so
  long-lived processes pile up at the top and the free space stays in one piece.

### fork and vfork

MINIX keeps the old *shadowing* scheme from its Atari ST port: after `fork()` the child runs at
the parent's addresses, and the kernel swaps the two data images in and out of that place on
every switch between them. That works, but it costs a second copy of the parent's data in RAM
and a copy on each switch.

- To start another program, use **`vfork()` + `exec`**. The child borrows the parent's memory
  until it calls `exec` or `_exit`, so nothing is copied. Do only `setuid`/`setgid`, `chdir`,
  `dup2`, `close` and `exec` between `vfork` and `exec`; anything that changes the parent's
  variables or stdio state is a bug. The launcher is the model (`flipper/gui/launcher.c`).
- Use `fork()` when the child must really run the parent's code on its own. Keep the parent's
  data small then, because the shadow is a copy of it.
- `system()` and `popen()` start `/bin/sh`, which costs about 12 KB of RAM while it runs. In a
  small tool, `vfork` + `execv` of the program itself is much cheaper.

### Threads

There are none, and none are planned in the kernel. A second activity is a second process (with
pipes or signals between the two), or an event loop in one process. Cooperative threads inside
one process come with `libfuri` (Phase 5) for ported stock apps **(planned)**.

## 1.3 Program text runs from flash

Programs are built position-independent (`-fpic -msingle-pic-base`, register `r9` points at the
data). That lets one copy of the text serve every process that runs the program, and lets that
copy live in flash:

- **Installed programs**: `pkg install --flash` or `pkg flash` writes a program's text into the
  installed region once, at install time. Every later `exec` runs it from flash and needs RAM
  only for data, bss and the stack/heap reserve. The programs used at every boot (`sh`, the
  launcher, `getty`, `login`, `ls`, `cat`, ...) are in flash from the firmware image itself
  (`make base-flash`).
- **Other programs** load their text into RAM at `exec`. After a program has run three times
  from RAM, MM copies its text into the run-time cache as background work, within a daily erase
  budget (`pkg budget`, `free -v`).

So the RAM a running program costs is mostly **data + bss + chmem**, not its size on disk. Code
is cheap; static tables and buffers are not. Make tables `const` (they then stay in flash),
keep buffers small, and prefer the stack for short-lived work.

## 1.4 `chmem`: the stack and heap reserve

Each program file carries the size of its stack + heap reserve, like MINIX's `chmem`. The build
sets it: 6 KB by default (`tools/elf2mnx.py`), and a `CHMEM_<program>` line in
`flipper/minix.mk` overrides it (4 KB for small tools, 12 KB for `sh` and `ls`).

Choose it by measuring, not guessing:

```
free -t on        # root: MM reports unused reserve at every exit
myprog ...        # run the program through its worst case
free -t off
```

MM fills the reserve with a pattern at `exec` and, at exit, prints how much of it was never
touched. Keep about 2 KB untouched as a margin; give back the rest. Every KB of `chmem` is a KB
that another app cannot have.

What eats the reserve, from experience in this tree:
- `opendir()` mallocs about 1 KB per open directory (recursive tools need several);
- `stdio` buffers (`BUFSIZ` is 512 here) for every open `FILE`;
- `exec` from a shell builds the argument block on the heap (a big glob needs KBs);
- large local arrays. There are no stack canaries: an overflow can reach a return address, so
  bound every copy into a stack buffer.

## 1.5 The C dialect and library

- **Compiler**: `arm-none-eabi-gcc -Os`, as **gnu89** with `-fsigned-char` (the 1996 sources
  expect K&R C and ACK's signed chars). New code uses ANSI prototypes, but keep to what gnu89
  accepts: no declarations inside `for (...)`, and there is no `<stdint.h>` or `<stdbool.h>`.
  Fixed-size types are `u8_t`, `u16_t`, `u32_t` from `<sys/types.h>` (`long` and `int` are 32
  bits). Host-tested cores (chapter 2) are written so they compile in both worlds.
- **libc** is MINIX 2.0's, extended for the port. It is one shared copy in flash: programs
  call it in place, and each process keeps only its data (about 0.6 KB). Notable points:
  - POSIX.1 (1990) calls: files, directories, `fork`/`vfork`/`exec`, signals with
    `sigaction`, `termios`, `pipe`, `mkfifo`, `alarm`, `pause`, `sigsuspend`;
  - **not present**: `select`/`poll` (`poll()` is spec R4b.6, **(planned)**), `usleep`,
    `nanosleep`, `setitimer`, `mmap`, sockets, threads, `setgroups` (R4b.4, **(planned)**);
  - `memcpy`/`memmove`/`memset` are fast assembly versions;
  - `cfopen(path, mode)` reads a settings file from the card and falls back to the default in
    `/rom` (chapter 5);
  - `printf` handles floating point, in software. Floating point is slow and makes code bigger;
    integer arithmetic (millivolts, tenths of a degree) is the habit here.
- **Headers**: `<minix/...>` for system-specific things such as `<minix/fg.h>` (foreground
  app), and the port's own `flipper/gui/gui.h` for screen apps.

## 1.6 Users, groups and permissions

Unix permissions are the security model, including for hardware:

- Apps started from the launcher run as `nobody` in group **`gui`**, which may use the screen,
  buttons, LED, vibro, speaker, GPIO, infrared and iButton (`/dev/*` with mode 660, group
  `gui`). An entry marked `root` in `/etc/apps` keeps root's rights.
- Some devices are readable by everyone (`/dev/adc`, `/dev/power`).
- Each package app gets its own uid and supplementary groups from its manifest (spec A.3,
  R4b.4) **(planned)**; until then, choose the user in `/etc/apps`.
- Set-uid programs must take their code from their own file (invariant I4); the flash text of an
  installed program is checked against the file before it is trusted.

## 1.7 Signals and the console

- Signals are POSIX: `SIGTERM` from the long Back press (chapter 4), `SIGINT` from the
  terminal's interrupt key, which in MINIX is **DEL**, not Ctrl-C.
- The console is the USB serial port (`/dev/ttyACM0` on a Linux PC) when a cable is attached, or
  the on-screen console app. Kernel messages go there too.
- `/dev/log` takes messages from daemons; `/etc/rc` output goes there.

## 1.8 Responsiveness is a requirement, not a wish

The device must feel at least as quick as the stock firmware. The spec turns that into budgets
(rule 0.8). The ones a program author affects:

| | Budget | What it means for your code |
|---|---|---|
| L1 | button to changed pixel ≤ 30 ms (p99) | react to the press, not the release; draw first, compute later |
| L6 | launcher to the app's first frame ≤ 300 ms | open devices and draw before reading big files |
| L7 | app exit to the menu ≤ 150 ms | exit promptly on Back; do not linger in cleanup |
| L9 | a flash write may freeze the UI ≤ 25 ms | never write internal flash from an app; files go to the SD card |

Background programs must not steal from the foreground: no busy loops, no polling faster than
needed, block in `read()` or `pause()` instead. A CPU-bound job should run rarely and briefly or
lower its priority (`nice` is not wired to anything useful yet; the interactive queue of R0.8a is
**(planned)**).

## 1.9 Flash wear

Internal flash survives about 10,000 erases per page, and an erase stalls the CPU for about
22 ms. The owner's rule is: **write internal flash as rarely as possible, and never at run time
if it can be avoided**. For programs this means:

- settings, logs, data and anything else that changes go to **files on the SD card**; nothing a
  program writes at run time may land in internal flash (spec L.1);
- install a program into flash once (`pkg install --flash`) rather than letting the run-time
  cache copy it;
- do not reinstall the same version in a loop during development: run test builds from RAM
  (`pkg install` without `--flash`, or run the file directly).
