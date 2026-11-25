# Installing Galtzagorriak MINIX on a Flipper Zero

This chapter of the manual explains how to put Galtzagorriak MINIX on a Flipper Zero as it
works **today** (main branch, version 0.1.x), how to update it, and how to go back to the stock
firmware. Steps marked **(planned)** do not exist yet; they are listed so you know what is coming.

Galtzagorriak MINIX **replaces** the Flipper firmware. It does not run next to it. The stock
apps (Sub-GHz, NFC, RFID, iButton, Bluetooth, the mobile app link) are not available while MINIX
is installed. You can always return to the stock firmware (section 8).

## 1. What you need

Hardware:

- A Flipper Zero.
- A microSD card of **4 GB or more** (the card that came with the Flipper is fine). The card
  is **erased**. MINIX uses at most the first 4 GB of it; a bigger card works, but the space
  after 4 GB stays unused (MINIX 2.0 addresses disk partitions with 32-bit byte offsets).
- A USB-C data cable and a PC.
- A microSD card reader for the PC.

PC, tested on Linux (Debian/Ubuntu). macOS should work the same way but is untested. On
Windows, use WSL or a Linux machine: `flsend` (section 7) needs a POSIX serial port.

| Package (Debian/Ubuntu) | Needed for |
|---|---|
| `gcc-arm-none-eabi`, `make`, `git` | building the system (not needed if you download the images) |
| `python3`, `python3-pil`, `xfonts-base` | the build tools; Pillow and the X11 6x10 font are needed when the build redraws the boot screen, which a fresh clone can trigger |
| `dfu-util` | writing the firmware into the Flipper's flash |
| a serial terminal (`screen`, `picocom`, `minicom`) | logging in over USB |
| `qemu-system-arm` (optional) | trying the system on the PC without a Flipper |

    sudo apt install gcc-arm-none-eabi make git python3 python3-pil xfonts-base \
                     dfu-util screen qemu-system-arm

Your user must be allowed to open the Flipper's serial port (`/dev/ttyACM0`): add it to the
group `dialout` (`sudo usermod -aG dialout $USER`, then log in again). `dfu-util` needs access
to the USB device 0483:df11 (ST's DFU loader); if you have no udev rule for it (qFlipper installs
one), run the flash command with `sudo`.

## 2. Getting the images

### Downloading the images

GitHub builds the images for every change (workflow `build`, `.github/workflows/build.yml`):

- **A release** (tag `vX.Y.Z`): open *Releases* on the GitHub page of the project; the release
  has the files of the table below attached, with `SHA256SUMS`. Releases made before the
  workflow existed (v0.1.0) have no files.
- **The newest main branch**: open *Actions* → *build*, choose the newest green run on `main`,
  and download the artifact `galtzagorriak-flipper-...` at the bottom of the page (a zip; you
  must be logged in to GitHub). Artifacts are kept for 30 days.

Check the files after unpacking: `sha256sum -c SHA256SUMS`.

### Building them yourself

    git clone https://github.com/arielkonopka/Galtzagorriak-MINIX.git
    cd Galtzagorriak-MINIX/flipper
    make BOARD=flipper minix card base-flash base

This takes a minute or two and writes, under `build/flipper/minix/` (the downloads hold the
same files, without the directories):

| File | Size (approx.) | What it is |
|---|---|---|
| `card.img` | 2 MB | SD card image: partition table + the 32 MB root file system |
| `minix-base.bin` | 390 KB | **the firmware to flash**: kernel and servers, the shared C library, the programs used at boot (shell, launcher, login, ls, ...) and the default settings (`/rom`) |
| `minix.bin` | 210 KB | the kernel, the servers and the shared C library only, without the programs in flash and without `/rom`; for developers |
| `pkgs/base_X.Y.tar` | 470 KB | the system programs as a package, for updating a card that is already in use (section 7) |
| `pkgs/hello_1.0.tar` | 10 KB | an example app package |

Always use `card.img`, `minix-base.bin` and `base_X.Y.tar` **from the same build**. The
programs on the card use the C library in the flashed image (one copy for all of them), and they
run only with the library of their own build: with another one every program, the shell
included, fails to start ("cannot execute"). `pkg` refuses such a package ("pkg: base needs
shared libc ..., this system has ..."; the same for the GUI apps' library, "needs shared
libfuri ..."), and `free -v` shows both libraries (`shared libc:`, `shared libfuri:`) and their ids.

**A download and your own build are two different builds**, even from the same commit: the
library's id is a checksum of the compiled code, so it changes with the compiler. The images on
GitHub are built with Ubuntu 24.04's GCC; a build on your computer (or on another computer with
another GCC version) has another id. Pick one source and stay with it. Switching sources means
writing a new card, which erases `/home`: back it up first (section 7, *Backing up your files*).

