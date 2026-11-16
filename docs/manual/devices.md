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

## 125 kHz RFID tags (`rfid`, `/dev/rfid`)

Hold a tag (a card, a key fob) flat against the middle of the Flipper's back:

```
rfid
EM4100 04 00 98 3D C5
```

The number is the one the stock app shows and saves (`Data:` in its `.rfid` files). `rfid -r`
also prints the timings, `-n N` makes up to N attempts of half a second (default 4). Without a
tag it says "no tag"; a tag of another kind (HID Prox, Indala) is not decoded yet and is shown
as a summary. Members of `gui` and root may read `/dev/rfid`: each read turns the 125 kHz field on
for half a second and returns the times between the changes of the tag's signal, in µs, as
32-bit numbers.

Writing a blank fob or card (T5577, the common kind; EM4305 is tried next, as the stock
firmware does):

```
rfid write EM 00 DE AD BE EF
written (T5577): EM4100 00 DE AD BE EF
```

Or copy one tag onto another, without typing the number:

```
rfid copy
read EM4100 04 00 98 3D C5
now put the blank one on
written (T5577): EM4100 04 00 98 3D C5
```

The tag is read back afterwards; "not written" means a locked or password-protected tag (one
written by another cloner may be), or a kind that cannot be written. Each round writes the tag
both ways (T5577, then EM4305); `-n N` sets how many rounds (default 20), because a tag that
wants a password only answers now and then.

`-pass HEX` gives a T5577 that password as well, so that nothing can rewrite it without the
word (some door entry systems try to overwrite a copy they find). Write the password down: it
is needed to change the tag afterwards, and this version cannot send one yet. `-lock` instead
sets the chip's lock bit, which can never be undone by anyone, you included. A write to `/dev/rfid` is
a list of field on/off times in µs (on first, an odd count), as `/dev/ir` takes marks and
spaces. Emulating a tag is **(planned)**.

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
