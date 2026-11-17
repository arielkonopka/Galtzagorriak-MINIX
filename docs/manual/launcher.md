# The launcher

The launcher is the main screen: the list of apps you see after the boot screen, and where you
come back to when an app ends.

![The app list](img/launcher-list.png)

## The app list

- **Up/Down** choose an app, **OK** starts it. Back (long, in any app) ends the app and brings
  you back here.
- The black title bar shows, on the left, the battery: its level, the charge in percent and a
  bolt while it charges. On the right is the position of the chosen app, e.g. `3/12`.
- The line at the bottom says what the buttons do on this screen: `<Setup  OK Run`.
- With more apps than fit (8 rows), a thin line on the right edge shows which part of the list
  you see.
- Short notices, such as `unknown user` or `app crashed`, appear on the bottom line for 2
  seconds instead of covering the screen. The list stays visible.

**Left** opens **SETTINGS**, the system settings. Left counts only as a short press on its
own: holding Left+Up to turn off the backlight (see [Buttons and gestures](buttons.md)) does
not open them.

## SETTINGS

The page lists the groups of settings: Display, Sound, LED, Time, Power, Storage, Gestures,
Launcher and About. OK opens one, Back returns to the app list. The ones marked `(later)` are
not there yet; Power and About open today's Power and About apps.

**Display** and **Sound** are small screens of their own (the `settings` program): Up/Down choose
a line, Left/Right change its value, which takes effect at once, Back leaves.

| Group | Line | Values |
|---|---|---|
| Display | Brightness | 10-100 % |
| | Timeout | the light goes off after 5 s ... 5 min without a key, or never |
| | Light | auto (the timeout), off or on: the forced modes of the button gestures; not kept over a restart |
| Sound | Volume | 0-100 %, the volume of every program that does not set its own (alarms set theirs) |
| | Test beep | OK plays three notes |

The values are kept in `/etc/flipper/display.conf` and `sound.conf` on the SD card, written once
when you leave the screen and only if something changed, and applied at every start
(`settings apply` in `/etc/rc`). Deleting a file brings the defaults back (100 %, 15 s, 100 %).
From the shell: `settings display`, `settings sound`.

## The launcher's own settings

![The settings page](img/launcher-settings.png)

Launcher, on the SETTINGS page:

| Setting | What it does |
|---|---|
| Icons | the small pictures before the app names |
| Hints | the button line at the bottom and the position in the title bar |
| Battery | the battery on the title bar |

Up/Down choose a setting, OK (or Left/Right) switches it, Back returns to SETTINGS. The
settings are kept in `/etc/launcher.conf` on the SD card, which is written only when you go back
to the app list with something changed:

```
Icons: on
Hints: on
Battery: on
```

You can also edit that file from the shell; the launcher reads it again after the next app ends.
With all three off, the screen looks as it did before icons existed.

The battery value is read when the screen is redrawn, at most once a minute; the launcher never
wakes up only to update it.

## The list of apps: `/etc/apps`

One line per app:

```
Label:user:icon:/program arg ...
Label:user:/program arg ...          (no icon: the older form, still valid)
Label:user:icon:=/program arg ...    (a window app, see below)
```

The program must be an absolute path (starting with `/`). The app runs as `user`, in group `gui`.
With `=` before the program it is a **window app**: it runs as `user` in that user's own group
and never touches the screen.
The icon is one of:

- **empty** (`Label:user::/program`): no icon;
- **`@name`**: a built-in icon, e.g. `@bell`;
- **`#` and 12 hex digits**: your own 8×6 picture, one byte per row from the top, the left pixel
  in the highest bit, e.g. `#183C3C7E0018` (a small bell);
- **one or two characters**, drawn in the list font, e.g. `>_` or `Zz`.

A broken icon (an unknown name, bad hex digits, three characters) shows as no icon; the app
still starts.

The built-in icons:

![The built-in icons](img/launcher-icons.png)

`app`, `console`, `info`, `light`, `power`, `battery`, `rfid`, `nfc`, `subghz`, `ir`,
`ibutton`, `gpio`, `usb`, `ble`, `clock`, `alarm`, `bell`, `timer`, `sensor`, `game`,
`music`, `tools`, `file`, `settings`.

`/etc/apps` is read from the SD card, or from `/rom/etc/apps` if the card has none. Updating the
system adds icons to the base apps' lines as they were shipped; lines you changed stay as they
are.

## Window apps

Some apps do not draw on their own: they only describe their screens, and the launcher draws
them (spec K.8). Such an app needs no graphics code, so it takes about 4 KB of RAM while open
instead of about 15 KB. Its package says `Type: window`, and its line in `/etc/apps` has `=`
before the program. The example `gmdemo` package (`pkg install /lib/pkg/gmdemo_1.0.tar`) puts
**Demo** on the list.

A window app's screens look like the launcher's: the title bar, a selection bar, values in
brackets on the right and a hint line that names the buttons for the row under the cursor:

| Row | Buttons |
|---|---|
| an entry that opens something (`>` before it) | OK opens it |
| a number, a choice, on/off (`[40 %]`, `[rise]`, `[on]`) | Left/Right change it (OK too) |
| a time, a date, week days (`[07:30]`, `[2026-11-17]`, `[MTWTF--]`) | OK starts editing: Left/Right choose the part, Up/Down change it, OK ends |
| a text (`[Wake up]`) | OK opens the keyboard |
| a button row (`> Save`) | OK presses it |

- **The keyboard**: four rows of letters, digits and signs, then `Aa` (capitals), `Del`, `Spc`
  and `Done`. The arrows move, OK types the key under the cursor, `Done` keeps the text, Back
  leaves it as it was.
- **Back** goes to the previous screen of the app at once, and on its first screen it closes the
  app. Back always works, even when the app does not answer.
- Values change on the screen at once. The app gets the changed values when you leave the screen
  or press a button row.
- When the app takes longer than 0.15 s to answer, a small hourglass appears on the right of the
  title bar; the screen still responds. An app that is still running 2 s after Back closed it is
  stopped (`SIGTERM`).
- Long lists are loaded a page (at most 16 rows) at a time; the title bar shows the position in the
  whole list, e.g. `17/40`.
- A **clock** screen shows the time in large digits and redraws once a minute while it is shown.

**Watching the traffic.** With `GuiLog: on` in `/etc/launcher.conf` the launcher copies every
record it gets from a window app (`gui< ...`) and every event it sends (`gui> ...`) to the
console. It is off when the line is missing, and the settings page keeps it as it is.

Writing a window app: the [Development Guide, chapter 7](../devguide/part1/07-desktop-modules.md).
