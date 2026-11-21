# Swap: idle programs on the SD card

The Flipper has little RAM, and every running program needs one contiguous piece of it. With
swap on, a program that sits and waits (a shell waiting for the command it started, a daemon
sleeping between jobs, the launcher while an app runs) can give its RAM back: the system copies
its memory to the swap partition of the SD card and frees the RAM. When the program has
something to do again (its wait ends, a signal arrives), its memory comes back to the same
place before it runs. The program never notices.

Swap is **off** by default. It uses only the swap partition `/dev/hd3` that the first boot made
on the card (partition type 82). On a card below 8 GB it is 64 MB, right after `/usr`; on an 8 GB
or larger card it is at the end of the card and takes half of the space after `/usr`, at most
4 GiB − 1 MiB (`mkpart -l /dev/hd0` shows it). It never writes to the Flipper's internal flash.
While `setup -H` converts `/home` ([The SD card](sdcard.md)), the partition holds the copy of
`/home` and swap stays off until the conversion is done.

The same partition can also hold apps' **memory blocks** (`blk=on`): see
[Memory blocks](blocks.md).

## Turning it on

In `/etc/rc.conf` on the card:

```
swap=plain
```

and reboot. `/etc/rc` then runs `swapon /dev/hd3 plain`. The choices are:

| `swap=` | Meaning |
|---|---|
| `off` | no swap (the default) |
| `plain` | swap without encryption |
| `crypt` | encrypted swap: **(planned)**, spec 12b. Until then `/etc/rc` prints a warning and leaves swap off |

**What `plain` means.** The memory of swapped programs is on the card unencrypted. Someone
who takes the card can read it. Each swapped image carries a checksum (CRC-32). A program whose
image comes back damaged (a bad card, or the image changed on the card) is ended with `SIGKILL`
and never runs with wrong memory. Programs that hold passwords or keys are **never** swapped in
plain mode:

- set-uid and set-gid programs (`passwd`, `su`);
- `login`;
- programs from packages marked `Secret: yes`.

Their memory stays in RAM. App authors mark such programs with `elf2mnx --secret` (the
`SECRETPROGS` list in `flipper/minix.mk`), and `mkpkg` then writes `Secret: yes`.

## When programs are swapped

- **When a program cannot start.** If starting a program (or a `fork`) does not find enough
  free RAM, programs that have waited for at least one second are swapped out. The ones that
  free the largest piece go first. Then the start is tried again.
- **To keep a reserve.** When free RAM falls below the reserve (16 KB by default), programs that
  have waited for at least 10 seconds are swapped out in the background. This happens only while
  nothing else wants the CPU and no key was pressed in the last second. It writes at most
  256 KB a minute, in 4 KB pieces. Change the reserve with `swap_reserve=` in `/etc/rc.conf`, or
  `swapon -r KB` (0 turns it off; at most 192).

The app on the screen, and the programs it started, are never swapped. Only programs whose code
runs from flash (the installed programs, or ones already in the text cache) are swapped. In this
version only programs that wait for the system in `wait`, `pause` or `sigsuspend` are swapped.
Programs that wait for a key, the console or a pipe stay in RAM.

Without an MMU a swapped program must come back to exactly the same addresses. So the app on
the screen is placed at the bottom of RAM and everything else at the top. A swapped program then
usually finds its place free. If it doesn't yet, its wake-up waits until the memory is free.

## Swap and the card's wear

Flash memory wears out with writing, and an SD card spreads its writes over its free blocks
(wear levelling). Swap writes as little as it can:

- **Only what changed.** The card keeps a checksum of every 4 KB piece of a swapped program.
  When the same program is swapped out again, only the pieces that changed since are written,
  and nothing at all if nothing changed. **A parked app that sleeps costs the card nothing**:
  its next swap-out after a wake that changed nothing writes 0 bytes.
- **Not what will wake soon.** A program that came back from the card because its own timer or
  a signal woke it is left in RAM by the reserve until it has waited for 2 minutes (instead of
  10 seconds), once. A parked app with a 1-minute timer therefore stays in RAM. When RAM is
  really needed (a program cannot start), it is swapped out at once as before.
- **In order.** The header first, then the pieces in ascending order: the card sees one
  sequential write.
- **Aligned partitions.** The first boot starts every partition on a 4 MiB boundary, so the
  swap slots lie in as few of the card's erase units as possible (new cards only; an existing
  card keeps its layout).
- **An empty card is told so.** The first boot runs `erase` over `/usr`, swap and `/home`
  before it makes the file systems (see [Installation](install.md), first boot).

### Which card

Typical use writes about 1 MB a day to swap, which no card minds. Prefer a branded card; a
"High Endurance" or industrial card (the kind sold for dash cams) lasts longest. Avoid unbranded
cards and cards that were full for a long time before, unless the first boot erased them.

### erase

```
# erase /dev/hd3
```

`erase /dev/hdN` (root only) tells the card that a whole partition is empty (SD ERASE). Its data
is gone; run `mkfs` after it (or `swapon` for the swap partition). It refuses the whole card
(`/dev/hd0`), a mounted partition and the swap partition while swap is on (`EBUSY`). It is
meant for setting up a card, never for normal use: an erase can take seconds. **(planned on the
Flipper, T18b)**: until the SD driver can erase, it says "this card or driver cannot erase" and
exits with status 2. See `man 8 erase`.

## Looking at it

```
# free -v
...
swap: plain, 2 of 16 slots used, 41 KB freed, reserve 16 KB
  swapped: pid    23 sh        17408 bytes
  swapped: pid     9 update    24064 bytes
# swapon
swap: plain, device 3/3, reserve 16 KB, 2 of 16 slots used, 14 out, 12 in
  swapped: pid    23 sh        17408 bytes (slot 4)
  swapped: pid     9 update    24064 bytes (slot 3)
```

`free -t on` also prints a line for every swap-out and swap-in on the console.

`swapon off` turns swap off when nothing is swapped. `swapon` refuses everything that is not a
primary partition of type 82 on the SD card (`EINVAL`), such as the root file system, `/dev/rom`
or a RAM disk. Only root may change the swap settings.

## Pulling the card out

Nothing swapped survives a reboot. If the card is removed or fails while programs are swapped,
swap turns off at once. Every swapped program is ended with `SIGKILL` and the console says, for
example, `swap: sh (pid 23) lost: SD card removed`. Programs in RAM go on, as far as they do
not need the card. Detecting the removal from the card-detect line, rather than from the
first failed transfer, is **(planned)**.
