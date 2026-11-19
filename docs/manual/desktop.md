# Desktop and modules

The launcher is also the **desktop** (spec K.6): besides the app list it has pages for the
modules' tiles, the notifications and the running apps. They sit in a row, and Left/Right move
along it:

```
SETTINGS  <-  APPS  ->  HOME  ->  NOTES  ->  RUNNING
```

A page turns when you let go of a short Left or Right press, so the backlight gestures (Left+Up)
never turn a page. The status line (the black title bar) shows the battery, the page's name, the
time, the number of unread notifications (`*3`) and, on lists, your position (`2/8`). The bottom
line says what the buttons do on each page. The app list and SETTINGS are described in
[The launcher](launcher.md).

## HOME: the modules' tiles

A **module** is a small program, often a shell script, that puts a line (a *tile*) on HOME,
such as the battery or the next bus. Up/Down move the bar. With no module running HOME says
`no tiles`. When a module ends or is killed, its tile goes.

## NOTES: notifications

Apps tell you things with notifications: a reminder, a finished backup. Each one shows its text
on the bottom line for 2 seconds and is kept on the **NOTES** page:

- the last **8** notifications, the newest at the top, each with the time it came; a `*` before
  one you have not opened yet. A ninth pushes the oldest out;
- **OK** opens the one under the bar: who sent it, the time, its title and its whole text. In it,
  **OK** keeps it (back to the list) and **Back** clears it;
- **Back** on the list clears the notification under the bar;
- the status line counts the ones not opened yet (`*2`) on every page.

Notifications live only in the launcher's memory: nothing is written to the card or the flash,
and they are gone after a restart. Alarms are not listed here: they always ring through their
own alert screen, even if the launcher is not running (see [Timers and alerts](timers.md)).
Reminders and info notifications are listed even during do-not-disturb, when their alert stays
quiet.

## RUNNING

This page will list the apps you parked with a long Back and let you end them (spec R12.20).
Parking is not there yet, so it says `nothing running`.

## Writing a module in shell

A module writes its lines with `guimod`:

```
guimod tile NAME TEXT ...      set or change the module's tile on HOME
guimod status NAME TEXT ...    a short text (6 characters) on the status line
guimod notify NAME TEXT ...    a notification on NOTES (23 characters on its row,
                               the whole text when it is opened)
guimod close NAME              its tile and status go
```

NAME is 1 to 10 characters. A module is your user plus its name, so it can change only its own
tiles; at most 16 tiles and statuses together. The example module
`/usr/share/examples/gui/battile.sh` (package `gmdemo`) is a battery tile in three lines:

```
IFS='= '
while read key soc rest </dev/power
do guimod tile battery "Battery $soc%"; sleep 60; done
```

Start it with `sh /usr/share/examples/gui/battile.sh &`. The tile stays while the shell running
the loop is alive; `kill` it and the tile goes. A notification from a script:

```
guimod notify backup "Backup done: 12 files"
```

`guimod` never waits: with no desktop, or while a full-screen app is open and the desktop's
FIFO is full, the record is dropped and `guimod` exits with status 1.

## Writing a module in C

A C program writes the same records, one `write()` each, to the FIFO `/var/run/gui` (group
`gui`: apps the launcher starts are in it). For notifications use `notify_post()` from `libapp`:
it puts info and reminder notifications on NOTES and starts the alert (sound, vibro, LED) as
your notification settings say:

```c
#include "app.h"

notify_post(TC_INFO, "Backup", "Backup done: 12 files", NULL);
```

For tiles, build the record with `dk_record()` (`flipper/gui/desktop.c`) and write it without
waiting:

```c
char rec[DK_LINE];
int n = dk_record(rec, sizeof rec, "tile", "clock", getuid(), getpid(), "12:30 lunch");
int fd = open(DK_PATH, O_WRONLY | O_NONBLOCK);

if (fd >= 0 && n > 0) write(fd, rec, n);
```

The record format, the rules and window apps are in the Development Guide, chapter 7
([Desktop modules and window apps](../devguide/part1/07-desktop-modules.md)).
