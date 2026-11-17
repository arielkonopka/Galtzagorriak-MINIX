# 2. Building, testing and CI

## 2.1 Toolchain

The same as for building the system in the
[manual](../../manual/install.md#1-what-you-need): `gcc-arm-none-eabi`, `make`,
`python3` (with `python3-pil` and `xfonts-base`, needed when the boot screen is redrawn),
`dfu-util` for flashing, `qemu-system-arm` for the emulator, and a serial terminal. Everything is
driven from `flipper/` with `make`; the host tools are Python scripts in `flipper/tools/`, run
with `python3 -I`.

## 2.2 Make targets

Run in `flipper/`. `BOARD=qemu` is the default; `BOARD=flipper` builds for the device. Output
goes to `build/<board>/minix/`.

| Target | Result |
|---|---|
| `minix` | `minix.bin`: kernel + MM + FS + INIT (fails above 192 KB), then the shared libc at 192 KB |
| `shlibc` | only the shared libc: `shlibc/libc.bin`, its stubs and `libc.json` for `elf2mnx.py` |
| `base-flash` | `minix-base.bin`: `minix.bin` plus the boot programs in the installed region and the ROM settings image. This is what users flash. |
| `base` | `pkgs/base_<BASE_VER>.tar`, the system programs as a package |
| `pkgs` | the other packages (`hello`, `tests`) |
| `card` | `card.img`: MBR + root partition for the SD card |
| `disk` | `fs.img` and the QEMU `disk.img` |
| `minix-run` | boot `minix.bin` in QEMU with `disk.img` (BOARD=qemu, 60 s limit) |
| `minix-flash` | `dfu-util` the plain `minix.bin` (BOARD=flipper) |
| `ramcheck` | static RAM of each image against `ram-budget.txt`, plus `versioncheck` and `bsscheck` (no base program above 4 KB `.bss` except those in `bss-exceptions.txt`) |
| `versioncheck` | version numbers in `gzversion.h` agree with `CHANGELOG.md` |
| `latcheck` | the same system with the latency trace compiled in (`build/<board>-lat/`) |
| `INIT=m2 minix` | a system whose INIT runs the self-test and exits QEMU |

Typical loops:

```
make -j minix disk && make minix-run                    # QEMU, interactive
make -j INIT=m2 minix disk && make INIT=m2 minix-run    # QEMU self-test
make BOARD=flipper -j minix base-flash base ramcheck    # device build + RAM gate
```

`make` (no target) and `make run` build the milestone-0 bring-up program (`flipper/m0/`), which
is only useful for HAL work on a new board.

## 2.3 The self-test (M2)

`flipper/m2/m2test.c`, started by the test INIT (`flipper/m2/init.c`). It exercises file I/O,
argv and environment, `malloc`, signals, `fork`/`vfork`, pipes, `exec`, MPU isolation, the
memory routines at every alignment, the foreground ioctl and more, and ends with
`M2 USER PASS` or a failure line, plus a `M2 FLASH erased` line used by the flash check.

Every change must keep it passing (spec rule 0.4). Extend it when you add a kernel or server
feature that QEMU can exercise. Mind its size: a bigger `m2test` may cross a 4 KB page and change
the flash-erase count (`flash-budget.txt` explains the one time that happened).

## 2.4 Security tests

`flipper/tests/`, built into the `tests` package (installed to `/usr/tests`). Each test tries an
attack in a child process and prints `PASS` when the system refuses it and keeps running
(`tests/t.h` has the helpers). A security fix comes with such a test (rule 0.4). Run them on QEMU
or the device:

```
pkg install /lib/pkg/tests_1.0.tar
for t in /usr/tests/*; do $t; done
```

## 2.5 Power cuts and flash erases

- `tools/powercut.py` boots QEMU repeatedly and cuts the "power" at chosen flash operations
  during program installs (before an erase or write, or half way through), then checks that
  nothing runs a damaged text and installs still work. Default: every erase, the ends of every
  text and every directory write; `--all` cuts at every operation; `-j N` runs in parallel.
  Needs `make BOARD=qemu minix disk` first. The QEMU board emulates the STM32WB's flash rules
  (a double word can only be programmed when erased), so the bugs it finds are real.
- `tools/flashcheck.py flash-budget.txt m2 m2.log` compares the pages the M2 run erased with
  `flipper/flash-budget.txt`. The budget is a ratchet: lower it when a change saves erases,
  raise it only with the reason in the same commit.

## 2.6 Latency (L1–L10)

QEMU's timing says nothing about the Flipper's, so latency is measured on the device:

1. `make BOARD=flipper latcheck`, flash that `minix.bin` (or `minix-base.bin`) and send its base
   package. The trace build adds a 32-event ring and `/dev/lat`.
2. Run `lat -c -w 60` and use the device for that minute: press buttons, start apps, with the
   load the budget names (for L1: `cksum` of a 1 MB file in a loop, plus console output).
3. `lat` prints min, median, p99 and max of L1, L2, L4, L5, L6, L7, L9 and L10, and the Stop2
   statistics.

Record the numbers before and after your change in `PORTING.md`. The release build has no trace
and pays nothing for it.

## 2.7 Memory measurements on the device

- `free`, `free -v`: free RAM, the largest hole, the holes, the flash regions and erase counters.
- `free -k`: high-water marks of every kernel task's stack and of the servers' stacks. Use it
  when you add work to a task; the stacks are sized from these numbers (`src/kernel/table.c`).
- `free -t on`: MM reports untouched `chmem` at every exit (chapter 2 of Part 1).

## 2.8 CI

GitHub Actions runs on every PR and on pushes to `main`:

| Workflow | Checks |
|---|---|
| `build` | Flipper images (`minix card base-flash base pkgs`), `ramcheck` (hard gate), QEMU `M2 USER PASS`, the flash-erase budget, the latency build compiles; then `powercut` with all its cut points. On a `v*` tag it attaches the images to the release. |
| `pr-rules` | a code change adds a line under `Unreleased` in `CHANGELOG.md` (skip with label `no-changelog`); a user-visible change touches `docs/manual/` or the PR description has a `Manual: <why not>` line (skip with `no-manual`). Changes only to Markdown, `docs/` and `man/` count as documentation. |
| `claude-worker` | the cloud worker that implements issues labelled `claude-task` (instructions in `.github/claude-worker.md`) |

A red check means the PR is not ready for review. `powercut` can rarely time out on a busy runner
("printed None"); pushing again reruns it.

## 2.9 Working on a real Flipper

- **Flashing**: `reboot -d` as root restarts into ST's DFU loader with no buttons; then
  `dfu-util -a 0 -s 0x08000000:leave -D build/flipper/minix/minix-base.bin`. If MINIX does not
  come up at all: hold Left + Back, release Back, keep Left (or Left + Back + OK, keep OK).
- **Dev mode**: `reboot -d` also sets the dev bit. Until a normal `reboot` or `halt`, a watchdog
  reset (a hang longer than 2 s) goes back to DFU instead of rebooting, so a broken image returns
  to the loader by itself.
- **Updating the userland** without rewriting the card: `flsend` the new `base_X.Y.tar`, then
  flash the matching `minix-base.bin`. Raise `BASE_VER` in `minix.mk` whenever base content
  changes (a new program, `/etc` files, `postinst`); otherwise `pkg` sees nothing new.
- **The console** is `/dev/ttyACM0` at any baud rate. The debug UART on header pins 13/14
  (115200 8N1, 3.3 V) shows kernel messages before USB is up; it cannot wake the CPU from Stop2,
  so keep USB attached while using it.
- **Panics** print on the console and the UART and stop kicking the watchdog; with dev mode the
  board then returns to DFU.
- **Writing flash during development** wears the device: test with plain installs, and use
  `pkg install --flash` only for what you keep.
- **Radios and readers against a partner**: a second Flipper with Unleashed firmware and
  `tools/twoflip.py` test RFID, IR, iButton and Sub-GHz in both directions; see
  [chapter 6](06-two-flipper-tests.md).