**(planned)** Signed release images (spec R17.2) and a stock-updater package (`.fuf`) that the
stock firmware or qFlipper can install from the SD card without `dfu-util` (spec R17.3).

### Trying it without a Flipper

The same system runs in QEMU (a Cortex-M4 board), with the SD card as a file:

    make minix disk        # BOARD=qemu is the default
    make minix-run         # set root's password at the first boot; the session ends after 60 s

## 3. Before you start: back up the stock SD card

The stock firmware keeps your data on the SD card: saved Sub-GHz, NFC, RFID, IR and iButton
files, BadUSB scripts, app data, and in `/ext/.int` its internal settings. Writing the MINIX card
image **erases all of it**. Copy the whole card to the PC first (for example with qFlipper's
file manager, or by copying the card's files in a card reader), or use a second card for MINIX.

Nothing else from the stock firmware is lost: MINIX never changes the option bytes, never touches
the Bluetooth radio stack (CPU2), and leaves the OTP area alone.

## 4. Writing the SD card

Put the card into the PC's card reader and find its device name (`lsblk`; for example `/dev/sdb`
or `/dev/mmcblk0`). **Check it twice: `dd` overwrites whatever device you give it.** Unmount the
card if the desktop mounted it, then write the image to the **whole card**, not to a partition:

    sudo dd if=build/flipper/minix/card.img of=/dev/sdX bs=1M conv=fsync

Put the card back into the Flipper.

## 5. Flashing the firmware

1. Connect the Flipper to the PC by USB.
2. Put it into **DFU mode**: hold **Left + Back** until the Flipper resets, then release **Back**
   and keep holding **Left**. On the PC, `dfu-util -l` now lists ST's DFU loader (USB id
   0483:df11). This works from the stock firmware and from any MINIX image.
3. Write the firmware:

       dfu-util -a 0 -s 0x08000000:leave -D build/flipper/minix/minix-base.bin

   (`make BOARD=flipper minix-flash` does the same with the bare `minix.bin`.)
   When `dfu-util` finishes, the Flipper starts MINIX by itself. `dfu-util` may end with
   "Error during download get_status" (exit code 74) even though the flash worked: the chip
   restarts before it answers the last request. If `File downloaded successfully` is printed
   above it, all is well.

If the Flipper does not reach DFU mode with Left + Back (for example a broken image that crashes
very early), use the hardware combination: hold **Left + Back + OK**, release **Back** and
**Left**, keep holding **OK**.

## 6. First boot

The LCD shows the Galtzagorriak boot screen and the boot messages. On a fresh card,
`/etc/setup` then prepares the rest of the card. This happens once and takes a minute or two:

