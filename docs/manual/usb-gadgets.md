# USB gadgets: keyboard, mouse and media keys

With a USB cable, the Flipper can act as a **keyboard**, a **mouse** and a set of **media keys**
for the computer it is plugged into, while the USB console keeps working. The stock firmware's
Bad USB and USB remote do this; on Galtzagorriak MINIX they are spec section 11.3. This chapter
grows with each part:

| Part | What | State |
|---|---|---|
| The device files | `/dev/hidkbd`, `/dev/hidmouse`, `/dev/hidcons`, `libhid` | **in QEMU** (T28a); on the Flipper the files exist but say "not attached" until the USB part lands **(planned, T28b)** |
| `badusb` and the **Bad USB** app | run DuckyScript files from `/ext/badusb/` | **(planned, T28c)** |
| **Teklatua** | keyboard, numpad, media keys, mouse, clicker, jiggler pages | **in QEMU** (T28d), package `usbrem`; on the Flipper with T28b |
| `jiggle` | keeps a computer awake, runs without a window | **(planned, T28e)** |

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

## Teklatua: the Flipper as keyboard and mouse

**Teklatua** (Basque for keyboard) is on the launcher once the `usbrem` package is installed
(`pkg install /lib/pkg/usbrem_1.0.tar`). It is a window app, so the desktop draws it and it
takes about 3 KB of RAM while open, nothing when closed. It runs as the user `usbremote`, the
one user in group `usb` that may write the HID files.

The first row of every page is **Page**: Left/Right turn the pages.

| Page | What it does |
|---|---|
| **Keyboard** | **Type** opens the keyboard. Each key you press goes to the computer at once; **Del** sends Backspace. After 48 characters the row starts empty again (the keyboard closes; OK opens it). The rows below send **Enter**, **Backspace**, **Tab** and **Esc**. **Layout** is the computer's layout: `en-US` (built in) or `de-DE` (needs `de-DE.kl` in `/usr/share/hid/layouts/` or `/ext/badusb/assets/layouts/`) |
| **Numpad** | the same keyboard, but digits, `+ - * / .` and Enter are the number-pad keys |
| **Media** | Volume +, Volume −, Mute, Play/Pause, Next track, Previous track |
| **Mouse** | **Mouse pad** opens the pad: the arrows move the pointer (faster the longer you hold them: 1, 2, 4, then 8 pixels a step), **OK** clicks, **OK held** half a second clicks the right button, **Back held with Up/Down** turns the wheel. **Back alone** leaves the pad |
| **Clicker** | clicks by itself, **Rate** 1–50 clicks a second with the **Button** you choose; **Start**/**Stop**. It clicks only while this page is open |
| **Jiggler** | **Start jiggle** runs the `jiggle` command in the background with the **Interval** and **Pattern** set here; it goes on after you close Teklatua and shows `J` on the desktop. **Stop jiggle** ends it. Needs the `jiggle` command **(planned, T28e)**: until then the page says "jiggle is not installed" |

The last line of each page tells what the USB side is doing: `USB: ready`, `USB: no cable`,
`USB: waiting for the host`, `USB: host asleep`, `USB busy: another persona` (the canary or a
reader mode holds the port) or `No access (group usb)`. Settings are not saved: Teklatua starts
with the defaults (en-US, 10 clicks/s, 30 s) every time, and writes nothing to the card or flash.

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
