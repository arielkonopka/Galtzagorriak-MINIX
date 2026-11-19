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
| `/dev/led` | 660 `gui` | | 3 bytes red, green, blue; or a text line: a pattern (`blink red`, `rainbow`, ...) |
| `/dev/vibro` | 660 `gui` | | `0` off, `1` on, `N` buzz N ms (2–999) |
| `/dev/speaker` | 660 `gui` | | notes, 4 bytes each; a note of 0xFFFF Hz sets the volume; returns when played |
| `/dev/gpio` | 660 `gui` | one byte `0`/`1` per pin (16 bytes: and which watched pins changed) | pairs of bytes: pin, operation (also: watch for edges) |
| `/dev/adc` | 444 | one text line, millivolts | |
| `/dev/adcs` | 660 `gui` | 16-bit samples of one pin at a set rate | text: pin and rate |
| `/dev/ir` | 660 `gui` | one received frame of durations | a frame to send |
| `/dev/ibutton` | 660 `gui` | the 8-byte ROM of the key on the contacts | `ioctl(IBIOCRUN)`: 1-Wire steps |
| `/dev/rfid` | 660 `gui` | a 125 kHz tag's signal: edge times | field gaps (writing a T5577); `ioctl`s: hold the field, carrier, emulate |
| `/dev/subghz` | 660 `gui` | one text line: RSSI, peak, frequency | text: the frequency to listen on (receive only) |
| `/dev/subghzraw` | 660 `gui` | the demodulated signal as durations | |
| `/dev/rtc` | 660 `timers` | | `ioctl`s: Alarm A, calibration |
| `/dev/hidkbd` | 660 `usb` | the host's lock LEDs, 1 byte, on a change; blocks | 8-byte keyboard reports (3.10e) |
| `/dev/hidmouse` | 660 `usb` | | 4-byte mouse reports |
| `/dev/hidcons` | 660 `usb` | | 2-byte consumer (media key) usages |
| `/dev/power` | 444 | one text line from the fuel gauge, charger and sleep statistics | |
| `/dev/random` | 444 (major 1) | hardware random bytes | |

