# USB gadgets: keyboard, mouse and media keys

With a USB cable, the Flipper can act as a **keyboard**, a **mouse** and a set of **media keys**
for the computer it is plugged into, while the USB console keeps working. The stock firmware's
Bad USB and USB remote do this; on Galtzagorriak MINIX they are spec section 11.3. This chapter
grows with each part:

| Part | What | State |
|---|---|---|
| The device files | `/dev/hidkbd`, `/dev/hidmouse`, `/dev/hidcons`, `libhid` | **in QEMU** (T28a); on the Flipper the files exist but say "not attached" until the USB part lands **(planned, T28b)** |
| `badusb` and the **Bad USB** app | run DuckyScript files from `/ext/badusb/` | **in QEMU** (T28c); on the Flipper with the USB part **(planned, T28b)** |
| **Teklatua** | keyboard, numpad, media keys, mouse, clicker, jiggler pages | **in QEMU** (T28d), package `usbrem`; on the Flipper with T28b |
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
| **Jiggler** | **Start jiggle** runs the `jiggle` command in the background with the **Interval** and **Pattern** set here; it goes on after you close Teklatua and shows `J` on the desktop. **Stop jiggle** ends it. |

The last line of each page tells what the USB side is doing: `USB: ready`, `USB: no cable`,
`USB: waiting for the host`, `USB: host asleep`, `USB busy: another persona` (the canary or a
reader mode holds the port) or `No access (group usb)`. Settings are not saved: Teklatua starts
with the defaults (en-US, 10 clicks/s, 30 s) every time, and writes nothing to the card or flash.

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
The Jiggler page of Teklatua starts and stops the same command.

You need to be root or in the group `usb` (see above). `man jiggle` has the details.

## Keyboard layouts

