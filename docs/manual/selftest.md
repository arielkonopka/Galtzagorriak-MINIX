# The self test

A hardware self test in the spirit of the old home computers' self-test programs (spec 7b.3).
It comes as the package `stest`:

    pkg install /lib/pkg/stest_1.0.tar      # or send it with flipper/tools/flsend

It adds **Self test** to the launcher.

## Using it

The menu lists **All tests** and then each test. Up and Down choose, OK runs, Back leaves.

- **All tests** runs every automatic test again and again and counts the loops. It stops at
  the first failure. Back stops it between two tests.
- A test picked from the list runs once. Interactive tests ask you to do something and say what
  they need on the screen. Afterwards the result is shown until you press a key.
- Holding Back for a second ends a test that hangs (as for any app); it is logged as
  `FAIL stopped`.

From a shell (as root), for scripts:

    selftest -a              # every automatic test once; exit status: the number of failures
    selftest -a -n 100 -k    # 100 loops, going on after a failure

Every result is appended to `/home/flipper/apps_data/selftest/log`, one line each:

    2026-11-17 14:49:40 board PASS

## The tests

| Test | Kind | What it checks |
|---|---|---|
| **Board** | automatic | The RTC runs (the clock advances, its calibration value). The SD card: its size, 1 MB read and the speed, and one scratch sector written, read back and compared. The sector is the one just before the first partition, which no file system uses, and its old content is put back. The battery gauge (charge, voltage and temperature in range) and the charger's state. The LED chip answers. USB connected or not (never a failure). |
| **Memory** | automatic | All 256 KB of RAM as an 8 × 8 grid of 4 KB blocks. Every block no program uses is tested by the memory manager itself (March C−, a pattern written and read back in both directions) and left zeroed; no program ever sees it. Blocks in use are shown dotted, the system's own memory (kernel, servers, the FS cache and the radio core's share) hatched. Filled: passed; a cross: failed (the result names the address). |
| **Input** | interactive | A picture of the d-pad, OK and Back; each lights while you hold it. Press and release every button. Fails after 30 seconds without a press and names the buttons missing. |
| **Sound, vibration and light** | interactive | A scale rising and falling on the speaker, three vibration pulses, the LED red, green, blue and white, the display all on, all off, a checkerboard and its inverse, and the backlight from dark to bright. Then OK if all of it looked and sounded right, Back if not. |

**(planned, spec R7b.3.1, R7b.3.5)** The flash part of the memory test (each region against its
checksum, with a picture of the flash pages, and the ECC counts), the GPIO loopback test
with jumpers, and tests for the radios as their drivers arrive. Starting the self test with Up +
Back held at power-on is planned as well.

## Adding a test

A test is a program in `/usr/lib/selftest/` with a one-line description file next to it,
`NAME.d`: `a` (automatic) or `i` (interactive), a space, and the text for the menu. The
program draws its own screen and writes one line to standard output: `PASS`, `FAIL reason` or
`SKIP reason`. With `-a` it must not wait for anybody. See `flipper/apps/selftest/` for the
existing ones.