1. It adds three partitions after the root file system, each starting on a 4 MiB boundary
   (the card's usual erase unit), and **reboots by itself**:

   | Partition | Mounted on | Size | Use |
   |---|---|---|---|
   | `/dev/hd1` | `/` | 32 MB | the system, `/etc` settings |
   | `/dev/hd2` | `/usr` | 2 GB | programs and packages |
   | `/dev/hd3` | (swap) | see below | swap for idle programs, off by default ([Swap](swap.md)) |
   | `/dev/hd4` | `/home` | the rest | your files, MINIX V3 |

   On a card below 8 GB swap is 64 MB, right after `/usr`, and `/home` takes the rest. On an
   8 GB or larger card swap sits **at the end of the card** and takes half of the space after
   `/usr`, at most 4 GiB − 1 MiB; `/home` lies between `/usr` and swap. `/home` is a MINIX V3
   file system (names up to 60 characters, larger than 4 GB), and its block map already has
   room up to the end of the card, so it can later take swap's space too (`setup -H`, see
   [The SD card](sdcard.md#converting-home-to-minix-v3-setup--h)). Before this version `/home`
   was MINIX V2, at most 4 GB.

   On a card of **32 GB or more** swap is larger: `/dev/hd3` at the end of the card holds four
   swap subpartitions `/dev/hd3a` to `/dev/hd3d` of 4 GiB − 1 MiB each (about 16 GiB), and
   `/home` again takes all the space between `/usr` and swap: on a 128 GB card (about 117 GiB) about 99 GiB.
   Programs swap to `/dev/hd3a`; the [memory blocks](blocks.md) of apps use all four.

2. After the reboot it shows "Preparing the card..." and tells the card that `/usr`, swap and
   `/home` are empty (`erase`, see [Swap](swap.md)): a card that was full before, from a camera
   or a phone, then has its whole free pool for wear levelling. **(planned on the Flipper,
   T18b)**: until the SD driver can erase, setup says "this card or driver cannot erase, going
   on" and goes on. Then it makes the file systems on `/usr` and `/home` and continues booting.
   Then `/etc/rc` binds the programs in flash (`pkg bind`, about 2 s the first time) and starts
   the launcher. The boot never waits for the console, so a Flipper with no USB host comes up as
   usual.
3. Root has no password on a new card, so the console asks for one **before its first login
   prompt**: connect USB, open the serial port (see *Logging in* below), and you see
   "root has no password yet. Press Enter to set one." Press Enter and type the password twice.
   It is kept in `/etc/shadow`, readable only by root; `/etc/passwd` holds `##root` in its
   place. Until it is set, `login` lets nobody in. The same happens after updating an older card
   whose root had no password.

Do not unplug or reset the Flipper during these steps. If one fails, the boot continues without
`/usr` and `/home` and says so on the console; rewrite the card (section 4) and start again.

Once booted, the LCD shows the **launcher**: Up/Down select an app, OK starts it, Back leaves it.
An app that does not react to Back can still be closed: **hold Back for a second**. The app is
asked to end (SIGTERM), and stopped for good 3 seconds later if it has not. While an app runs it
owns the screen and the buttons; other programs cannot draw over it.
The launcher is a small desktop with three pages side by side, SETTINGS, APPS and HOME;
a short Left or Right moves between them. [The launcher](launcher.md) describes it in full.
Among the apps is *Console* (the system console on the LCD, 31x10 characters in a frame; it shows
what the USB console prints from the moment you open it, and says "USB console: waiting for
text" until then). *About*, on the SETTINGS page, shows the system version (e.g. `Galtzagorriak 0.1.1`), the MINIX version it is
based on, the machine and ABI number, the host name and the uptime; `uname -a` and `ver` print
the same on the console. After 5 minutes without a key the backlight fades out; the next key only turns it on.

### Logging in

Connect the Flipper by USB and open its serial port; the speed setting does not matter for USB:

    screen /dev/ttyACM0

`picocom /dev/ttyACM0` and `minicom -D /dev/ttyACM0` work as well, with their default settings:
the console ends each line with CR LF itself (output post-processing `OPOST`/`ONLCR`, as on any
Unix terminal; `stty -opost` switches it off, full-screen programs and file transfer do so on
their own). Do not turn on "add carriage return" in the terminal program, or you get blank lines.

Press Enter, and log in as **root** with the password set at the first boot. `passwd` changes
it. A wrong password is refused after 2 seconds, to make guessing slow.

The keys are those of a Linux terminal: **Ctrl-C** stops the running command, **Backspace**
rubs out a character (whether the terminal sends DEL or Ctrl-H), **Ctrl-U** clears the line,
**Ctrl-D** at the start of a line ends the input, Ctrl-\\ quits with a core dump. (MINIX 2.0
used DEL to stop a command, which is what Backspace sends today; `stty erase ^H intr ^?` brings
that back for the session.)

To see on the Flipper when the computer talks to it (handy when a script drives it, or to tell a
slow command from a dead cable), let the status LED flash with the USB console's traffic:

    echo console cyan >/dev/led     # any colour; on until "echo off >/dev/led" or a reboot

Each burst of traffic, in either direction, gives a short flash; no traffic, no light. Add the
line to `/etc/rc.conf` to have it at every boot. Details: [Devices](devices.md), the status LED.

The serial pins 13 (TX) and 14 (RX) on the GPIO header also carry the console, at 115200 8N1.
Useful first commands: `ver` (version, build and the cause of the last reset), `df`, `free`,
`pkg list`.

**The clock keeps UTC.** Local time comes from the time zone in `/etc/TZ`, a POSIX rule; choose
it on the launcher under SETTINGS > Time (see [The launcher](launcher.md)), or write it yourself,
for example for Central Europe:

    # Europe/Warsaw
    CET-1CEST,M3.5.0,M10.5.0/3

INIT gives it as `TZ` to everything it starts, and `/etc/profile` reads it for login shells. A
`TZ` set in `~/.profile` still wins for that user. Without `/etc/TZ` the time shown is UTC.
`date` prints local time, `date -u` UTC. To set the clock (as root; or on SETTINGS > Time): `date MMDDYYhhmm` takes local time, `date -u MMDDYYhhmm` or a trailing `Z` UTC
(`date 0119380314Z`: 2038-01-19 03:14 UTC); `MMDDCCYYhhmmss` gives the century too. Programs
use 64-bit times, so nothing breaks in 2038; files keep times from 1970 to 2106-02-07.

In `/tmp` and `/usr/tmp` (sticky directories, mode 1777) everybody may create files, but only a
file's owner (or root) may delete or rename it.

**(planned)** A first-boot wizard on the LCD that asks for the root password, hostname, region,
date, time zone and location (spec Phase 18b). Until then the clock starts from whatever the RTC holds; after the stock firmware that is your **local** time,
but MINIX reads it as UTC: set it once with `date -u ...Z`.

## 7. Installing apps and updating the system

Packages (`name_version.tar`) are sent from the PC over the USB console:

1. Log in as root on the console and leave it at the shell prompt.
2. **Close the terminal program** on the PC (it must not hold the port).
3. On the PC: `flipper/tools/flsend build/flipper/minix/pkgs/hello_1.0.tar`
   (`-p /dev/ttyACMn` for another port). `flsend` types `pkg get` on the Flipper, sends each
   file with a CRC check and resends damaged ones. About 100 KB/s.

On the Flipper: `pkg list`, `pkg info NAME`, `pkg files NAME`, `pkg remove NAME`.

`pkg` receives into `/usr/var/pkg/in` and unpacks in `/usr/var/pkg/tmp`, both root's only (it
refuses to use them otherwise), and installs only the packages received in that run. It checks a
package before unpacking anything and refuses it, with the reason, if a member has an absolute
path, a `..` component, a name too long for `tar`, or is a link or device; package names and
`Depends:` names are 1–32 letters, digits and `._+-`. `pkg remove` deletes only the files the
package installed.

A package is installed only when its setup script (`postinst`) ran and ended without an error.
Otherwise `pkg` says why and exits 1: `pkg: NAME: postinst cannot start: REASON` (for example when
there is no room in RAM, even after waiting 10 s) or `pkg: NAME: postinst failed: exit N` (128 + the signal number if it was killed). Then
`pkg: NAME VERSION is not set up: pkg install FILE again`: the files are in place, and the same
command runs the script again. When RAM is short for a moment, `pkg` and the shell wait and try
again (1 s apart, up to 10 s) before they give up. While a subshell of yours runs in the
background (for example `( ... ) &`), the shell cannot wait this way: it says
`NAME: no memory to start it` at once; run the command again a moment later.

### Backing up your files

Your files live in `/home` on the SD card. Before anything that rewrites the card (a new card
image, switching between downloaded and self-built images, an ABI change), copy them off:

- **The whole card**, to restore it exactly as it was (any Linux PC with a card reader):

      lsblk -o NAME,SIZE,TRAN,MODEL       # find the card, e.g. sdb; never the system disk
      sudo dd if=/dev/sdX of=flipper-sd-$(date +%F).img bs=4M status=progress

- **Only the files**: Linux reads MINIX file systems. `/home` is the card's 4th partition:

      sudo mount -t minix -o ro /dev/sdX4 /mnt && cp -a /mnt ~/flipper-home && sudo umount /mnt

  (`/dev/mmcblk0p4` on a built-in reader.) Copy them back the same way after the new card's
  first boot (mount without `-o ro`), or send them over the console.

**(planned)** Files to and from a PC over USB with no card reader and no drivers (MTP, spec
R10.4), also from Windows and macOS.

### Updating to a newer build without erasing the card

Rewriting the card (section 4) always works but erases `/home`. To keep your files:

(**From 0.1.1 to 0.1.2 or later** the steps below do not work: that step changes the ABI, the
shared C library and the place of the installed programs at once. Back up `/home` as above,
write the new card image (section 4), flash the new `minix-base.bin` (section 5) and install
your app packages again. The release notes in `CHANGELOG.md` say when a release needs this.)

(Not across an ABI change: the new system cannot run the old programs and the old `pkg` refuses
the new package, "pkg: base needs ABI 2, this system is ABI 1". The step from ABI 1 to ABI 2
(64-bit time, base 3.68) needs a rewritten card and `minix-base.bin` of the same build; copy what
you need from `/home` first. The step from ABI 2 to ABI 3 (32-bit inode numbers, T27a) is the
same: rewrite the card. `ver` and the *About* app show the ABI.)

(Not either when the shared C library changed: `pkg` refuses the new package with "pkg: base needs
shared libc X, this system has Y". Then too rewrite the card and flash `minix-base.bin` of the
same build. Builds that do not touch the library keep its id, and the steps below work, but only
when they are made with the same compiler: a build from another computer (another GCC version) has
another id. If you flash first anyway, the Flipper starts without a console (`init: console: Exec
format error`) until you rewrite the card.)

(Since the shared libfuri + libapp, issue #201, the programs installed in internal flash start
32 KB later, at `0x08048000`. Coming from an older build, flash `minix-base.bin` (not only
`minix.bin`) and install the app packages (`remind`, `alarm`, `timed`) again: what was installed
before is not found at the new place.)


1. Build the new version (section 2).
2. Send the new system package: `flipper/tools/flsend build/flipper/minix/pkgs/base_X.Y.tar`.
   Its install script also updates `/etc/rc` and `/etc/profile` on older cards and says so.
3. In the console, as root: `reboot -d`. The Flipper restarts into DFU mode, no buttons needed.
4. Flash the new `minix-base.bin` as in section 5, step 3.

`reboot -d` also turns on *dev mode*: until the next normal `reboot` or `halt`, a watchdog reset
(a hang) goes back to DFU mode, so a broken new image returns there by itself. Without dev mode
a hang just reboots the Flipper, like the stock firmware.

### Updating with the update file

Every release comes with one signed file, `Galtzagorriak-X.Y.Z.update`, on its GitHub release page,
with a `.sha512` next to it. The file is signed with the project's release key (its public half is
`flipper/keys/release.pub`); the Flipper refuses a file signed with any other key. It holds the whole system from one build: the firmware for the
Flipper's flash, the base programs on the SD card, the official apps and the release notes.

> **Not finished yet.** The Flipper can already check the file and prepare the update. The step
> that writes the flash at the next start is `atea`, the new boot stage, and it is not on the
> Flipper yet (task T33b). Until it is, update with DFU as described above.

**The steps:**

1. Download `Galtzagorriak-X.Y.Z.update` from the release page.
2. Connect the Flipper over USB. In Windows Explorer it shows up as a portable device. Drag the
   file onto it. Any name ending in `.update` works.
3. The Flipper checks the file (about 1–2 seconds per MB) and shows a window: `System update`,
   `0.8.2 → 0.9.0`, the first lines of the release notes (for example
   `0.9.0: alarm snooze, "Egun on!" on the welcome screen`), and `OK install  Back later`.
   Nothing is installed until you press **OK** on the Flipper. A file on its own, an MTP host or a
   remote shell cannot start an update.
4. After OK the Flipper prepares the update and restarts. During the restart it shows a progress
   bar (about 10 seconds for a full release, less for a small one). **Do not take the battery out
   while the bar is showing.** If the power goes anyway, the next start carries on where it
   stopped.
5. The new system starts. On its first start it switches the base programs on the card (one line
   `gztree: ...` per file on the console) and starts as usual. Your files in `/home`, your
   settings and your alarms stay.

From the console you can do the same as root:

```
gzupdate check /home/flipper/Galtzagorriak.update    # only check, change nothing
gzupdate apply /home/flipper/Galtzagorriak.update    # check, ask, prepare, restart
```

**What the messages mean** (`gzupdate: ...`, one line, and nothing is changed):

| Message | Meaning |
|---|---|
| `not signed by a trusted key` | The file was not signed by the project's key or your own key. It may come from somewhere else. |
| `bad signature`, `... damaged (hash mismatch)`, `padding not zero`, `truncated file` | The file was changed or did not copy completely. Copy it again and check it against the `.sha512` file. |
| `for another board (...)` | The file is for another device (for example the QEMU build). |
| `already installed` | This release is already running. |
| `older than the running system (...)` | This is an older release. See "Going back" below. |
| `needs at least X.Y.Z (running ...)` | The release is too old to update from this file. First install release X.Y.Z, or use DFU. |
| `not an update file` | The file is something else. |

**Going back to an older release.** An older signed file is installed only after you confirm it on
the Flipper itself: the screen shows `0.9.0 → 0.8.2 (older). OK: yes, Back: no`. A program cannot
do this without your button press. If a new release does not start three times in a row, the
Flipper asks `0.9.0 did not start. Go back to 0.8.2? OK / Back` and, on OK, installs the update file
it came from (kept as `/usr/.gzupdate/prev.update`). Both questions come from `atea` (T33b).

**When DFU is still needed:** the first time you install a build that has `atea` (it moves the
system up by 16 KB in flash, so the whole `minix-base.bin` has to be written once, and the apps
installed again); when the project's key changes; when the card is broken or missing (the Flipper
then says `Insert the SD card`, or `Recovery: connect to a PC (DFU)`).

