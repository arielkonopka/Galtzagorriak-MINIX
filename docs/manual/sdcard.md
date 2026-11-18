# The SD card: power loss and pulling the card out

Spec section N, tasks T17a and T17b. The card-detect checks on the Flipper itself (pulling the
card during a copy, the check on a full card) are still being measured (T17b).

The whole file system (`/`, `/usr`, `/home`) lives on the microSD card. MINIX keeps recent
changes in memory and writes them to the card every 30 seconds (`update`), or at once with
`sync`. There is no journal. Instead each file system has a **clean** mark:

- The mark is set when nothing has been written for about 30–60 seconds, and when the Flipper
  powers off properly (`halt`, the power menu, automatic power off, low battery).
- It is cleared just before the first new write.

So if the battery dies, the Flipper resets, or the card comes out **while nothing is being
written**, the card is fine and the next boot is as fast as always.

## When the card is checked at boot

If a file system was not clean, the next boot checks and repairs it with `fsck` before using it.
You see it on the Flipper's screen, under the boot messages (the line `reset: ...` above it
says why the Flipper last restarted), for example:

    fsck /dev/hd4: unclean
    fix 812: adjust
    fix: install a new map
    / zone map            100%
    fixed 2, left 0, 6.2 s

While it works, the line with the turning `| / - \` shows the step (`tree`, `zones`,
`zone map`, `links`, `inode map`, `inode list`) and how far it is; on a large file system the
zones are checked in parts, shown as `3/8`. Each repair is one short line. If anything was
repaired, the summary stays on the screen for a few seconds or until you press a button.

- The root file system (`/`) is checked first. If something was repaired there, the Flipper
  shows the summary and restarts once by itself.
- `/usr` and `/home` are checked before they are mounted. Hold **Back** to skip a check: that
  file system is then mounted read-only (you can read your files but not change them) and the
  next boot checks it again.
- If errors are left, the file system is mounted read-only and the launcher's title says "CARD
  HAS ERRORS: RUN FSCK" until a later boot checks it clean.
- What was found on `/usr` and `/home` is kept in `/usr/adm/fsck.log` (the last 400 lines).
- The first boot after updating to the version that brings this checks every file system once,
  because older cards have no clean mark yet.

A check takes a few seconds for `/`, and up to about 30 seconds for `/usr` and `/home` together
on a card with a few thousand files. `fsck_mem=64` in `/etc/rc.conf` sets how much memory (KB)
it may use (at most about 64); less memory means more passes and a slower check. `fsck` needs
about 90 KB of RAM: at boot that is free; to run it later yourself (`man fsck`), close the apps
first. Reading files does not change the card (no access-time writes), so `cat` or starting a
program never makes a file system unclean. Pipes and FIFOs (`ls | more`, the desktop's
`/var/run/gui`, the window apps) do not write to the card either: their data stays in memory.
A FIFO's times (`ls -l`) on the card are not updated by reading or writing it.

## Pulling the card out

If you take the card out while the Flipper runs, it stops using the card at once: the screen
shows a box **"SD card removed / Put it back"** over whatever was there, the console says the same,
and the Flipper buzzes briefly. Put the **same** card
back and the next file access continues ("sd: the card is back, carrying on"). Apps that need
files report an error meanwhile; apps running from internal flash keep running.
The changes not yet written stay in memory and go to the card when it is back: nothing is lost.
(The memory holds 18 blocks of 1 KB; if a program writes more than that while the card is out,
the oldest is lost and the console says `Lost block ...`.)

If you insert a **different** card, the Flipper never touches it: it shows "Different card / Put the original
back" (and on the console "sd: Different card: put the original back or power off"). It tells
the cards apart by their serial number (the CID). If you
power off instead, the original card is checked at its next boot if it was in the middle of a write.

## Does the card support DISCARD?

At boot the console shows the card's size, followed by `DISCARD` if the card supports it (SD 5.1
and newer: most cards made since 2016), for example:

    sd: 122000 MB, DISCARD

DISCARD lets the Flipper tell the card which areas hold nothing any more. The card then stops
copying that dead data around internally, which saves wear and keeps writes fast. Nothing is
erased by it and no data is written. The swap partition will use it **(planned, spec 12h,
R12.40)** for memory blocks that apps have given back. For programmers: `ioctl(fd, DIOCDISCARD,
&range)` on a partition device (root only), `struct part_range` in `<minix/partition.h>`
(first sector and count, from the partition's start); `ENOTTY` if the card has no DISCARD.

## Things that cannot save your data

- The **Left + Back** reset, the battery running completely flat without the low-battery power
  off, or a crash restart do not write what is in memory (`init` powers off in order when
  it gets `SIGPWR`, signal 16: it ends every program, writes everything and marks the card
  clean; the kernel sending it on low battery is **(planned, R6.1)**). Changes from the last 30 seconds may be
  lost; the card itself is repaired at the next boot.
- Programs that write the card very often keep it "not clean" all the time. If you write such a
  program (a logger, for example), collect the data in memory and write it every few minutes.

## Checking the card on a computer

The file systems are MINIX v2 file systems, which Linux understands. With the card in a reader
(here as `/dev/sdX`), unmounted:

    sudo fsck.minix -av /dev/sdX4      # /home; sdX1 is /, sdX2 is /usr

`fsck.minix` (from util-linux) uses the same clean mark as the Flipper.