The user's view of most of these, with shell examples, is in the
[manual's device chapter](../../manual/devices.md); this chapter is the programmer's view.

Not yet: Sub-GHz transmit, `/dev/nfc`, `/dev/ble`, `/dev/sensor/*` are **(planned)** in spec
Phases 8, 13, 15 and section H; the HID files work in QEMU, on the Flipper with T28b (3.10e).

Rules that apply to all of them:
- **Permissions decide access.** Launcher apps run in group `gui`. A program run from a login
  shell as an ordinary user needs that group too, or root.
- **Short operations are on demand.** `/dev/adc`, `/dev/power`, `/dev/ibutton` and `/dev/rfid` do one
  measurement per read, in the kernel, in well under the latency budgets, and power the
  hardware down again. Reading them in a tight loop is legal but wastes battery; read when you
  need a value.
- **Blocking reads really block.** `/dev/buttons`, `/dev/ir`, `/dev/rfid`, `/dev/adcs` and
  `/dev/subghzraw` suspend the reader until there is something; the process uses no CPU
  meanwhile. Set `O_NONBLOCK` with `fcntl` to get `EAGAIN` instead. A signal ends the wait
  with `EINTR`.
- **`poll()` works** (`<poll.h>`, R4b.6): `/dev/buttons` is readable when an event is queued for
  you, `/dev/speaker` writable when no tune plays, `/dev/gpio` readable when a watched pin
  changed (3.7). The capture devices (IR, RFID, Sub-GHz raw, ADC stream) only collect while a
  read waits, so they always report ready: start the read instead of polling them.
- **Captures land straight in your buffer.** IR receive, RFID, `/dev/adcs` and
  `/dev/subghzraw` are timed by the pulse engine (R4b.7): a hardware timer with DMA writes into
  the reading process's memory, which stays pinned for the read. The kernel keeps no copy.
- **One user at a time** where the hardware is single: one reader of `/dev/buttons` (a second
  gets `EIO`), one tune on the speaker. Several devices share timers and buffers, and the one
  that comes second gets `EBUSY`: the speaker and IR transmit; IR transmit, `/dev/adcs` and RFID
  (TIM1/TIM2). Handle `EBUSY` with a message, not a retry loop.

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

`/dev/led` takes three bytes, red, green and blue, 0–255 each, or **a text line** that starts a
pattern (R7b.5):

```c
write(fd, "pulse blue\n", 11);         /* fades in and out */
write(fd, "morph #ff8000 blue\n", 19);
write(fd, "off\n", 4);
```

| Pattern | Runs in | |
|---|---|---|
| `blink`, `fast`, `double`, `pulse` + one colour | the LED chip's own program engines | the CPU can sleep (Stop2) meanwhile |
| `rainbow`, `dual A B`, `dualsoft A B`, `morph A B` | the kernel, stepped up to 25 times a second | wakes the CPU briefly |

Colours are names (`red`, `green`, `blue`, `yellow`, `cyan`, `magenta`, `white`) or `#rrggbb`;
an unknown pattern or colour fails with `EINVAL`. Three raw bytes or `off` stop a pattern. The
LED chip has three engines: one fades the backlight, two run the LED. Prefer the first group of
patterns for anything that runs for long (an alarm, a "charging" state): they cost no CPU.

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

**Volume.** A note with the frequency `0xFFFF` plays nothing and sets the volume, 0–100, to its
duration field, for the notes after it, in this and later writes, until the next such note or
until you close the device (which sets 100 again, so programs that never set it sound as
before). The loudness follows a cubic curve like the stock firmware's: 50 is about an eighth of
the power, 20 is quiet. Respect the user's settings: reminders use the `Volume:` of
`/etc/notify.conf` (chapter 6).

`poll()` with `POLLOUT` tells you when a tune has finished without blocking in `write()`.

From the shell: `beep`, `beep HZ MS ...`, `beep -v VOL ...`, `beep -c` (three rising beeps to call someone). `^G`
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
- **Read** returns one byte per pin, `'0'` or `'1'`, starting with pin 0. A read of 16 bytes
  also returns, in bytes 8–15, `'1'` for each watched pin that changed since the last such read,
  and clears them.
- **Edges (R6.4):** operation `6` watches a pin (set it as an input first), `7` stops.
  `poll()` with `POLLIN` then waits until a watched pin changes, no busy loop:

  ```c
  unsigned char w[2] = { 0, 4 }, v[16];   /* pin 0 (header 2): input, pull-down */
  write(g, w, 2);
  w[1] = 6; write(g, w, 2);               /* watch it */
  struct pollfd p = { g, POLLIN, 0 };
  while (poll(&p, 1, -1) == 1) {
          read(g, v, 16);                 /* v[0]: the level, v[8]: changed */
          /* ... */
  }
  ```
  The watch belongs to the process that set it and ends when it closes `/dev/gpio` or exits; one
  process at a time may watch (`EBUSY` otherwise). The pins are sampled on the clock tick (1/60 s),
  so a pulse shorter than about 17 ms can be missed, and while a pin is watched the Flipper stays
  out of deep sleep (Stop2). Without any watched pin `/dev/gpio` is always readable.

The shell command `gpio` takes header pin numbers or port names: `gpio 2 1`, `gpio pa7 up`,
`gpio` (all levels). Pins 13 and 14 (PB6/PB7) are the debug UART and are not offered. Logic is
3.3 V; never feed a pin more than that.

Edge interrupts (PA4 and PA7 have a free EXTI line: no sampling, deep sleep allowed), pulse
capture and the 5 V supply on pin 1 are **(planned)** (spec R4b.7, R6.4, H.2).

## 3.8 Voltages: `/dev/adc` and `/dev/adcs`

```
$ cat /dev/adc
pa7=1650 pa6=- pa4=- pc3=- pc1=- pc0=> ref=2500
```

Millivolts for each ADC-capable header pin that is in the "off" state, measured against the
chip's internal 2.5 V reference (`ref`). `-` is a pin in use as input or output (reading never
reconfigures a pin), `>` a pin above the reference. One fresh measurement per read (16 samples,
corrected with the factory calibration), a few milliseconds while the reference settles, then
the converter and the reference are off again. Read the whole line at once (80 bytes are enough)
and parse `name=value` pairs; new fields may appear at the end.

**A waveform: `/dev/adcs`** (R6.4 part 2). Write the pin (device index 0–7, as in 3.7) and a
rate in Hz (100–100 000) as text, then read: every 2 bytes are one sample, 0–4095 for
0–2.5 V. The samples go by DMA straight into your buffer; the read returns when it is full.

```c
write(fd, "0 8000\n", 7);          /* pin 2 (PA7) at 8 kHz */
read(fd, samples, 2000);            /* 1000 samples, 125 ms */
```

While a stream runs, IR transmit and RFID are `EBUSY` (shared timer) and the CPU does not enter
Stop2.

## 3.9 Infrared: `/dev/ir`

Raw timings as 32-bit little-endian durations in microseconds: mark, space, mark, ... (an odd
count, starting and ending with a mark).

