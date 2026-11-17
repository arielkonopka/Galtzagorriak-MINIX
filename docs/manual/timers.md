# Timers and alerts (`timed`, `alert`)

Things that should happen at a time (an alarm, a reminder every half hour, a program run every
morning) are **timer files**. Nothing waits in RAM for them: the clock's Alarm A holds the next
time, and when it comes INIT runs `/usr/lib/timed`, which does what is due, sets the alarm for
the next time and exits (spec F.2).

The apps that use them are the interval reminder `remind` and the alarm clock `alarm`
([their chapter](remind-alarm.md)); this chapter is the machinery underneath, which also works
from the shell.

## Installing

```
pkg install /lib/pkg/timed_1.0.tar
```

This puts `timed` and `alert` in `/usr/lib`, makes the spool `/var/timers` (owner root, group
`timers`, mode 1770: root and group `timers` may add timers, each only removes its own) and
`/etc/notify.conf` if they are missing, and `/home/flipper/apps_data` for app settings. INIT runs
`timed` at every boot, when the alarm fires and after the time is set; `kill -7 1` (as root) runs
it now.

## A timer file

One file per timer in `/var/timers`, mode **600** (other files are ignored), named by the
timer's id (1–14 letters, digits, `.`, `_`, `-`). `Key: value` lines:

```
Kind: interval
Days: Mo Tu We Th Fr
Start: 09:00
End: 17:00
Every: 30
Inclusive: no
Category: reminder
Missed: skip
Action: notify
Title: Water
Text: Drink a glass of water
Snooze: 5
```

| key | values |
|---|---|
| `Kind` | `once` (with `At:`), `daily` (with `Time: HH:MM`), `interval` (with `Start:`, `End:`, `Every:` minutes) |
| `At` | `once`: seconds since 1970, UTC |
| `Days` | `all` (default), `weekdays`, `weekend`, or day names `Su Mo Tu We Th Fr Sa` |
| `Inclusive` | `interval`: `yes` makes `End` itself fire too (09:00 … 17:00 instead of … 16:30) |
| `Category` | `alarm`, `reminder`, `info` (default): how it treats do-not-disturb |
| `Missed` | `skip` (default) or `fire-once`: what to do when its time passed while the Flipper was off or the clock jumped forward |
| `Action` | `notify` (default): show an alert; `run` with `Program: /path`: start it as the file's owner with `--timer ID` |
| `Title`, `Text` | the alert's text (23 and 63 characters) |
| `Sound`, `Vibro`, `Led` | `yes` (default) or `no` |
| `Snooze` | minutes Up/Down snooze for, 0–60 (0: no snooze) |
| `Tone` | a built-in tone (`beep`, `chime`, `rise`, `classic`, `gentle`, `pips`) or the full path of an RTTTL or `.fmf` file the owner may read |
| `Ring` | minutes the alert rings, 1–60 (default: alarms 10, others 1) |
| `Ramp` | `yes`: single beeps for the first 30 s, then the whole tune |
| `Log` | an app name: OK on the alert appends `TIME ID` to `/home/flipper/apps_data/APP/log`, as the timer's owner |

`daily` and `interval` times are **local time** (`/etc/TZ`, else UTC) and follow summer time;
`once` is a UTC instant. An `interval` counts from `Start` every day. A `once` timer is deleted
after its time.

A time counts as on time up to 60 s late. If `timed` finds the latest occurrence later than that
(the Flipper was off, or `date` moved the clock forward), `Missed:` decides: `skip` drops it,
`fire-once` fires it once, however many occurrences were missed.

## Do not disturb and the other settings

`/etc/notify.conf` (root) and then `~/.config/notify` (the timer owner's) say how notifications
may get attention:

```
Volume: 80          # 0 = no sound (the speaker has no volume steps yet)
Vibro: yes
Led: yes
Dnd: 22:00-07:00    # do not disturb, local time; "Dnd: off"
```

An **alarm always rings** as it is set (sound and vibro), also during do-not-disturb and with
`Volume: 0`; an alarm with `Sound: no` only vibrates. **Reminders and info** are not shown during
do-not-disturb, and use only what the settings allow otherwise.

## The alert

`alert` shows the title, the time and the text on the screen, rings every 5 s (a short tune,
vibration, and the LED: fast red for alarms, a blue pulse otherwise) and waits for a button:

| button | meaning |
|---|---|
| OK | done |
| Back | dismiss |
| Up or Down | snooze (a new `once` timer `_ID`), when the timer has `Snooze:` |

Without a button it gives up after the timer's `Ring:` time (default 10 minutes for alarms,
1 minute otherwise). It also writes one line to
the console log, for example `alert: 09:30 reminder: Water: Drink a glass of water`.

**Limitation:** the alert takes the screen only when no app holds it (the launcher's main
screen). Over a running app it only rings, vibrates and logs, and cannot be answered with the
buttons, until foreground preemption (`FG_PREEMPT`, spec R7b.2) arrives.

## Commands

```
/usr/lib/timed -v              # run now, saying what it fires and what it arms (root)
/usr/lib/timed -v -n SECONDS   # as if the time were SECONDS since 1970 (tests)
/usr/lib/alert -c alarm Wake "Time to get up"
/usr/lib/alert -c alarm -t rise -r 5 -R Wake "Time to get up"   # tone, ring 5 min, rising
rtc                            # the alarm timed has set
```

In QEMU there is no alarm: `timed` says `no RTC alarm, next at …` instead.

## Where it writes

`/var/timers` is on the SD card's root file system. Each run of `timed` writes one small file,
`/var/timers/.last` (the time of the run, to know what was missed); a `once` timer is deleted
after it fired. Nothing is written to the internal flash.
