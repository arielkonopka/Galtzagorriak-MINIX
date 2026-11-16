# 3. Using the hardware: device files

All of the Flipper's hardware that MINIX drives so far is reached through files in `/dev`. There
is no special API: `open`, `read`, `write`, and in a few places `ioctl`. That means the shell can
use the hardware too, which is handy for trying things before writing C.

All the devices below are minors of one kernel device task (major 6, `src/kernel/arm/flipdev.c`;
the header comment there is the authoritative description). The user-level view of some of
them is also in the [manual's device chapter](../../manual/devices.md).

## 3.1 Overview

| File | Mode, group | Read | Write |
|---|---|---|---|
| `/dev/lcd` | 660 `gui` | the current frame (1024 bytes) | a frame (1024 bytes); one byte `C`: the text console takes the screen |
| `/dev/buttons` | 660 `gui` | button events, 2 bytes each; blocks | |
| `/dev/backlight` | 660 `gui` | state, 4 bytes | 1 byte level, or 3 bytes level + idle timeout |
| `/dev/led` | 660 `gui` | | 3 bytes: red, green, blue |
| `/dev/vibro` | 660 `gui` | | `0` off, `1` on, `N` buzz N ms (2–999) |
| `/dev/speaker` | 660 `gui` | | notes, 4 bytes each; returns when played |
| `/dev/gpio` | 660 `gui` | one byte `0`/`1` per pin | pairs of bytes: pin, operation |
| `/dev/ir` | 660 `gui` | one received frame of durations | a frame to send |
| `/dev/ibutton` | 660 `gui` | the 8-byte ROM of the key on the contacts | |
| `/dev/adc` | 444 | one text line, millivolts | |
| `/dev/power` | 444 | one text line from the fuel gauge | |
| `/dev/random` | 444 (major 1) | hardware random bytes | |

Not yet: `/dev/rtc`, `/dev/subghz`, `/dev/nfc`, `/dev/rfid`, `/dev/ble`, `/dev/sensor/*`,
`/dev/usbhid` and the 5 V header supply are **(planned)** in spec Phases 6, 8, 10, 13 and 15.

Rules that apply to all of them:
- **Permissions decide access.** Launcher apps run in group `gui`. A program run from a login
  shell as an ordinary user needs that group too, or root.
- **Short operations are on demand.** `/dev/adc`, `/dev/power` and `/dev/ibutton` do one
  measurement per read, in the kernel, in well under the latency budgets, and power the
  hardware down again. Reading them in a tight loop is legal but wastes battery; read when you
  need a value.
- **Blocking reads really block.** `/dev/buttons` and `/dev/ir` suspend the reader until there
  is something; the process uses no CPU meanwhile. Set `O_NONBLOCK` with `fcntl` to poll
  instead (you get `EAGAIN`). A signal ends the wait with `EINTR`.
- **One user at a time** where the hardware is single: one reader of `/dev/buttons` (a second
  gets `EIO`), one tune on the speaker, and the speaker and infrared share a buffer, so one of
  them gets `EBUSY` while the other is in use.

## 3.2 Screen: `/dev/lcd`

A frame is 1024 bytes in the ST7567's page order: byte `page * 128 + x`, bit `n` is pixel row
`page * 8 + n` (page 0 is the top 8 rows). A set bit is a dark pixel. Write the whole frame in
one `write()`; the kernel sends it to the display at once.

Setting a pixel:

```c
fb[(y >> 3) * 128 + x] |= 1 << (y & 7);
```

The `gui` library (chapter 4) does this and draws text, lines and boxes for you; use it unless
you have a reason not to.

Reading `/dev/lcd` returns the last frame an app wrote (it returns 0 bytes while the text
console has the screen). Writing the single byte `C` hands the screen to the text console
(`/bin/console` does that).

While a **foreground app** is set (chapter 4), frames written by anybody else are dropped
silently: the write "succeeds", nothing changes on screen.

## 3.3 Buttons: `/dev/buttons`

Each event is two bytes: the button bit, then `1` for pressed or `0` for released.

| Bit | Button |
|---|---|
| `0x01` | Up |
| `0x02` | Down |
| `0x04` | Left |
| `0x08` | Right |
| `0x10` | OK |
| `0x20` | Back |

Read in multiples of two bytes; a read returns as many events as are waiting and fit. The
kernel queues 32 events. Events are produced from interrupts with an 8 ms debounce, and
answered in microseconds (budget L2: 2 ms).

- **React on the press.** The stock firmware does, users expect it, and it halves the
  button-to-pixel latency (L1).
- While a foreground app is set, other readers wait; their presses are not saved for later.
- The first press after the backlight went off only wakes the light; it is swallowed and not
  delivered.
- Holding Back for one second sends `SIGTERM` to the foreground app (chapter 4).

## 3.4 Backlight and LED

`/dev/backlight` takes one byte, the brightness 0–255, or three bytes: brightness and the idle
timeout in seconds (16-bit little endian; 0 = never; default 15). The kernel turns the light off
after the timeout, dims it 10 % a second over the last five, and turns it on again at any button.
The settings survive resets. Reading gives 4 bytes of state (`HAL_BL_*` in `flipper/hal/hal.h`).
The shell command is `backlight LEVEL [SECONDS]`.

`/dev/led` takes three bytes, red, green and blue, 0–255 each.

```sh
printf '\377\000\000' > /dev/led      # red (printf with octal escapes)
printf '\000\000\000' > /dev/led      # off
```

LED blink patterns running in the LED chip's own program engines, with no CPU involvement, are
**(planned)** with alerts (spec F.2).

## 3.5 Vibration: `/dev/vibro`

Write text: `0` stops, `1` starts, and a number from 2 to 999 buzzes that many milliseconds.
A timed buzz is ended by the kernel's millisecond timer, so the writer does not need to stay
awake, and it works while the CPU sleeps in Stop2.

```sh
echo 200 > /dev/vibro
```

## 3.6 Speaker: `/dev/speaker`

Write notes of four bytes each: frequency in Hz and duration in ms, both 16-bit little endian.
0 Hz is a rest. Up to 64 notes per write. The `write()` returns when the notes have played; the
kernel times them from the clock tick (1/60 s resolution), so the program sleeps meanwhile.

```c
unsigned char tune[8] = { 0xE8, 0x03, 100, 0,     /* 1000 Hz, 100 ms */
                          0xD0, 0x07, 100, 0 };   /* 2000 Hz, 100 ms */
write(fd, tune, sizeof tune);
```

From the shell: `beep`, `beep HZ MS ...`, `beep -c` (three rising beeps to call someone). `^G`
on the console also beeps.

## 3.7 GPIO header: `/dev/gpio`

The eight free header pins, numbered 0–7 in the device:

| Device index | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| Port | PA7 | PA6 | PA4 | PB3 | PB2 | PC3 | PC1 | PC0 |
| Header pin | 2 | 3 | 4 | 5 | 6 | 7 | 15 | 16 |

- **Write** pairs of bytes, pin index and operation: `0` output low, `1` output high, `2` input,
  `3` input with pull-up, `4` input with pull-down, `5` off (analog, high impedance, the
  default).
- **Read** returns one byte per pin, `'0'` or `'1'`, starting with pin 0.

The shell command `gpio` takes header pin numbers or port names: `gpio 2 1`, `gpio pa7 up`,
`gpio` (all levels). Pins 13 and 14 (PB6/PB7) are the debug UART and are not offered. Logic is
3.3 V; never feed a pin more than that.

Edge interrupts, pulse capture and the 5 V supply on pin 1 are **(planned)** (spec R4b.7, R6.4,
H.2).

## 3.8 Voltages: `/dev/adc`

```
$ cat /dev/adc
pa7=1650 pa6=- pa4=- pc3=- pc1=- pc0=3297 vdda=3301
```

Millivolts for each ADC-capable header pin that is in the "off" state, `-` for a pin in use as
input or output (reading never reconfigures a pin), and the supply. One fresh measurement per
read (16 samples, corrected with the chip's internal reference), under 1 ms. Read the whole line
at once (a buffer of 80 bytes is enough) and parse `name=value` pairs; new fields may appear at
the end.

## 3.9 Infrared: `/dev/ir`

Raw timings as 32-bit little-endian durations in microseconds: mark, space, mark, ... (an odd
count, starting and ending with a mark).

- **Read** waits for a frame that starts while the read is waiting, and returns it when 30 ms
  pass without carrier (or the buffer is full).
- **Write** sends up to 2048 durations (each ≤ 1 s) at 38 kHz with a third duty cycle. A first
  word with bit 31 set chooses the carrier instead: bits 0–19 the frequency in Hz
  (10000–60000), bits 20–26 the duty in percent (0 = 33). The write returns when the frame is
  sent.

The `ir` command decodes and encodes the stock firmware's protocols (NEC, Samsung32, RC5, RC6,
SIRC, Kaseikyo, RCA, Pioneer and variants) and reads and writes its remote files, so `ir send
tv.ir Power` works with files from a stock Flipper. Its source (`flipper/cmds/ir.c`) is a good
example of protocol code on top of a raw device.

## 3.10 iButton: `/dev/ibutton`

A read of at least 8 bytes returns the ROM of the Dallas 1-Wire key on the contacts: family
code, 6-byte serial, CRC (already checked), then end of file. No key: `ENODEV` at once. A bad
touch: `EIO`, try again. A read takes about 10 ms. Writing and emulating keys are
**(planned)** (R6.7).

## 3.11 Battery: `/dev/power`

One text line per open, from the fuel gauge, read on demand:

```
soc=87 mv=3912 ma=-6 avg=-5 sby=-5 temp=243 flags=0x0181 usb=0 chg=0 vbus=0
```

The fields are explained in the [manual's power chapter](../../manual/power.md). Nothing is ever
written to the gauge. On QEMU the read fails with `ENXIO`.

## 3.12 Random numbers: `/dev/random`

Bytes from the STM32's hardware random number generator. Use it for salts, nonces and random
names; `rand()` is not random. A read can be short while the radio core holds the generator:
loop until you have what you need. **On QEMU it is a weak fallback** (a xorshift seeded from the
uptime), clearly not for keys.

## 3.13 Who may use what: an example

A program run from a login shell as an ordinary user, say `alex`, cannot write `/dev/vibro`
(`EACCES`): the file is mode 660, group `gui`. MINIX 2.0 has no supplementary groups yet
(`setgroups` is spec R4b.4, **(planned)**), so listing `alex` in `/etc/group` does not help; a
process has exactly one group. The options today:

- run the program from the launcher, which starts apps in group `gui`;
- make `gui` (gid 10) the user's login group in `/etc/passwd`;
- run it as root while developing.

Do not make device files world-writable to get around this: any script or stranger's program
would then have the same access.