**If you build the system yourself:** make a key pair once with `flipper/tools/gzsign --new-key`.
It writes `~/.config/galtzagorriak/update.key` (private: keep it safe, never commit it or copy it
to the Flipper) and `update.pub`. `make BOARD=flipper update` then builds
`build/flipper/minix/Galtzagorriak.update`, signs it with your key and checks it. Your build
trusts your `update.pub` as well as the project's key, so your Flipper accepts your own files. To
sign in another place, pass the private key with `--key FILE` or the variable `GZ_UPDATE_KEY`.
Without a key `make update` stops and says how to make one.

## 8. Recovery and going back to the stock firmware

| Problem | What to do |
|---|---|
| MINIX hangs | The watchdog resets the Flipper after 2 s. Or hold Left + Back for a hardware reset. |
| A new image does not boot | Left + Back, keep Left: DFU mode; flash a working `minix-base.bin`. If that does not work: Left + Back + OK, keep OK. |
| `/usr` or `/home` not mounted, or the card is damaged | Rewrite the card (section 4). This erases your files on it. |
| Forgotten root password | Rewrite the card (the password lives in `/etc/shadow` on the card). |

**Back to the stock firmware:**

1. Put the Flipper into DFU mode (Left + Back, keep Left).
2. In **qFlipper**, choose **Repair**. It installs the current stock firmware (and the radio stack
   if needed).
