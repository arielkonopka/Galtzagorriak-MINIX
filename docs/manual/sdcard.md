# The SD card: power loss and pulling the card out (planned)

**(planned, spec section N, task T17)** Everything below describes how it will work. Today a
power loss or a pulled card can leave the file systems damaged, and nothing checks them at boot.

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
You see it on the Flipper's screen, for example:

    fsck /dev/hd4 /home: unclean
    (watchdog)
    tree 64%
    zone 1234 not in map: fixed
    fixed 1, left 0, 6.2 s

- The root file system (`/`) is checked first. If something was repaired there, the Flipper
  shows the summary and restarts once by itself.
- `/usr` and `/home` are checked before they are mounted. Hold **Back** to skip a check: that
  file system is then mounted read-only (you can read your files but not change them) and the
  next boot checks it again.
- If errors are left, the file system is mounted read-only and the launcher says "Card has
  errors: run fsck".
- What was found is kept in `/usr/adm/fsck.log`.
- The first boot after updating to the version that brings this checks every file system once,
  because older cards have no clean mark yet.

A check takes a few seconds for `/`, and up to about 30 seconds for `/usr` and `/home` together
on a card with a few thousand files. `fsck_mem=64` in `/etc/rc.conf` sets how much memory (KB)
it may use; less memory means more passes and a slower check.

## Pulling the card out

If you take the card out while the Flipper runs, it stops writing at once and keeps the changes
in memory. The screen says **"SD card removed: put it back"**. Put the **same** card back and
everything continues with nothing lost. Apps that need files wait or report an error meanwhile.

If you insert a **different** card, the Flipper never writes to it: it says "Different card: put
the original back or power off". If you power off instead, the original card is checked at its
next boot if it was in the middle of a write.

## Things that cannot save your data

- The **Left + Back** reset, the battery running completely flat without the low-battery power
  off, or a crash restart do not write what is in memory. Changes from the last 30 seconds may be
  lost; the card itself is repaired at the next boot.
- Programs that write the card very often keep it "not clean" all the time. If you write such a
  program (a logger, for example), collect the data in memory and write it every few minutes.

## Checking the card on a computer

The file systems are MINIX v2 file systems, which Linux understands. With the card in a reader
(here as `/dev/sdX`), unmounted:

    sudo fsck.minix -av /dev/sdX4      # /home; sdX1 is /, sdX2 is /usr

`fsck.minix` (from util-linux) uses the same clean mark as the Flipper.