A computer turns key codes into letters with **its own** keyboard layout, so the Flipper must
know which one the computer uses: `z` is a different key on a German keyboard than on a US one.
Layouts are the stock firmware's `.kl` files (256 bytes each, the stock Bad USB format, so the
files from the stock SD card work unchanged). US is built in; the others are looked up as
`/usr/share/hid/layouts/NAME.kl`, then `/ext/badusb/assets/layouts/NAME.kl` (the stock card's
place; on MINIX `/ext` is `/home/flipper`, so that is `/home/flipper/badusb/assets/layouts/`).
The `badusb` package brings `de-DE`. A file that is not exactly 256 bytes is refused. Only ASCII characters have keys in a
`.kl` file; other characters (`ñ`, `€`) are left out (Bad USB's `ALTSTRING` types them as Alt
codes, below). On a German layout `^` and `` ` `` are dead keys: the computer waits for
the next key before it shows them.

## Bad USB

Bad USB types a prepared script into the computer the Flipper is plugged into, as if someone typed
it on a keyboard. **Run scripts only on computers you own or may use.** Install it with

    pkg install /lib/pkg/badusb_1.0.tar

It adds the **Bad USB** app to the launcher, the `badusb` command, the German layout and four
example scripts in `/ext/badusb` (`/home/flipper/badusb`): `windows`, `macos` and `linux` open an
editor or a terminal and type a greeting; `keys` waits for OK, then types every letter, digit and
sign, to check that the layout matches the computer's. The examples belong to the package (an
upgrade puts them back): change a copy. Put your own scripts there as `.txt` files (names of at
most 14 characters with the `.txt`, as for every file on the card).

### The app

- **The list**: *Settings*, then the scripts in `/ext/badusb` (paged when there are many).
- **OK on a script** asks first: the script, the layout, "Connect the USB cable to the target.
  Run?". OK runs it, Back does not. Nothing ever runs without this question.
- **While it runs** the window shows the state (*Waiting for the host* until the computer has set
  the Flipper up, *Running*, *Press OK to go on* at a `WAIT_FOR_BUTTON_PRESS`: OK on *Continue*),
  and `Line n / total`. At the end: *Done* (with the number of characters the layout had no key
  for, if any), *Error at line N* with the reason, or *Stopped*.
- **Back stops the script** at once (within 100 ms): every key and mouse button is let go, so no
  key stays pressed on the computer, and the list comes back.
- **Settings**: *Layout* (en-US, de-DE and any `.kl` file in the two layout folders) and *Default
  delay* (0–1000 ms after every line). They are saved in `/etc/flipper/badusb.conf`, only when
  you change a value (an empty file means the defaults).

The app runs as the user `badusb` in the group `usb` and costs about 3.3 KB of RAM while it is
open, the running script about 1–3 KB more, nothing when it is closed.

### The `badusb` command

    badusb [-n] [-l LAYOUT] [-d MS] FILE

runs a script from the shell (`-l de-DE`: the layout, `-d`: a default delay in ms). `badusb -n
FILE` only checks it and prints the first error with its line:

    # badusb -n test.txt
    badusb: test.txt: line 3: unknown command or key: FOO

Ctrl-C (or `kill`) stops it within 100 ms with every key up. At a `WAIT_FOR_BUTTON_PRESS` it waits
for Enter. It needs the group `usb` (or root). Exit status: 0 done, 1 an error, 3 stopped. Nothing
starts a script at boot unless you add a `badusb` line to a timer or to `/etc/rc.local` yourself.

### The script language

The scripts are the stock firmware's (DuckyScript 1.0 with its extensions), so scripts from the
stock SD card run unchanged. One command per line; lines that are empty or start with `REM` do
nothing.

| Line | Does |
|---|---|
| `STRING text` | types the text (UTF-8; characters with no key in the layout are left out) |
| `STRINGLN text` | the same, then Enter |
| `DELAY ms` | waits (at most 3 600 000 ms, one hour) |
| `DEFAULT_DELAY ms` | waits this long after every following line (`DEFAULTDELAY`) |
| `STRING_DELAY ms` | between the characters of the next `STRING` only (`STRINGDELAY`); `DEFAULT_STRING_DELAY ms`: of every one |
| `REPEAT n` | the line before again, n times (at most 10 000) |
| `GUI r`, `CTRL-ALT DELETE`, `ENTER`, ... | a key combination: modifiers (`CTRL`, `SHIFT`, `ALT`, `GUI`, `WINDOWS`, `CTRL-ALT`, `CTRL-SHIFT`, `ALT-SHIFT`, `ALT-GUI`, `GUI-SHIFT`, `GUI-CTRL`), then keys (`ENTER`, `ESC`, `TAB`, `SPACE`, `BACKSPACE`, `DELETE`, `INSERT`, `HOME`, `END`, `PAGEUP`, `PAGEDOWN`, `UP`/`DOWN`/`LEFT`/`RIGHT` (also `UPARROW` ...), `CAPSLOCK`, `NUMLOCK`, `SCROLLLOCK`, `PRINTSCREEN`, `PAUSE`/`BREAK`, `MENU`/`APP`, `F1`–`F24`) or, after a modifier, one character |
| `HOLD key`, `RELEASE key` | presses a key, modifier or mouse button and keeps it down; lets it go (everything is let go at the end anyway) |
| `ALTCHAR n` | Alt + the number on the keypad (Windows Alt codes) |
| `ALTSTRING text` | every character as an Alt code (`ALTCODE`); ASCII as its number, `ñ` as `0241` |
| `SYSRQ key` | Alt + PrintScreen + the key (Linux magic SysRq) |
| `MEDIA name` | a media key: `VOLUME_UP`, `VOLUME_DOWN`, `MUTE`, `PLAY_PAUSE`, `PLAY`, `PAUSE`, `STOP`, `NEXT_TRACK`, `PREV_TRACK`, `EJECT`, `BRIGHT_UP`, `BRIGHT_DOWN`, `POWER`, `REBOOT`, `SLEEP`, `LOGOFF`, `EXIT`, `HOME`, `BACK`, `FORWARD`, `REFRESH`, `SNAPSHOT`, `FN` |
| `GLOBE key` | the Fn/Globe key held with the key (Apple) |
| `MOUSEMOVE x y`, `MOUSESCROLL n` | moves the mouse, turns the wheel (`MOUSE_MOVE`, `MOUSE_SCROLL`) |
| `LEFTCLICK`, `RIGHTCLICK`, `MIDDLECLICK` | a click (also `LEFT_CLICK` ..., `WHEELCLICK`), with modifiers before it if wanted |
| `WAIT_FOR_BUTTON_PRESS` | waits for OK in the app (Enter at the shell) |
| `ID VID:PID ...` | checked, but the USB ids are not changed (planned: `usbmode`) |

A line longer than 256 bytes is an error (it is not cut and typed in part), and so are numbers out
of range: `badusb -n` reports them.

## On the Flipper **(planned, T28b)**

Until the USB part lands, the files exist on the card (new cards, and `pkg install` of base on
older ones) but the bus always reads "not attached" and writes fail with `EAGAIN`. Then the USB
port becomes a composite device (console + one HID interface for keyboard, mouse and media keys),
and a computer sees a keyboard named after the Flipper.
