# Reminders and the alarm clock (`remind`, `alarm`)

Two apps on top of [timers and alerts](timers.md) (spec 7b.1, 7b.2). Both keep nothing in RAM
while they are closed: each reminder or alarm is a timer file in `/var/timers`, `timed` fires it
and `alert` rings. You only open an app to change your reminders or alarms, see what you have
done today, or use the desk clock.

## Installing

```
pkg install /lib/pkg/timed_1.6.tar     # needed by both
pkg install /lib/pkg/remind_1.1.tar
pkg install /lib/pkg/alarm_1.4.tar
```

Each package adds its own user (`remind`, uid 21; `alarm`, uid 22; both in group `timers`), a line
on the launcher (**Kanpaia**, "bell" in Basque, for the reminders, and **Ordua**, "the hour",
for the alarm clock) and its data directory
`/home/flipper/apps_data/remind` or `…/alarm` (mode 700, the app's user). The programs go into
the internal flash at install time (`Flash: yes`), so their code is not copied into RAM.
`pkg remove` takes the app off the launcher; your timers, your data and the user stay, and
installing the package again brings everything back.

Kanpaia and Ordua are [window apps](launcher.md) (spec K.8, `Type: window`): their launcher lines
are `Kanpaia:remind:@bell:=/usr/bin/remind` and `Ordua:alarm:@alarm:=/usr/bin/alarm`, so the
launcher starts each directly as its own user and draws its windows itself. An app can only touch
its own timers and files. Installing `remind` 1.1 or `alarm` 1.1 over 1.0 turns the old launcher
line into the new one.

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
| Melody | the built-in `beep`, `chime`, `rise`, `classic`, `gentle`, `pips`, and the three **1980s digital-watch** tunes `watch` (a rising arpeggio, as an hourly chime), `hourly` (double chirps) and `jingle` (a short square-wave tune), and four **pocket-game** blips, `catch` (rising blips), `eggs` (the egg-catching rhythm), `game` (the game-over slide) and `duel` (a fast two-tone race), then short arrangements of old tunes, `marsel` (La Marseillaise), `saints` (When the Saints Go Marching In), `ode` (Ode to Joy), `elise` (Für Elise), `troll` (In the Hall of the Mountain King) and `korob` (Korobeiniki), `spy`, an original spy-film style riff, and `chirp`, fast chirps up and down at 2 to 4 kHz, where the piezo is loudest (the most annoying one: for heavy sleepers), then your own tone file (below) |
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

How much of a tune rings: an alarm plays up to 15 seconds of it at each ring (and at most 64
notes), a reminder or a notice up to 3 seconds; the rest is cut. The piezo is loudest between
about 1 and 4 kHz: write a tune in octaves 6 and 7 (`c6` is 1047 Hz); below 500 Hz it is
hardly heard.

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

- **RAM while open** (`free -v` before and after): Kanpaia **4.0 KB** (it was 15.6 KB as a
  full-screen app), Ordua **4.3 KB** (it was 16.1 KB; ACK8.3: at most 4.5 KB): no GUI
  library, screen buffer or thread, only the form, the time-zone data of `localtime` and the
  stack. Both take nothing while closed.
- The desk clock's "next alarm" line is the one from when it opened: an alarm that rings or is
  snoozed meanwhile does not change it until you open the desk clock again.
- The desk clock no longer sets the backlight itself (the old Left/Right `auto`/`dim`/`on`);
  a `Light:` line left in `apps_data/alarm/settings` is ignored.

## Where they write

Everything is on the SD card: the timer files in `/var/timers`, the reminder log (one ~20-byte line
per OK) and off alarms in `/home/flipper/apps_data/<app>/`.
Nothing is written to the internal flash at run time; the programs are written there once, by
`pkg install`.

### Sound files as alarm tones (coming)

After the watch melodies, an alarm will be able to play a WAV file from the SD card through the beeper: plain PCM, 8 or 16 bit, mono or stereo, 8 kHz is plenty (the piezo has no deep bass anyway). Short files only, a few seconds: a *kaixo* or a rooster. The file side is ready; the sound output arrives with the PCM driver.
