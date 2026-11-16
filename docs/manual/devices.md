# Hardware devices

The Flipper's hardware appears as files under `/dev`, so the shell and any program can use it.
This chapter grows as the drivers arrive; the apps built on them get their own chapters.

## iButton (`/dev/ibutton`)

Reads the ROM of a Dallas 1-Wire key (DS1990A and compatible) held on the iButton contacts.
Members of the `gui` group (the launcher's apps) and root may read it.

```
od -x /dev/ibutton
```

With a key on the contacts this prints its 8-byte ROM: the family code, the 6-byte serial
number and a CRC byte, the same number the stock iButton app shows under "Read". Each read
gives the ROM once, then end of file.

- With no key on the contacts the read fails at once with "No such device" (ENODEV).
- A bad touch (wrong CRC, or a line held low) fails with "I/O error" (EIO); touch again.

A read takes about 10 ms. Writing keys, emulating a key and the Cyfral/Metakom keys are
**(planned)** (spec R6.7, later parts), as is an iButton app in the launcher.

## Voltages on the header pins (`/dev/adc`)

Measures the voltage on the GPIO header pins that have an analog input: PA7, PA6, PA4, PC3,
PC1 and PC0 (PB3 and PB2 have none). Anyone may read it.

```
cat /dev/adc
pa7=1650 pa6=- pa4=- pc3=- pc1=- pc0=> ref=2500
```

Values are in millivolts, from 0 to 2.5 V: the converter measures against the chip's internal
2.5 V reference (`ref`, as the stock firmware does), so a pin above 2.5 V shows `>`. A pin must
never get more than 3.3 V. Only a pin left in its default "off" state (`gpio N off`) is
measured; a pin set up as an input or output shows `-`, so reading never disturbs it. Each read
takes one fresh measurement (16 samples averaged, corrected with the chip's factory
calibration), usually in a few milliseconds while the reference settles, and the converter and
the reference are switched off again afterwards. Continuous sampling and an app with a
voltmeter screen are **(planned)** (spec R6.4, later parts).

### Sampling a pin at a fixed rate (`/dev/adcs`)

For programs that need a waveform (for example the camera finder's light-flicker check):
write the header pin (counted as `gpio` counts them, 0 = pin 2 … 7 = pin 16) and a rate in Hz
(100–100 000), then read: each 2 bytes are one sample, 0–4095 for 0–2.5 V (the reference shown
by `cat /dev/adc` as `ref=`).

```
echo 0 8000 > /dev/adcs                         # pin 2 (PA7) at 8 kHz
dd if=/dev/adcs bs=2000 count=1 | od -d | head  # 1000 samples, 125 ms
```

The samples go straight into the reading program's memory; the read waits until they are all
there. While a stream runs, infrared sending is not possible (they share a timer), and the
Flipper does not go into deep sleep.

## Sub-GHz radio, listening only (`/dev/subghz`)

The Sub-GHz chip (CC1101) can listen on one frequency and report the signal strength there. It
never transmits through this device.

```
echo 433920000 > /dev/subghz    # listen at 433.92 MHz (in Hz)
cat /dev/subghz                  # rssi=-95 peak=-60 freq=433919830
echo 0 > /dev/subghz             # stop
```

Frequencies are those the stock firmware accepts: 300–348, 387–464 and 779–928 MHz; anything else
is refused. `peak` is the strongest of the samples taken during the read (about 30 ms), so short bursts of a
remote are not missed. The frequency read back is what the chip really tuned (in steps of about 400 Hz). The
signal strength is in dBm: around −100 means quiet, a remote pressed nearby shows −40 or more. The
radio keeps listening until `echo 0`, or until nobody has read it for about a minute (it draws
about 15 mA while listening). A spectrum view and
recording will build on this.

## The clock, its alarm and calibration (`/dev/rtc`, `rtc`)

The Flipper's real-time clock keeps UTC on the battery, also while the Flipper is off. Setting
the time (`date`) now writes it too, so the time survives a reset and power-off, and every
program reads the time from it.

Alarm A of the clock is for the timers and the alarm clock (spec F.2, coming with `timed`); the
`rtc` command is the tool underneath, for root and the group `timers`:

```
rtc                 # the alarm, the boot lead, how this boot started, the calibration
rtc alarm +600      # in 10 minutes
rtc alarm off
rtc lead 12         # a switched-off Flipper is started 12 s before the alarm
rtc cal -20         # 19 ppm slower (about 1.6 s a day)
```

- When the alarm fires, INIT gets `SIGRTC` and runs `/usr/lib/timed` if it is installed (also
  once at every boot, and after the time is set).
- **Alarm while off.** If the Flipper is switched off (`halt`, on battery) with an alarm set,
  the clock switches it on again, *lead* seconds early so it has finished booting by then (first
  10 s). `rtc` then says "this boot: by the alarm". On USB power the Flipper does not really
  switch off (the charger keeps it on), so this only works on battery.
- An alarm at most 27 days ahead; further ones fail with "Invalid argument" (timed arms the
  nearest one and moves on).
- The calibration is kept by the clock itself, also while off. One step is 0.954 ppm, about
  0.08 s a day; positive runs faster. Measure the drift against a good clock over some days.

### Raw signal (`/dev/subghzraw`)

What the receiver demodulates (on-off keying), as the times between its changes: 32-bit
little-endian microseconds, carrier first, the same format as `/dev/ir`. Tune with
`/dev/subghz` first; a read waits for the first change and ends after 0.1 s of silence or when
the buffer is full:

```
echo 433920000 > /dev/subghz
ir rx -r -d /dev/subghzraw
```

Without a signal strength threshold (planned) the receiver's noise toggles it too, so a read
fills up with noise at once; FSK remotes (many car keys) do not show at all.

## The status LED (`/dev/led`)

The RGB LED next to the screen. Root and the `gui` group (the launcher's apps) may write it.

Three bytes set red, green and blue directly (0–255 each). A text line starts a pattern.
The first four run in the LED driver chip itself, so the CPU can sleep meanwhile; the colour
sequences (`rainbow`, `dual`, `dualsoft`, `morph`) are stepped by the kernel, which wakes the
CPU briefly up to 25 times a second:

```
echo blink red >/dev/led
echo pulse blue >/dev/led
echo double #ff8000 >/dev/led
echo morph #ff8000 blue >/dev/led
echo off >/dev/led
```

| pattern | what it does                                  |
|---------|-----------------------------------------------|
| `blink` | a 0.1 s flash about once a second             |
| `fast`  | flashes about five times a second             |
| `double`| two short flashes, then a pause of about 1.6 s|
| `pulse` | fades in and out, about every 1.8 s           |
| `rainbow`| the six colours of the pride flag, 1 s each, gliding into the next in 0.2 s |
| `dual A B` | switches between colours A and B every 0.5 s |
| `dualsoft A B` | A and B, 1 s each, gliding into the other in 0.2 s |
| `morph A B` | glides from A to B and back, 10 s for the round |

Colours: `red`, `green`, `blue`, `yellow`, `cyan`, `magenta`, `white`, or any `#rrggbb` (hex,
as on the web, corrected for the LEDs so that `#ff8000` looks orange; very dark colours such as
`#080808` are refused). An unknown pattern or
colour fails with "Invalid argument" (EINVAL). Setting the colour with three bytes, or `off`,
stops the pattern; the screen's backlight does not (the chip has three
program engines: one fades the backlight, two run the LED). The pattern stops with a reset.
