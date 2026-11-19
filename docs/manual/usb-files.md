# Files to a PC over USB (MTP)

Plug the Flipper into a Windows or Linux computer and its files show up the way a phone's do:
as a portable device, with folders you can drag files into and out of. Nothing is installed on
the computer: Windows 10/11 and Linux (GNOME Files, KDE Dolphin, anything on libmtp) have the
driver built in. This is MTP, the Media Transfer Protocol (spec R10.4).

| Part | What | State |
|---|---|---|
| `mtpd` | the MTP server: the protocol, the files, the checks | **done** (T29a), tested on the PC and in QEMU |
| The USB side | the MTP interface next to the USB console, `devd` starting `mtpd` | **(planned, T29b)**: until then the Flipper does not show up as a device |

## How it looks

**Windows:** in Explorer, under **This PC**, a device named after the Flipper (its host name)
appears with one storage, **SD card**. Open it like a folder: copy files in and out, make
folders, rename (F2) and delete. No driver, no `.inf`, no extra software.

**Linux:** GNOME Files and KDE Dolphin show the Flipper in the side bar (gvfs/KIO over libmtp);
`mtp-detect` and `jmtpfs` work too. The device's files are not a disk (`mount` is not used), so
nothing has to be "ejected"; unplug when the copy is done.

## What the computer sees

- One storage: **`/home/flipper`** on the SD card, the same tree as `/ext` (your apps' data,
  Sub-GHz files, Bad USB scripts, ...). Nothing outside it: not `/etc`, not `/bin`, not other
  users' homes.
- Folders and plain files only. Device files and FIFOs are not shown.
- Files you copy in belong to the user **flipper**, with flipper's `umask`; `mtpd` never runs as
  root (started as root, it takes the identity of the owner of `/home/flipper`).
- A file name the SD card cannot hold is refused with an error on the computer, never cut short.
  On the MINIX V2 file system that is **14 characters**; with the bigger `/home` of spec O
  (MINIX V3, **(planned, T27)**) **60 characters**, for example
  `galtzagorriak_etxeko_atea_irekitzeko_urruneko_agintea.sub`. Names with `/`, control
  characters, `.` or `..` are refused too.
- Files up to 4 GiB − 1 byte.
- Dates are sent in UTC (the Flipper's clock); the computer shows them in its own time zone.
- Free space: shown once the free-space counters of spec O.5 exist **(planned, T27)**; until then
  the storage reports "unknown" and Windows shows no bar.

The `/home` file system **stays mounted** while the cable is in: MTP works file by file, so apps,
the alarm and the shell keep running and see the files at once.

## An example: a Sub-GHz file

Copy `etxea.sub` (a Sub-GHz key file, made on the stock firmware or on another Flipper) from
the computer into the **SD card → subghz** folder. On the Flipper it is then
`/ext/subghz/etxea.sub`:

```
$ cat /ext/subghz/etxea.sub
Filetype: Flipper SubGhz Key File
Version: 1
Frequency: 433920000
...
```

The other way round works the same: a file you saved on the Flipper appears on the computer
within a few seconds (the Flipper tells it the storage changed, at most once every 2 s).

## Pulling the cable, cancelling

If the cable comes out (or you press Cancel on the computer) in the middle of a copy **to** the
Flipper, the half-written file is removed: you never find a cut-off file later. `mtpd` ends when
the cable goes and leaves no memory behind (about 7.4 KB while it runs, 0 otherwise).

## Turning it off

- Settings **USB files [on]** (spec K.7) turns the MTP interface off for people who never want
  it **(planned, T29b)**.
- While the desktop's PIN lock is on, the computer sees no storage, and it appears when you
  unlock, without replugging **(planned: the PIN lock does not exist yet)**.

## For developers: `mtpd` without USB

`mtpd` speaks MTP on any byte stream, framed as described in `man 8 mtpd`. On the PC:

```
make -C flipper/mtpd test          # tools/mtphost.py against mtpd on a pipe
make -C flipper/mtpd fuzz          # the container parser under libFuzzer
```

In QEMU, `/dev/mtpline` (minor 21 of major 6) is the second serial line; `make INIT=m2 minix-run` puts
`tools/mtphost.py` on it and the M2 test checks the result.
