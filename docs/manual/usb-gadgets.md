# USB gadgets: keyboard, mouse and media keys

With a USB cable, the Flipper can act as a **keyboard**, a **mouse** and a set of **media keys**
for the computer it is plugged into, while the USB console keeps working. The stock firmware's
Bad USB and USB remote do this; on Galtzagorriak MINIX they are spec section 11.3. This chapter
grows with each part:

| Part | What | State |
|---|---|---|
| The device files | `/dev/hidkbd`, `/dev/hidmouse`, `/dev/hidcons`, `libhid` | **in QEMU** (T28a); on the Flipper the files exist but say "not attached" until the USB part lands **(planned, T28b)** |
| `badusb` and the **Bad USB** app | run DuckyScript files from `/ext/badusb/` | **(planned, T28c)** |
| **Teklatua** | keyboard, numpad, media keys, mouse, clicker, jiggler pages | **(planned, T28d)** |
| `jiggle` | keeps a computer awake, runs without a window | **in QEMU** (T28e); on the Flipper with T28b |

The apps never touch the USB hardware; they write reports to the device files below. That also
means you can use the files yourself, from C or a script.

## The device files

| File | Mode, group | Write | Read |
|---|---|---|---|
| `/dev/hidkbd` | 660 `usb` | 8-byte keyboard reports: modifiers, 0, six key codes | one byte, the computer's lock LEDs (1 Num, 2 Caps, 4 Scroll), when they change |
| `/dev/hidmouse` | 660 `usb` | 4-byte reports: buttons (1 left, 2 right, 4 middle), dx, dy, wheel, each −127..127 | |
| `/dev/hidcons` | 660 `usb` | 2-byte media keys: volume up `e9 00`, down `ea 00`, mute `e2 00`, play/pause `cd 00`, next `b5 00`, previous `b6 00`, brightness `6f 00`/`70 00`; `00 00` releases | |

Only root and the group **`usb`** may open them. Programs that type into another computer
should run in that group and nobody else should be in it.

A report is a state, not a key press: `02 00 0b 00 00 00 00 00` holds Shift and H (the US
key code 0x0b) down until the next report, so every key needs a second report with all zeros
(the key-up). `libhid` does both for you.

**When the computer is not listening** (no cable, not configured yet, asleep) a write fails with
`EAGAIN`. A program waits for it with `poll()` (`POLLOUT`) instead of trying again in a loop;
`POLLIN` tells it that the state changed (cable in or out, suspended, LEDs).

### One USB persona at a time

The USB port can be one thing at a time besides the console: the HID gadgets above, the USB
canary (spec 11.2), a reader mode (11.1) or mass storage. While another one holds the port,
opening a HID file fails with **"Device busy"** (`EBUSY`) and a program that had it open gets
`EBUSY` on its next write. The canary always wins: it never waits for a HID app.

### In QEMU

QEMU has no USB, so each report is printed on the console as one line:

```
hid< kbd 02 00 0b 00 00 00 00 00
hid< kbd 00 00 00 00 00 00 00 00
hid< mouse 01 05 fd 00
hid< cons e9 00
```

The tests (`M2`, and `nattest` for the apps to come) check these lines.

## `jiggle`: keep the computer awake

`jiggle` moves the mouse a little every so often, so that the computer on the cable does not
lock its screen or go to sleep. It has no window and takes about 1 KB of RAM, so it can run
all day next to your other apps:

```
jiggle &              every 30 s a 1-pixel square: right, down, left, up
jiggle -i 60 &        every 60 s (1 to 3600)
jiggle -p zero &      a report that does not move the pointer at all
jiggle -k             stop it
```

While it runs the status line shows **`J`**. It sleeps between reports and does not wake at
all while the cable is out, the computer has not set up the Flipper yet or has suspended the
USB bus, or another USB persona (the canary) holds the port: it carries on by itself once the
computer listens again. Only one `jiggle` runs at a time; a second one says
`already running`. `jiggle -k` works for the user who started it and for root.
The Jiggler page of Teklatua starts and stops the same command **(planned, T28d)**.

You need to be root or in the group `usb` (see above). `man jiggle` has the details.

## Keyboard layouts

A computer turns key codes into letters with **its own** keyboard layout, so the Flipper must
know which one the computer uses: `z` is a different key on a German keyboard than on a US one.
Layouts are the stock firmware's `.kl` files (256 bytes each, the stock Bad USB format, so the
files from the stock SD card work unchanged). US is built in; the others are looked up as
`/usr/share/hid/layouts/NAME.kl`, then `/ext/badusb/assets/layouts/NAME.kl` (the stock card's
place). A file that is not exactly 256 bytes is refused. Only ASCII characters have keys in a
`.kl` file; other characters (`ñ`, `€`) are left out (Bad USB's `ALTCHAR` will type them,
**(planned, T28c)**). On a German layout `^` and `` ` `` are dead keys: the computer waits for
the next key before it shows them.

## On the Flipper **(planned, T28b)**

Until the USB part lands, the files exist on the card (new cards, and `pkg install` of base on
older ones) but the bus always reads "not attached" and writes fail with `EAGAIN`. Then the USB
port becomes a composite device (console + one HID interface for keyboard, mouse and media keys),
and a computer sees a keyboard named after the Flipper.
