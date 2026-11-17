# Reminders and the alarm clock (`remind`, `alarm`)

Two apps on top of [timers and alerts](timers.md) (spec 7b.1, 7b.2). Both keep nothing in RAM
while they are closed: each reminder or alarm is a timer file in `/var/timers`, `timed` fires it
and `alert` rings. You only open an app to change your reminders or alarms, see what you have
done today, or use the desk clock.

## Installing

```
pkg install /lib/pkg/timed_1.2.tar     # needed by both
pkg install /lib/pkg/remind_1.0.tar
pkg install /lib/pkg/alarm_1.1.tar
```

Each package adds its own user (`remind`, uid 21; `alarm`, uid 22; both in group `timers`), a line
on the launcher (**Kanpaia**, "bell" in Basque, for the reminders, and **Ordua**, "the hour",
for the alarm clock) and its data directory
`/home/flipper/apps_data/remind` or `…/alarm` (mode 700, the app's user). The programs go into
the internal flash at install time (`Flash: yes`), so their code is not copied into RAM.
`pkg remove` takes the app off the launcher; your timers, your data and the user stay, and
installing the package again brings everything back.

The launcher starts Kanpaia as root; as soon as it has the screen and buttons it becomes its own
user. Ordua is a **window app** ([the launcher](launcher.md), spec K.8): the launcher starts it
directly as the user `alarm` and draws its screens itself. Either way an app can only touch its
own timers and files.

## Remind

**Main screen:** how many reminders you marked done today and a bar chart of the last 7 days.
OK opens the list of reminders, Back closes the app.

**List:** the first line, **+ New reminder**, adds one; OK on a reminder edits it.

**Form** (Up/Down: field, Left/Right: change, OK: toggle or the next value, Back: cancel):

| field | values |
|---|---|
| (text) | Drink a glass of water, Stand up and stretch, Rest your eyes, Take your medicine, Breathe slowly, Check your posture |
| Every | 5 … 60 minutes in 5-minute steps, then up to 240 in 15-minute steps |
| From, To | the daily window, 15-minute steps (default 09:00–17:00) |
| Days | Left/Right moves along `MTWTFSS`, OK switches the day on or off |
| Ring at To | `yes`: the end of the window rings too |
| Sound, Vibro, LED | what the alert uses (do-not-disturb and `/etc/notify.conf` still apply) |
| Snooze | 0, 5, 10, 15, 30 minutes for Up/Down on the alert |
| Save, Delete | OK on them |

When a reminder rings, **OK** on the alert counts it as done: `alert` appends one line,
`TIME remind-N`, to `/home/flipper/apps_data/remind/log`, as the user `remind`. Back
(dismiss) and snooze do not count.

From the shell:

```
remind -s        # today 3
                 # week 0 2 5 4 0 1 3   (6 days ago … today)
```

## Alarm (Ordua)

Ordua is a window app: it only describes its screens, and the launcher draws them in the same
style as its own lists, moves the cursor and changes the values. Up/Down move, Left/Right change
the value of the row under the cursor, OK on a time or days row edits it field by field, OK on
a label row opens the keyboard. The hint line at the bottom says what the keys do on that row.
Back goes back one screen; on the list it closes Ordua, which then takes no RAM at all.

**List:** **Desk clock**, **+ New alarm** (not shown once there are 14 alarms), then your alarms
with their time, label and days (`MTWTF--`), `once` or `off`. OK edits one.

**Edit form:**

| row | values |
|---|---|
| Time | the time, local; OK, then Up/Down change the hour or the minute, Left/Right move between them, OK ends |
| Repeat | `on days` (with Days) or `once` (the next time it comes) |
| Days | OK, then Left/Right move along `MTWTFSS`, Up/Down switch the day on or off, OK ends |
| Label | the keyboard, up to 12 characters (new alarms: `Alarm`) |
| On | off keeps the alarm but switches it off |
| Ring settings | opens the next screen |
| Save, Delete | OK on them; both go back to the list |

Back on the form leaves it without saving. Save with `on days` and no day checked says so and
keeps the form.

**Ring settings** (Back returns to the form; Save there keeps them):

| row | values |
|---|---|
| Mode | `escalating` (new alarms): it gets stronger the longer you do not answer. The rings come closer (5, 5, 4, 4, 3, 3, 2, 2 s apart, then every second), a single beep becomes the full sound, then vibro, LED and screen flash join in, and the volume rises from a fifth of **Volume** to all of it. `steady`: everything at once, at **Volume**, a ring every second |
| Stage | `escalating`: 10 (default) … 120 s per stage, in steps of 10; with 10 s the sound and volume reach full together with the rhythm, about 30 s in |
| Volume | 10–100 % in steps of 10 (a cubic curve, as on the stock firmware: 50 % is much quieter than 100 %) |
| Beep | on/off: the speaker |
| Sound | `melody` (the tune) or `single` (beeps of one note) |
| Melody | the built-in `beep`, `chime`, `rise`, `classic`, `gentle`, `pips`, then your own tone file (below) |
| Vibro, LED blink, Screen flash | on/off each; the screen flash blinks the backlight and leaves it as it was afterwards |
| Snooze | off, 5, 10, 15, 20, 30 minutes |
| Ring for | 1, 5, 10, 15, 30, 60 minutes before it gives up |

An alarm **always rings** as set — its outputs and its own volume — also in do-not-disturb and
when the volume of reminders (`/etc/notify.conf`, `Volume`) is 0. It does not need Ordua or the
launcher: `timed` and `alert` ring it, over whatever app is open.

**Your own tone:** put an RTTTL file `tone.rtttl` (or `tone.txt`) or a Flipper music file
`tone.fmf` into `/home/flipper/apps_data/alarm/` (as root or the user `alarm`, readable by
`alarm`); the Melody row offers it after the built-in ones (at most two files: the one the
alarm has, and the first of these names that is there). An alarm made by the older Ordua with
another file name keeps it, and the Melody row shows it.

An alarm that is **off** is kept as `/home/flipper/apps_data/alarm/alarm-N.off`, so `timed`
never looks at it; switching it on moves it back to `/var/timers/alarm-N`. Ordua uses the ids
`alarm-1` to `alarm-14`.

**Desk clock:** the launcher draws a large clock and keeps it to the minute; Ordua gives the two
lines under it: the next alarm (`Next 07:00`, or `Next Tue 07:00` when it is more than a day
away; `No alarm on`) and its label. Ordua sleeps meanwhile (no wake-ups). Back returns to the
list. The backlight follows the system setting (Settings → Display).

From the shell:

```
alarm -n         # alarm-1 1794985200 07:00 Wake up   (id, time in seconds, local time, label)
```

## Limitations

- **RAM while open:** Ordua about **4.4 KB** (its stack, the alarm in the form and the libc
  data; ACK8.3: at most 4.5 KB). Kanpaia about 15.6 KB (its thread stacks, screen buffer and
  the data of the GUI library; the library's code is in flash once for all apps, `free -v`
  shows it as `shared libfuri`). Nothing while they are closed.
- The desk clock's "next alarm" line is the one from when it opened: an alarm that rings or is
  snoozed meanwhile does not change it until you open the desk clock again.
- The desk clock no longer sets the backlight itself (the old Left/Right `auto`/`dim`/`on`);
  a `Light:` line left in `apps_data/alarm/settings` is ignored.

## Where they write

Everything is on the SD card: the timer files in `/var/timers`, the reminder log (one ~20-byte line
per OK) and off alarms in `/home/flipper/apps_data/<app>/`.
Nothing is written to the internal flash at run time; the programs are written there once, by
`pkg install`.