- **Read** waits for a frame that starts while the read is waiting, and returns it when 30 ms
  pass without carrier (or the buffer is full). The durations are captured by timer and DMA
  straight into your buffer (R4b.7), so give the read a buffer as large as the longest frame
  you expect.
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
touch: `EIO`, try again. A read takes about 10 ms.

For other 1-Wire work (writing an RW1990 blank, talking to other 1-Wire parts) there is
`ioctl(fd, IBIOCRUN, &run)` (`<minix/ibutton.h>`, R6.7 part 2): a list of up to 32 master steps
(`IB_RESET`, `IB_W0`/`IB_W1` bits, `IB_WBYTE`, `IB_RBYTE`) run back to back, with up to 16 result
bytes. Each bit slot masks interrupts for at most about 70 µs (the one measured exception to L4);
pauses a part needs, such as EEPROM programming time, are yours, between calls. The `ikey`
command (`flipper/cmds/ikey.c`: `ikey read`, `ikey write Dallas HEX`) is the worked example.
Emulating a key and the Cyfral/Metakom keys are **(planned)** (R6.7, later parts).

## 3.10a 125 kHz RFID: `/dev/rfid`

The low-frequency antenna, driven by the pulse engine (R6.5, R9.0.4; `<minix/rfid.h>`):

- **Read**: the field goes on for half a second and the read returns the times between changes
  of the tag's signal, 32-bit µs, as `/dev/ir` gives durations. Decoding is yours: the `rfid`
  command links the stock (Unleashed) `lib/lfrfid` unchanged (`flipper/gui/lfrfid.c` is the
  launcher app on top of it).
- **Write**: field on/off times in µs, field on first, an odd count: this is how a T5577 or
  EM4305 is programmed.
- `ioctl(RFIDIOCFIELD, 1)` keeps the field up between a write and the read that follows (a
  T5577 block read needs it); 0, a close or a signal drops it.
- `ioctl(RFIDIOCCARRIER, RFID_CARRIER_125 | _PSK | _134)` picks the field for the next reads:
  125 kHz, 62.5 kHz at 25 % for PSK tags, 134.2 kHz for FDX-B. A close sets 125 kHz again.
- `ioctl(RFIDIOCEMU, &emu)` emulates a tag: a list of segments counted in the *reader's*
  carrier periods, with how many of them load the antenna. The kernel counts the reader's
  carrier and plays the list in step with it, with no CPU work and no drift, until `n` = 0, a
  close or a signal. Your arrays stay pinned meanwhile: do not free them. At most 512 segments.

## 3.10b Sub-GHz: `/dev/subghz`, `/dev/subghzraw`, `/dev/subghzpkt`

Group `subghz` (`<minix/subghz.h>`, R8.1, R8.2, issue #302). The `subghz` command (package
`subghz`, manual chapter [Sub-GHz remotes](../../manual/subghz.md)) is the tool; its protocols
are the library `libsubghz` (`src/lib/subghz`, the Unleashed `lib/subghz`; see its `README`).

