# 7. The overnight battery test

Everything about the overnight power test in one place: what it proves, how to set up a fresh
computer for it, the evening and morning steps, how to read the numbers, where to record them,
and what has been measured so far. The steps need the MINIX Flipper and one computer with a USB
cable; nothing else. The script `flipper/tools/nightpower.py` takes the readings and judges
them, but every step can also be done by hand in a serial terminal (section 7.6).

## 7.1 What the test proves

The Flipper spends almost all its life idle in a pocket or on a desk, so the idle current decides
the battery life, and the alarm clock and reminders have to survive every night. The test leaves
the Flipper idle on battery for a night (8 h or more) and checks acceptance test **AC6.6** of the
power modes (spec R6.8; `docs/spec/power-plan.md`, PR 2, test 2; first asked for in issue #104):

| Check | Pass when | Why it can fail |
|---|---|---|
| **No reset** | uptime grew by the whole night, and `ver` does not say `reset: watchdog` | the independent watchdog keeps running in Stop2; a sleep that is too long or a wake that does not kick it resets the chip |
| **Clock** | the Flipper's clock moved by at most **±2 s** against an NTP-synchronised computer (about 70 ppm over 8 h; the 32 kHz crystal is good for a few ppm) | ticks lost or counted twice around Stop2 entry and exit |
| **Deep sleep** | more than **99 %** of the time in Stop2 | something keeps the CPU awake: a driver's sleep blocker, a timer firing too often, a background program polling |

and records the energy side:

| Number | Where from | Target |
|---|---|---|
| charge used over the night | the gauge's `soc` in the evening and in the morning | — |
| average current | charge used × 2100 mAh / hours | ≤ 1.5 mA (the stock firmware's level); ≤ 0.6 mA once the screen-off ladder switches the SD card off (power plan PR 4) |
| gauge's standby estimate | `sby` in `/dev/power` | whole mA only |

The floor is the infrared receiver, which draws about 0.35 mA with no way to switch it off
(`docs/spec/power-plan.md` 1.1; the full budget is in the power-budget report).

### Why a night and not a minute

The fuel gauge (BQ27220) counts in whole milliamps, and a USB meter cannot see the idle state at
all, because with USB connected the Flipper never goes into Stop2. What is left without a meter on
the battery leads is the charge percentage over a long time: 1 % of the 2100 mAh battery is
21 mAh, so over 10 hours one percent is about 2.1 mA. A night therefore shows whether the Flipper
is in the right range, and a weekend (48 h, 1 % ≈ 0.44 mA) shows whether it meets 1.5 mA. For
finer numbers use a meter on the battery leads (Nordic PPK2, or a µCurrent with a multimeter).

The reset, the clock and the Stop2 share, on the other hand, are exact after one night.

## 7.2 How the Flipper sleeps (what the numbers mean)

The details are in `flipper/PORTING.md`, "How the sleep works". The short version:

- When nothing runs and **no USB cable** is plugged in, the idle task puts the CPU into Stop2
  until the next clock work is due. The LPTIM1 timer on the 32 kHz crystal wakes it.
- **No sleep is longer than 1 s** (58 ticks, 0.967 s), so that every wake can kick the watchdog.
  At least **62 wake-ups a minute** of Stop2 from the timer (`wother`) are therefore normal and
  expected (the device showed 64); a button press adds one `wbtn`. Every sleep ends in a counted
  wake-up, so `wbtn + wother` is never below `stop2 / 0.967`.
- `wother` is 32-bit since #284 and does not wrap in a night. Older kernels kept it in 16 bits,
  and it wrapped at 65536, about every 18 h. `wbtn` is still 16-bit (65536 presses). For readings
  from an older kernel, `nightpower.py` corrects a wrapped count with the rule above; by hand, add
  65536 until it holds.
- Since R6.8 (#207, merged 2026-11-17) the kernel counts this in every build, and the last four
  fields of `/dev/power` show it:

  ```
  soc=87 mv=3912 ma=-6 avg=-5 sby=-5 temp=243 flags=0x0181 usb=0 chg=0 vbus=0 vreg=4208 ichg=2048 otg=0 up=43210 stop2=41877 wbtn=12 wother=3510
  ```

  `up` seconds since boot, `stop2` seconds of them in Stop2, `wbtn` and `wother` the wake-ups by a
  button and by anything else (the timer, the RTC alarm). The share of the night asleep is
  (stop2 in the morning − stop2 in the evening) / (up in the morning − up in the evening).
- On older kernels these fields are missing. Only a **latency-trace build** (`make latcheck`)
  shows them, through `lat`:

  ```
  Stop2  39800 of 40000 s up (99%), wake-ups: 2 by a button, 39700 other
  Stop2  in and out (clocks back)    39702  avg   140 us        max    180 us
  Stop2  not taken: blocker 3, USB 600, alarm 0, tick work 12, arming 39700, timer write 0, other 0
  ```

  The third line says why idle turns did not sleep, which is the first thing to read when the
  share is below 99 %. A trace build is still the tool for that question; for the share itself
  a normal build is enough now.

## 7.3 A fresh computer

You need: a USB-C data cable, Python 3.8 or newer, a serial terminal, `git`, and permission to open
the Flipper's serial port. To flash a new build you also need `dfu-util`; to build one yourself,
the ARM toolchain (both as in the [installation chapter](../../manual/install.md#1-what-you-need)).

### Linux (Debian, Ubuntu, Mint, Raspberry Pi OS)

```
sudo apt install git python3 screen dfu-util
sudo apt install gcc-arm-none-eabi make python3-pil xfonts-base   # only to build images yourself
git clone https://github.com/arielkonopka/Galtzagorriak-MINIX.git
cd Galtzagorriak-MINIX/flipper
python3 -I tools/nightpower.py --selftest                         # checks the script, no hardware
```

The Flipper is `/dev/ttyACM0` (`ls /dev/serial/by-id/` names it). Your user must be in the port's
group: `ls -l /dev/ttyACM0` shows it (`dialout` on Debian, Ubuntu and Fedora, `uucp` on Arch), then
`sudo usermod -aG dialout $USER` and log out and in again. If the port answers with garbage for
the first seconds, ModemManager is probing it: `sudo systemctl disable --now ModemManager`.
`dfu-util` needs a udev rule for 0483:df11 (Debian 13's `dfu-util` package has one, and qFlipper installs one); without it run the flash
command with `sudo`.

Flashing a build from a new computer onto a card made elsewhere needs a new card first (development
guide [2.9](02-build-test.md#29-working-on-a-real-flipper), "A new computer or compiler"). Read the
night's counters **before** you flash: a flash restarts the Flipper and they start again at 0.

Fedora: `sudo dnf install git python3 screen dfu-util`. Arch: `sudo pacman -S git python screen
dfu-util`.

### macOS

Install Python 3 (python.org or `brew install python`), `git` (Xcode command line tools) and, for
flashing, `brew install dfu-util`. The Flipper's port is `/dev/cu.usbmodem…`
(`ls /dev/cu.usbmodem*`); pass it with `--port`. The script needs no extra modules.

### Windows

Install Python 3 from python.org (tick "Add python.exe to PATH") and Git for Windows, then
`py -m pip install pyserial`. The Flipper is a `COM` port (Device Manager, "Ports (COM & LPT)");
pass it with `--port COM5`. For a terminal use PuTTY (Serial, the COM port, any speed). Flashing
and building go through WSL or a Linux machine, as in the installation chapter.

### Before every run

Close every other program that holds the port (screen, picocom, PuTTY, qFlipper): a serial port
has one reader, and the script then sees no prompt. Make sure the computer's clock is
NTP-synchronised (`timedatectl` on Linux shows "System clock synchronized: yes"; macOS and Windows
do it by default), because the clock check compares the Flipper with it.

## 7.4 The evening

1. **Choose the build.** Any build of main after #207 (2026-11-17) gives every number without a
   special image. Use a latency-trace build only when you want the "not taken" line of `lat`
   (7.2). To flash, as root on the Flipper `reboot -d`, then on the computer
   `dfu-util -a 0 -s 0x08000000:leave -D build/flipper/minix/minix-base.bin` (the trace build is
   in `build/flipper-lat/minix/`; it needs `/dev/lat`: `mknod /dev/lat c 6 15` once, as root).
   Install the matching base package if the build needs it (development guide 2.9).
2. **Leave dev mode.** `reboot -d` sets the dev bit, and in dev mode a watchdog reset goes to the
   DFU loader instead of rebooting. Do one normal `reboot` (as root) after flashing, so that a
   reset in the night reboots the Flipper and shows up as `reset: watchdog` in the morning.
3. **Set the scene.** Note what is running, because it changes the result:
   - Leave the Flipper on the launcher (or at the shell). Do not leave the Power app open: it
     reads the gauge every 10 s.
   - Alarms and reminders that ring in the night are fine (that is the real use), but each one
     wakes the Flipper, and one that is not answered keeps it awake while it rings. For a clean
     baseline night arm none; for an "everyday" night arm the usual ones and say so in the record.
   - No background loops (`while :; do …; done &`), no `jiggle`, nothing streaming.
   - The SD card stays in.
4. **The charge.** A full battery tends to show 100 % for the first hours of discharge (the
   2026-11-17 run stayed at 100 % all night), which hides the drain. When the energy number
   matters, start between about 50 and 90 %. A full battery is fine when only the reset, the
   clock and the Stop2 share matter.
5. **Log in on the console** (`screen /dev/ttyACM0`, any user), then close the terminal
   (`Ctrl-A k y` in screen) so that the port is free. The shell stays logged in.
6. **Take the evening reading:**

   ```
   cd Galtzagorriak-MINIX/flipper
   python3 -I tools/nightpower.py start               # --port COM5 or /dev/cu.usbmodem… elsewhere
   ```

   It prints the version, the reset cause, the charge, the counters and the clock offset, and keeps
   them in `nightpower.json` in the current directory (`--state FILE` for another place). Keep
   that file: the morning compares with it.
7. **Unplug the USB cable at once**, put the Flipper down, and do not press any key until the
   morning. The minutes between `start` and unplugging count as awake, so keep them short.

## 7.5 The morning

1. Plug the USB cable in. The screen stays as it was; the first key only wakes the backlight.
2. Run at once:

   ```
   python3 -I tools/nightpower.py end --report night-$(date +%F).md
   ```

   If the shell was logged out in the meantime, log in with `screen` first and close it again.
3. Read the verdict (7.7). The exit status is 0 when the night passed AC6.6.
4. Record it (7.8).

If the Flipper's screen is dark and the console does not come up, see 7.9.

## 7.6 By hand, without the script

The same readings in a serial terminal. Evening, just before unplugging:

```
ver                  # the build and the reset cause
cat /dev/power       # write down soc, mv, up, stop2, wbtn, wother
date -u              # compare with https://time.is or `date -u` on an NTP-synchronised computer
lat                  # trace builds only: the "Stop2 … of … s up" line
```

Morning, right after plugging in: the same four commands. Then:

- **Reset:** `up` in the morning should be about `up` in the evening plus the night in seconds,
  and `ver` must not say `reset: watchdog`.
- **Clock:** the difference to the reference in the morning minus the one in the evening, within
  ±2 s. Reading a clock by eye is good to about a second; the script does better by catching the
  moment the second turns over.
- **Stop2:** (stop2 morning − stop2 evening) / (up morning − up evening) above 0.99.
- **Current:** (soc evening − soc morning) / 100 × 2100 mAh / hours.

A night that was started **without an evening reading** (on another computer, or before this
script existed) can still be judged from the counters since boot: run
`nightpower.py end` with no state file. It says so, uses `up`/`stop2` (or `lat`) since boot, and
leaves the clock and the charge to you. The time on USB between the boot and unplugging counts as
awake there, so the share comes out a little low; subtract it if you know when you unplugged
(share = stop2 / (up − seconds on USB)).

The time on USB **after** plugging back in is left out by the script itself (Linux and macOS): it
takes the moment the serial port appeared (its device node's change time) as the plug-in and
prints it (`plugged in at … UTC`). On Windows, or when the port was not re-created, run `end` at
once after plugging in.

## 7.7 Reading the result

A good night looks like this (made-up numbers in the shape of the real output):

```
Night: 10.02 h (2026-11-18 21:40 to 2026-11-19 07:41 UTC)
Build: Galtzagorriak 0.1.1+140.g1234567
PASS no reset (cause software, up 36420 s)
PASS clock: +0.4 s over the night (+11.1 ppm; offset +0.3 -> +0.7 s; limit +-2 s)
  this computer's clock must be NTP-synchronised for this to mean anything
PASS Stop2: 99.62 % (35890 of 36028 s; from /dev/power); wake-ups: 0 button, 37200 other (62.0 per minute)
Charge: 82 % -> 80 % (2 %), 3998 -> 3975 mV; about 4.19 mA average from the charge (1 % = 21 mAh; coarse)
Gauge at the end: avg -1 mA, standby estimate -1 mA (whole mA only)
  (read with USB connected: avg/ma show the charger, not the night)

VERDICT: PASS (AC6.6)
```

(Two percent over ten hours is a resolution of ±2 mA: such a night says "a few mA", not 4.19 mA.
Run a weekend for the 1.5 mA question.)

What to do when a line fails:

| Line | Look at |
|---|---|
| `FAIL reset` with `watchdog` | a sleep longer than the watchdog allows, or a wake path that skips the kick (PORTING.md, "Watchdog"). Reproduce with a trace build; the kernel's panic text, if any, went to the UART on header pins 13/14 |
| `FAIL reset` with `power on` or `low power` | the battery ran flat or a brown-out; check the evening `mv` |
| `FAIL clock` | lost or doubled ticks around Stop2 (PORTING.md, "Time"); also check the computer's NTP state before blaming the Flipper |
| `FAIL Stop2` | something kept the CPU awake. Wake-ups far above 62 a minute mean a timer firing too often; few wake-ups but a low share mean a sleep blocker (USB seen as present, an SD transfer, a tune, the LCD console). A trace build's "Stop2 not taken" line names the reason |
| `????` | a reading was missing: an old kernel without R6.8 and not a trace build, or `date` did not answer |

## 7.8 Recording the result

- Add a row to the **Power** table in `flipper/PORTING.md`. `nightpower.py end` prints one ready to
  paste; add in the Notes what was armed and what the Flipper was showing.
- Comment on issue **#109** (the device-check backlog) with the report (`--report` writes it as
  Markdown, with both `/dev/power` lines and any `lat` output), and tick the `#104: overnight
  battery test` line once a night has passed all three checks with the Stop2 share measured.
- A change to the power code (`klib.c` `pm_sleep`, the HAL's `hal_stop2`, the ms timer, a driver's
  sleep blocker) needs a new night before it merges, as its PR's "Needs on-device test" section.

## 7.9 Troubleshooting

- **"no shell prompt on /dev/ttyACM0".** Another program holds the port (close screen, PuTTY,
  qFlipper), the console is not logged in, or the port is another one (`--port`). A program left
  running in the foreground: open the terminal, press Ctrl-C, close it again.
- **The screen is dark and no serial port appears in the morning.** Hold Back about a second (if
  the Flipper was switched off by a power-off or ran flat, this powers it on). If `lsusb` shows
  `0483:df11 STMicroelectronics STM Device in DFU Mode`, the Flipper reset in dev mode and went to
  the loader: that is a failed night (`reset`). Flash again (7.4, step 1).
- **The Stop2 share is 0.** USB stayed connected (a powered hub, a charger, a computer that keeps
  the bus awake): with power on VBUS the Flipper never sleeps (PORTING.md, "USB").
- **The charge did not move.** Normal near full and on short nights; see 7.1.
- **`lat: no latency trace in this kernel`.** Not a trace build; the `/dev/power` counters are
  enough on any build after #207.

## 7.10 What has been measured so far

| Night | Build | Result |
|---|---|---|
| 2026-11-16 (short runs) | main 9ddab7f + T14b part 2 (#116, Stop2) + T15a; then + T14d part 1 (#120) | idle −3 mA, then −2 mA with the LCD blanked (Power app, whole mA); 66 of ~90 s in Stop2. In the PORTING.md Power table |
| 2026-11-16/17, ~11 h | R6.5 part 3 build, 0.1.1+74.ga4b5c0a, normal mode | **no reset** (cause "software" from the reboot before the test), **clock within about 1 s** of the computer after 11 h, charge **held 100 %** (started full at 4138 mV). The exact Stop2 share was not read: that image had neither the R6.8 counters nor `lat`. Issue #109, comment of 2026-11-17 11:10 UTC |
| 2026-11-17/18, 20.0 h | `0.1.1+124.g0566ba1` (built on the old laptop; the commit is not on GitHub). It has the R6.8 counters in `/dev/power`, but not `/dev/lat` (`lat`: ENXIO), so the "not taken" reasons are missing. Booted 22:05 UTC | read 2026-11-18 with `nightpower.py end`, no start file (the first run on hardware). **No reset**; **Stop2 98.22 %**, under 99 %: the awake 21 min include the unknown USB minutes before unplugging and 151 button wakes. 64 wake-ups a minute (`wother` wrapped, #284). Clock **−1.6 s** after 20 h, set by hand to the second (≈ 22 ppm; within ±2 s). Charge not judged (no evening reading). PORTING.md Power table; issue #109 |

Still open: a clean night above 99 % in Stop2 (with `start`, no keys pressed), the idle current against 1.5 mA (needs a weekend
run or a meter on the battery leads), and later ≤ 0.6 mA once the SD card is switched off with
the screen (T14d part 2; the reason it is not done yet is in PORTING.md under the Power table).

## 7.11 The repeat: a long Stop2 run with clock drift and temperature

The 2026-11-17/18 night has to be repeated (it had no evening reading, and the wake-up counter
wrapped, #284). This run also measures the **clock drift**: −1.6 s in 20 h (about 22 ppm, about 2 s
a day) passes AC6.6 but is poor for an alarm clock. A 32 kHz crystal drifts with temperature, so
the room temperature is written down at both ends. The plan:

| Run | Length | USB | Answers |
|---|---|---|---|
| A | 26 h or a weekend, on battery | unplugged | Stop2 share above 99 %, `wother` past 65535 without a wrap (the #344 fix on the device), drift in ppm in Stop2 |
| B | 8 h or more | plugged in all the time (the Flipper never sleeps) | drift in ppm **without** Stop2: if A and B drift the same, the crystal is the cause, if A drifts more, tick counting around Stop2 entry and exit is |
| C | as A, after `rtc cal` | unplugged | whether the calibration brought the drift below about ±5 ppm |

The Stop2 share is only meaningful for A and C; for B it is about 0 %, by design.

### For the person

1. **Before:** a build of main that has #344 (`ver` shows it; the run needs `wother` as 32 bits),
   dev mode off (`reboot` once as root), launcher on the screen, Power app closed, SD card in, no
   alarms for a baseline run. Battery 50 to 90 %.
2. **The sensor.** With the AM2301 (DHT21) connected as soon as `dht` exists (issue #358), `nightpower.py`
   reads it itself. Until then, put any thermometer next to the Flipper (not on the charger, not in
   the sun) and write down the temperature at the start and at the end.
3. **Start** (USB still connected), from `flipper/`:

   ```
   python3 -I tools/nightpower.py start
   ```

   Unplug USB at once and press no key. Do not set the clock with `date` or the phone link during
   the run: the drift is measured against the start offset.
4. **End** (plug in, then at once):

   ```
   python3 -I tools/nightpower.py end --report night-$(date +%F).md
   ```

   It prints the Stop2 share, the wake-ups, whether a counter wrapped, the clock drift in s and
   ppm, and the PORTING.md row. Add the two temperatures to the row by hand (Notes) until the sensor
   is wired in.
5. **Run B** is the same with the cable plugged in the whole time (the console must stay free:
   close the terminal after `start`).
6. **Calibrating:** if A shows a steady drift of 20 ppm or more, as root on the Flipper:
   `rtc cal STEPS`, where one step is 0.954 ppm and **+ is faster**. A clock that lost 22 ppm
   needs about `+23`. Then run C. `rtc` alone shows the calibration in force.
7. **Report** each run in issue #109 with the report file attached, and add its row to the Power
   table in `flipper/PORTING.md`.

### For the agent

Task from #109 (comments of 2026-11-19), local lane; the cloud has written `nightpower.py` and this
chapter.

1. Flash main with #344 and leave dev mode (`reboot`, not `reboot -d`). Confirm
   `cat /dev/power` ends with `up= stop2= wbtn= wother=` and that `ver` names the build.
2. Run A, B and C as above with `nightpower.py start`/`end`. Never set the clock between `start`
   and `end`. If the person is not present, plug and unplug the cable at the times they give; the
   script leaves out the time on USB by itself (it prints `plugged in at …`).
3. For each run record: build (`ver`), hours, Stop2 %, `wbtn`/`wother` per minute, whether a
   counter wrapped, drift in s and ppm, `temp=` from `/dev/power` at both ends (battery
   temperature, not ambient), ambient temperature (sensor or the person's reading), and the
   charge in %.
4. After A: with `wother` past 65535, check `wother >= stop2 / 0.967` **without** unwrapping.
   That is the device test of #344; tick it in #344's "Needs on-device test" list.
5. If A fails the Stop2 share, run once on a `make latcheck` image and attach `lat`'s
   `Stop2 not taken:` line: it names the reason.
6. Compare A with B for the drift. If A drifts more than B by 5 ppm or more, look at
   `pm_sleep()` (`src/kernel/arm/klib.c`): the ticks counted from the LPTIM clocks on each wake
   (`lost_ticks`, the part of the tick already gone) and how the RTC is read at boot.
7. If the drift is steady, calibrate with `rtc cal`, run C, and write the final step count into
   the Power table. A permanent calibration at boot is a design question (where it is stored,
   R6.2): propose it in the issue, do not add it silently.
8. Results go into issue #109 (report attached) and `flipper/PORTING.md`, with the numbers above.
   The wording rule applies: "the user", never "the owner".
