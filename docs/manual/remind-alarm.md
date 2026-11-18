# Reminders and the alarm clock (`remind`, `alarm`)

Two apps on top of [timers and alerts](timers.md) (spec 7b.1, 7b.2). Both keep nothing in RAM
while they are closed: each reminder or alarm is a timer file in `/var/timers`, `timed` fires it
and `alert` rings. You only open an app to change your reminders or alarms, see what you have
done today, or use the desk clock.

## Installing

```
pkg install /lib/pkg/timed_1.2.tar     # needed by both
pkg install /lib/pkg/remind_1.1.tar
pkg install /lib/pkg/alarm_1.0.tar
```

Each package adds its own user (`remind`, uid 21; `alarm`, uid 22; both in group `timers`), a line
on the launcher (**Kanpaia**, "bell" in Basque, for the reminders, and **Ordua**, "the hour",
for the alarm clock) and its data directory
`/home/flipper/apps_data/remind` or `…/alarm` (mode 700, the app's user). The programs go into
the internal flash at install time (`Flash: yes`), so their code is not copied into RAM.
`pkg remove` takes the app off the launcher; your timers, your data and the user stay, and
installing the package again brings everything back.

Kanpaia is a [window app](launcher.md) (spec K.8, `Type: window`): its launcher line is
`Kanpaia:remind:@bell:=/usr/bin/remind`, so the launcher starts it as the user `remind` and draws
its windows itself. Ordua is still a full-screen app: the launcher starts it as root, and as soon as
it has the screen and buttons it becomes its own user. Either way an app can only touch its own
timers and files. Installing `remind` 1.1 over 1.0 turns the old launcher line into the new one.

## Remind (Kanpaia)

The launcher draws every window, moves the cursor and changes the values; Kanpaia only describes
them and sleeps until you press OK, so the keys never wait for it. Back goes one window back and
closes the app on the first one. Nothing of it stays in RAM after that.

**Main window:**

| row | right side | OK |
|---|---|---|
| Today | reminders marked done today | the reminders that ring today, by their next time (`rang`: none left today) |
| Upcoming | how many reminders there are | every reminder by its next time: `14:30` today, `Tu 09:00` this week, `11-24 09:00` later, `--` never again |
| History | marked done in the last 7 days | the last 64 times you marked one done, newest first |
| + New reminder | | an empty form |

Long lists come 8 rows at a time; the launcher fetches the next page when the cursor reaches the
edge, so the RAM stays the same however many reminders or log lines there are. OK on a reminder in
any list opens its form.

**Form** (Up/Down: row; Left/Right or OK: change; OK on a time, date or days row edits it one
field at a time; Back: cancel):

| row | values |
|---|---|
| Text | OK opens the keyboard (up to 40 characters; default "Drink a glass of water") |
| Repeat | `once`, `daily`, `interval` |
| Date | `once`: the day |
| Time | `once`, `daily`: when it rings; `interval`: the start of the window (default 09:00) |
| Until | `interval`: the end of the window (default 17:00) |
| Every | `interval`: 5 … 240 minutes in 5-minute steps (default 30) |
| Days | `daily`, `interval`: the days of the week |
| Snooze | off, 5, 10, 15, 30 minutes for Up/Down on the alert |
| Sound, Vibro, LED | what the alert uses (do-not-disturb and `/etc/notify.conf` still apply) |
| Save, Delete | OK on them; Delete only on a reminder that exists |

Save checks the form first: a text, a time still to come for `once`, at least one day, and Time
before Until for `interval`. When something is missing, the form's title tells you what, and
nothing is written. Kanpaia keeps up to 32 reminders (`remind-1` … ); a reminder file written by
older versions with Ring at To (`Inclusive: yes`) keeps it.

When a reminder rings, **OK** on the alert counts it as done: `alert` appends one line,
`TIME remind-N`, to `/home/flipper/apps_data/remind/log`, as the user `remind`. Back
(dismiss) and snooze do not count.

From the shell:

```
remind -s        # today 3
                 # week 0 2 5 4 0 1 3   (6 days ago … today)
```

## Alarm

**List:** the first line opens the **Desk clock**, the second, **+ New alarm**, adds one; then
your alarms with time, days (or `once`) and label. OK edits, Back closes the app.

**Form** (the same keys as in Remind):

| field | values |
|---|---|
| Hour, Minute | the time, local |
| Repeat | `on days` (with Days) or `once` (the next time it comes) |
| Days | as in Remind |
| Label | Alarm, Wake up, Work, School, Medicine, Nap is over |
| Mode | `escalating` (new alarms): it gets stronger the longer you do not answer. The rings come closer (5, 5, 4, 4, 3, 3, 2, 2 s apart, then every second), a single beep becomes the full sound, then vibro, LED and screen flash join in, and the volume rises from a fifth of **Volume** to all of it. `steady`: everything at once, at **Volume**, a ring every second |
| Stage | `escalating`: 10 (default), 20, 30, 60, 90, 120 s per stage; with 10 s the sound and volume reach full together with the rhythm, about 30 s in |
| Volume | 10–100 % in steps of 10 (a cubic curve, as on the stock firmware: 50 % is much quieter than 100 %) |
| Sound | `yes`/`no`: the speaker |
| Sound type | `melody` (the tone's tune) or `single` (beeps of one note) |
| Tone | the built-in `beep`, `chime`, `rise`, `classic`, `gentle`, `pips`, then your own files |
| Vibro, LED blink, Screen flash | `yes`/`no` each; the screen flash blinks the backlight and leaves it as it was afterwards |
| Snooze | 0, 5, 10, 15, 20, 30 minutes |
| Ring for | 1, 5, 10, 15, 30, 60 minutes before it gives up |
| On | `no` keeps the alarm but switches it off |
| Save, Delete | OK on them |

An alarm **always rings** as set — its outputs and its own volume — also in do-not-disturb and
when the volume of reminders (`/etc/notify.conf`, `Volume`) is 0.

**Your own tones:** put RTTTL (`.rtttl` or `.txt`) or Flipper music (`.fmf`) files into
`/home/flipper/apps_data/alarm/` (as root or the user `alarm`, mode readable by `alarm`); the
Tone field offers them after the built-in ones.

An alarm that is **off** is kept as `/home/flipper/apps_data/alarm/alarm-N.off`, so `timed`
never looks at it; switching it on moves it back to `/var/timers/alarm-N`.

**Desk clock:** a large clock, the date and the next alarm. It redraws once a minute (the app
sleeps until the next minute; no polling). Left/Right sets the backlight: `auto` (as
everywhere), `dim` or `on`; the choice is remembered. Back returns to the list and gives the
backlight back as it was.

From the shell:

```
alarm -n         # alarm-1 1794985200 07:00 Wake up   (id, time in seconds, local time, label)
```

## Limitations

- **Kanpaia takes 4.0 KB of RAM while it is open** (`free -v` before and after; it was 15.6 KB
  as a full-screen app): no GUI library, screen buffer or thread, only its form, the time-zone
  data of `localtime` and a 2.25 KB stack. **Ordua takes about
  16.1 KB** (its thread stacks, screen buffer and the data of the GUI library; the library's code
  is in flash once for all apps, `free -v` shows it as `shared libfuri`) until it becomes a window
  app too (T25b). Both take nothing while closed.
- The desk clock wakes once a minute from a timed wait; the RTC wake-up timer (planned, R7b.1)
  will replace it.

## Where they write

Everything is on the SD card: the timer files in `/var/timers`, the reminder log (one ~20-byte line
per OK), off alarms and the desk clock's backlight setting in `/home/flipper/apps_data/<app>/`.
Nothing is written to the internal flash at run time; the programs are written there once, by
`pkg install`.