- `/dev/subghz` (minor 11): write the frequency in Hz as text (`433920000`); only the stock
  firmware's bands are accepted (300–348, 387–464, 779–928 MHz), others give `EINVAL`. Read one
  line: `rssi=-95 peak=-60 freq=433919830` (dBm; `peak` is the strongest sample during the
  read's ~30 ms; `freq` what the chip tuned). Write `0` to stop. The radio also stops by itself
  after about a minute without a read (it draws ~15 mA).
- `/dev/subghzraw` (minor 14): a **read** gives what the receiver demodulates (on-off keying),
  as 32-bit µs durations, carrier first, the `/dev/ir` format. It waits for the first change and
  ends after 0.1 s of silence or a full buffer. There is no RSSI threshold yet, so noise toggles
  it too. A **write** sends a frame in the same format (each duration 1 µs–1 s) at the frequency
  `/dev/subghz` tuned.
- `/dev/subghzpkt` (minor 20): one packet (1–60 bytes) per write or read, through the chip's
  FIFO, as stock's `subghz chat`; a read with nothing heard gives `EAGAIN`.
- Every transmit is checked by the kernel against the **region** (`SGIOCREGION`, set once per
  boot by `/etc/rc` from `/etc/flipper/region`; `SGIOCGREGION` reads it) and the band's duty
  cycle for the hour (`SGIOCAIR`: used and allowed ms). Errors: `EIO` not tuned, `EPERM` no
  region or outside its bands, `EAGAIN` the hour's share used up, `EBUSY` a reader is listening,
  `EINVAL` a bad frame, `ENOTSUP` no transmitter (the Flipper, for now: the CC1101 transmit path
  is **(planned)**).
- QEMU: a transmit prints console lines (`sg> tx FREQ N` and the durations, 16 a line, off ones
  negative; `sg> pkt FREQ hex…`). Test hooks, `ENOTTY` on the Flipper: `SGIOCLINE` gives the
  FNV-1a hash of the last one printed; `SGIOCFAKE` (`SG_FAKE_RAW` on `/dev/subghzraw`,
  `SG_FAKE_PKT` on `/dev/subghzpkt`) makes the next write a received frame or packet.
  `/usr/tests/sgfake FILE` (package `tests`) plays a `.sub` RAW capture into a running
  `subghz rx` this way; `tools/nattest.py subghz` uses it.

## 3.10c The clock's alarm: `/dev/rtc`

Root and group `timers` (`<minix/rtc.h>`, R6.2, R7b.1). The time itself is read and set with
`time()` and `stime()` as usual; the device is for the rest:

- `RTCIOCSALARM` / `RTCIOCGALARM`: Alarm A at a UTC time at most 27 days ahead (0 clears it),
  and the *lead*, how many seconds early a switched-off Flipper is powered on so it has booted
  in time. When the alarm fires the kernel sends `SIGRTC` to INIT, which runs `timed`. The
  flags say `RTC_ARMED`, `RTC_FIRED`, `RTC_WOKE` (this boot came from the alarm).
- `RTCIOCSCAL` / `RTCIOCGCAL`: the smooth calibration, -511..512 steps of 0.954 ppm, positive
  runs faster; kept by the clock while off.

Programs should not set the alarm themselves: `timed` owns it, and a timer file is the way to be
woken (chapter 6). The `rtc` command is the shell tool.

## 3.10d The radios at a glance: RFID, NFC, Bluetooth, Sub-GHz

Where each radio stands on main, so you know what you can build on today:

| Radio | State | Device | Tools and libraries |
|---|---|---|---|
| **125 kHz RFID** | works: read, write (T5577, EM4305), emulate, clear passwords, read blocks | `/dev/rfid` (3.10a) | `rfid` command; stock `lib/lfrfid` (25 protocols); the RFID launcher app. ASK/biphase protocols read and emulate; FSK and PSK ones are written but not yet read or emulated (T19b) |
| **iButton** | works: read, write RW1990 | `/dev/ibutton` (3.10) | `ikey` |
| **Sub-GHz** | receive: RSSI, the demodulated signal, every Unleashed decoder; transmit with the region's limits in QEMU, on the Flipper **(planned)** | `/dev/subghz`, `/dev/subghzraw`, `/dev/subghzpkt` (3.10b) | `subghz` command (package `subghz`); `libsubghz` (`src/lib/subghz`, 70 protocols, decoders and encoders) |
| **Infrared** | works: send and receive, stock protocols and remote files | `/dev/ir` (3.9) | `ir` |
| **NFC** (ST25R3916, 13.56 MHz) | **(planned)**, spec Phase 13–14 (milestone M6) | `/dev/nfc` to come: field control, polling NFC-A/B/F/V, `transceive` straight between the chip's FIFO and your buffer, card emulation with the chip's own anticollision | the stock NFC protocol libraries ported as user-space libraries. Nothing in the code yet |
| **Bluetooth LE** (CPU2) | **(planned)**, spec Phase 15–16 (milestone M7). The radio core is never started today | a transport task `ble` and a `bled` daemon (GATT serial, battery, HID, pairing with a PIN on the screen), started on demand; `rpcd` for the official phone app | `sbctl` (Storz & Bickel remote) has its protocol core ready and host-tested (`flipper/apps/sbctl`) waiting for it |

What you can do now for the planned radios: write the protocol logic as a host-tested core with
no I/O (the `sbctl` and `camfind` pattern, chapter 2), so it only needs a thin layer once the
device exists. Spec sections: NFC 13–14 and 14.1–14.2, BLE 15–16, Sub-GHz 8–9.

## 3.10e USB keyboard, mouse, media keys: `/dev/hidkbd`, `/dev/hidmouse`, `/dev/hidcons`

Root and group `usb` (`<minix/hid.h>`, R10.2, R11.3.1; the user's view is the manual's
[USB gadgets](../../manual/usb-gadgets.md)). Minors 17-19; the header is the contract every
backend implements: QEMU's prints each report as a console line `hid< kbd 02 00 0b ...`, the
Flipper's USB one is T28b (until then the bus reads "not attached").

- **Write whole reports**: 8 bytes on `hidkbd` (modifiers, 0, six key codes), 4 on `hidmouse`
  (buttons, dx, dy, wheel; −127..127, so 0x80 is `EINVAL`), 2 on `hidcons` (a consumer usage,
  little endian). One write may carry several reports, up to `HID_MAXWRITE` (64) bytes; they go
  out in order. A report is a state: send the key-up (all zeros) yourself, or use `libhid`.
- **Not listening**: no cable, not configured or suspended gives `EAGAIN`. Wait with `poll()`:
  `POLLOUT` when a report would go out, `POLLIN` when the state changed since your last
  `HIDIOCSTATE` (and, on `hidkbd`, when the LEDs changed since the last read). A program that
  only needs to act on a change (the coming `jiggle`) sleeps in `poll()` and costs nothing
  while unplugged.
- `ioctl(HIDIOCSTATE, &int)`: `HID_ATTACHED`, `HID_CONFIGURED`, `HID_SUSPENDED` in bits 0-7,
  the persona holding the bus in 8-15 (`HID_PERSONA()`), the LED byte in 16-23 (`HID_LEDS()`).
- **One USB persona at a time**: `ioctl(HIDIOCSPERSONA, &p)` takes the bus for a non-HID persona
  (`HIDP_CANARY`, `HIDP_READER`, `HIDP_STORAGE`), 0 gives it back, and so does the holder's
  close. Meanwhile `open()` of a HID file and writes on open ones fail with `EBUSY`. Taking it
  never waits: the canary always wins.
- QEMU test hooks (`ENOTTY` on the Flipper): `HIDIOCFAKE` plays the host (bus bits, LEDs <<
  16); `HIDIOCLINE` gives the FNV-1a hash of the last `hid<` line, which `m2test` compares with
  the line it expects.

**`libhid`** (`src/lib/hid`, static, no malloc, no stdio; R11.3.2) builds the reports. Layouts are
the stock Bad USB `.kl` files: exactly 256 bytes, 128 little-endian entries, one per ASCII code,
each the key code (low byte) and the boot modifier byte (high byte: Shift 0x02, AltGr 0x40). US is
built in (`hid_us`, const); `hid_kl_load(kl, "de-DE")` reads `/usr/share/hid/layouts/de-DE.kl`
or `/ext/badusb/assets/layouts/de-DE.kl` (on MINIX `/home/flipper/badusb/assets/layouts/`) into your 256-byte buffer and refuses a file of another
size. `hid_type(fd, kl, "Kaixo!\n", &skipped)` types UTF-8 text, key then key-up in one write per
character; characters without a key are counted in `skipped`. `hid_mouse()` clamps to −127..127.
Host test: `make hidtest` (all printable ASCII in US and DE back through a reverse table).

**The Bad USB engine** (`flipper/apps/badusb/ducky.c`, R11.3.3) runs DuckyScript on top of
`libhid`: `dk_run(&d, fd)` reads the script a line at a time (a 128-byte buffer and one 256-byte
line on the stack; `REPEAT` seeks back and reads the line again) and hands each report to the
caller's `struct dk_io` (keyboard, mouse, consumer, wait, button). With `d.io == NULL` it only
checks the script (`badusb -n`). The `badusb` command adds the device files, `poll()` sleeps and the
stop signals (blocked, looked at every 50 ms: no handler, so no MM gap). `make hidtest` also runs
`ducktest`: golden reports, the `-n` errors and a fuzz test of random scripts under ASan/UBSan;
`ducktest -r LAYOUT SCRIPT` prints the reports a script makes, which `tools/nattest.py badusb`
compares with the `hid<` lines of QEMU.

```c
#include <fcntl.h>
#include <minix/hid.h>
#include "libhid.h"

int fd = open("/dev/hidkbd", O_WRONLY), skipped;

if (fd < 0) ...			/* EBUSY: another USB persona has the port */
if (hid_type(fd, hid_us, "Kaixo!\n", &skipped) < 0) ...	/* EAGAIN: unplugged */
```

## 3.11 Battery: `/dev/power`

One text line per open, read on demand from the fuel gauge and the charger, with the CPU's sleep
statistics at the end (R6.8: uptime, time in Stop2, wake-ups by buttons and by other causes):

```
soc=87 mv=3912 ma=-6 avg=-5 sby=-5 temp=243 flags=0x0181 usb=0 chg=0 vbus=0 vreg=4208 ichg=2048 otg=0 up=43210 stop2=41877 wbtn=12 wother=3510
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
