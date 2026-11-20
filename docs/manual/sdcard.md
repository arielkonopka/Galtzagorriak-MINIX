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
first. With many files on `/home` (more than about 10 000), the boot check keeps its counts on
the swap partition, which is not in use yet at that point; this needs no setting. After a crash
some space on `/home` can stay marked as used with no file in it (the boot check does not look
for it, to stay quick); `umount /home; fsck -S /dev/hd4; mount /dev/hd4 /home` gives it back. Reading files does not change the card (no access-time writes), so `cat` or starting a
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

Raw sectors (R12.39): `bread(fd, sector, buf, count)` and `bwrite(...)` (`DIOCPRW`) read and
write sectors anywhere in a partition, also past 4 GiB. A read needs `fd` open for reading, a
write needs it open for writing (else `EBADF`), and a write to a partition that is mounted is
refused (`EBUSY`). This check covers the partition device only: writing through `/dev/hd0`
(the whole card, root only) still reaches a mounted partition behind the file system's back,
as plain `write()` always could. Do not do that.

## Things that cannot save your data

- The **Left + Back** reset, the battery running completely flat without the low-battery power
  off, or a crash restart do not write what is in memory (`init` powers off in order when
  it gets `SIGPWR`, signal 16: it ends every program, writes everything and marks the card
  clean; the kernel sending it on low battery is **(planned, R6.1)**). Changes from the last 30 seconds may be
  lost; the card itself is repaired at the next boot.
- Programs that write the card very often keep it "not clean" all the time. If you write such a
  program (a logger, for example), collect the data in memory and write it every few minutes.

## Converting `/home` to MINIX V3 (`setup -H`)

A card set up before this version has a MINIX V2 `/home` (names up to 14 characters, at most
4 GB). As root,

    # sh /etc/setup -H

converts it to MINIX V3 and keeps your files. It first checks that `/home` is V2, that
`/dev/hd3` is the swap partition and that `/home`'s files fit on it (`du -s /home` plus 1 KB
per file and directory), makes `/home`'s partition reach up to the next partition after it,
and reboots. The rest runs at that boot, before swap is turned on, with these messages:

1. `setup -H: copying /home to the swap partition /dev/hd3`: a `tar` archive of `/home` on
   `/dev/hd3`, read back and compared with its checksum. If it differs, nothing was changed
   and `/home` stays V2.
2. `setup -H: making a MINIX V3 /home on /dev/hd4 and copying it back`: `mkfs -3` and the copy
   back from `/dev/hd3`.
3. `setup -H: checking every file`: every file's checksum (`cksum`) is compared with the list
   made in step 1.

Then it says `setup -H: /home is MINIX V3 now; every file is the same.` The step it is in is
kept in `/etc/.home-v3`: if the power goes off, the next boot goes on from that step. Until
the end, the copy on `/dev/hd3` is kept: swap stays off, and the boot check of `/home` does
not use `/dev/hd3` for its counts. If the check finds a file that differs, it says so and
leaves the copy there (`cd /home; tar xopf /dev/hd3` gets the files back by hand; remove
`/etc/.home-v3` afterwards to turn swap on again). Back up `/home` before you start anyway.

On a card where swap is right after `/usr` (cards below 8 GB, or any card set up before), the
partition after `/home` is none, so `/home` grows to the end of the card. There swap is only
64 MB, so `setup -H` converts a `/home` of up to about 60 MB; with more it says how much it
needs and changes nothing (copy the rest to a computer first).

## Checking the card on a computer

`/` and `/usr` are MINIX V2 file systems and `/home` is MINIX V3 (V2 on a card set up before;
see above); Linux understands both. With the card in a reader
(here as `/dev/sdX`), unmounted:

    sudo fsck.minix -av /dev/sdX4      # /home; sdX1 is /, sdX2 is /usr

A `/home` made as MINIX V3 on a computer (`mkfs.minix -3`, 1 KB blocks; names up to 60
characters) is checked and repaired by the Flipper's `fsck` too, at boot like the others, and
`fsck.minix -f` checks it on the computer. The Flipper can also make one itself: with `/home`
unmounted, `mkfs -3 /dev/hd4` makes an empty MINIX V3 file system on all of the partition, also
when it is larger than 4 GB (this erases everything on `/home`). The Flipper keeps its clean mark at byte 14 of the V3
super block. Linux does not know it and leaves it as it is, so after writing to a V3 `/home` on a
computer, check it there with `fsck.minix -f` before you put the card back.

`fsck.minix` (from util-linux) uses the same clean mark as the Flipper.

### Free space (`df`)

`df` answers at once on any size of card: the Flipper counts free blocks and files as it uses
them and does not read the whole map of a big `/home`. Right after the Flipper starts, or after
`mount`, the numbers come from a note the Flipper left on the card at its last clean stop, and
`df` shows them with a `~`. A minute or so later `update` has counted again, a few blocks at a
time while the Flipper is idle, and the `~` goes away. After Linux wrote to the card, the first
numbers can be wrong until then. `df` counts the data blocks of a file system (without the maps
and the inode table), so its "total" is a little smaller than the partition.

## Checking and repairing the card by hand

`fsck` cannot be started from the shell on a running system: the launcher and the shell leave no
memory hole big enough (`fsck: no memory to start it`). Ask for the check at the next start
instead (as root):

```
sh /etc/fsckboot
```

The Flipper restarts, checks and repairs `/usr` and `/home` before anything else runs (rainbow LED,
the progress on the console; a 113 GB card takes a minute or two), restarts again, and then
starts normally. Do not switch it off meanwhile. What was found is appended to
`/usr/adm/fsck.log`.
