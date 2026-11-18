# Swap: idle programs on the SD card

The Flipper has little RAM, and every running program needs one contiguous piece of it. With
swap on, a program that sits and waits (a shell waiting for the command it started, a daemon
sleeping between jobs, the launcher while an app runs) can give its RAM back: the system copies
its memory to the swap partition of the SD card and frees the RAM. When the program has
something to do again (its wait ends, a signal arrives), its memory comes back to the same
place before it runs. The program never notices.

Swap is **off** by default. It uses only the swap partition `/dev/hd3` that the first boot made
on the card (64 MB, partition type 82). It never writes to the Flipper's internal flash.

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