3. The stock firmware cannot read the MINIX card. Format it (Settings → Storage → Format SD Card
   on the Flipper, or FAT32/exFAT on the PC) and copy your backup back (section 3).
4. The stock clock may now show UTC instead of local time until you set it again (for example by
   connecting the mobile app).

## 9. What works and known limitations (today)

What works on the Flipper (details in [Hardware devices](devices.md) and the other chapters):

- Screen, backlight with fade-out, RGB status LED, buttons, vibration, speaker.
- The launcher desktop with settings, reminders and an alarm clock, timers and alerts ([Timers and alerts](timers.md)).
- 125 kHz RFID: read, write T5577 cards and emulate, all 25 LF protocols (`rfid`).
- iButton (Dallas): read, write RW1990 blanks and emulate (`ikey`). Cyfral and Metakom: in QEMU only for now (device part pending).
- Infrared send and receive with the stock protocols (`ir`).
- Sub-GHz: receiving and tuning only so far (`/dev/subghz`, `/dev/subghzraw`).
- USB: the serial console, and the Flipper as a keyboard, mouse or media keys
  ([USB gadgets](usb-gadgets.md), Bad USB).
- GPIO header (`gpio`), the hardware self test ([The self test](selftest.md)).
- Power: deep sleep when idle (with no USB cable), real power-off with `halt` (hold **Back** or
  plug in USB to start again), battery readings in the *Power* app ([Power and battery](power.md)).

Not yet:

- Sub-GHz sending, NFC, Bluetooth, U2F and the other stock apps. They come in the later phases of
  the spec (`docs/spec/galtzagorriak-spec.md`).
- A new time zone reaches programs started after the change; open shells keep the old one until
  the next login (section 6).
- There is no on-screen keyboard yet; for typing use the USB console.
- `/home` ends at 4 GB of the card (see section 6).
- **(planned)** Connecting the official Flipper mobile app over Bluetooth (time sync, device
  info, files; spec R15.7).
- **(planned)** Installing MINIX on a second Flipper from a MINIX Flipper over Bluetooth, without a
  PC (spec Phase 21).
- **(planned)** Wi-Fi devboard support (spec Phase 22, deferred).
