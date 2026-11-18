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
   When `dfu-util` finishes, the Flipper starts MINIX by itself.

If the Flipper does not reach DFU mode with Left + Back (for example a broken image that crashes
very early), use the hardware combination: hold **Left + Back + OK**, release **Back** and
**Left**, keep holding **OK**.

## 6. First boot

The LCD shows the Galtzagorriak boot screen and the boot messages. On a fresh card,
`/etc/setup` then prepares the rest of the card. This happens once and takes a minute or two:

1. It adds three partitions after the root file system and **reboots by itself**:

   | Partition | Mounted on | Size | Use |
   |---|---|---|---|
   | `/dev/hd1` | `/` | 32 MB | the system, `/etc` settings |
   | `/dev/hd2` | `/usr` | 2 GB | programs and packages |
   | `/dev/hd3` | (swap) | 64 MB | swap for idle programs, off by default ([Swap](swap.md)) |
   | `/dev/hd4` | `/home` | the rest, up to 4 GB | your files |

2. After the reboot it makes the file systems on `/usr` and `/home` and continues booting.
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
Today it offers *Console* (the system console on the LCD, 31x10 characters in a frame; it shows
what the USB console prints from the moment you open it, and says "USB console: waiting for
text" until then), *About* and
*Lights*. *About* shows the system version (e.g. `Galtzagorriak 0.1.1`), the MINIX version it is
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

### Updating to a newer build without erasing the card

Rewriting the card (section 4) always works but erases `/home`. To keep your files:

(Not across an ABI change: the new system cannot run the old programs and the old `pkg` refuses
the new package, "pkg: base needs ABI 2, this system is ABI 1". The step from ABI 1 to ABI 2
(64-bit time, base 3.68) needs a rewritten card and `minix-base.bin` of the same build; copy what
you need from `/home` first. `ver` and the *About* app show the ABI.)

(Not either when the shared C library changed: `pkg` refuses the new package with "pkg: base needs
shared libc X, this system has Y". Then too rewrite the card and flash `minix-base.bin` of the
same build. Builds that do not touch the library keep its id, and the steps below work.)

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

## 9. Known limitations (today)

- Hardware that works: LCD, backlight, RGB LED, buttons, vibration, speaker (`beep`), GPIO
  header (`gpio`), infrared send and receive (`ir`), USB serial console, SD card.
- Not yet: Sub-GHz radio, NFC, 125 kHz RFID, iButton, Bluetooth, U2F, BadUSB and the other stock
  apps; the menu interface is a first launcher with three apps. These come in the later phases of
  the spec (`docs/spec/galtzagorriak-spec.md`).
- Power management is not done yet: the Flipper uses several mA even when idle, so the battery
  lasts much shorter than with the stock firmware, and `halt` does not switch it off. Real
  power-off and sleep come with spec tasks T14/T15. To see the current draw yourself, see
  [Power and battery](power.md).
- A new time zone reaches programs started after the change; open shells keep the old one until
  the next login (section 6).
- The system console is on USB and on the LCD *Console* app; there is no on-screen keyboard yet.
- **(planned)** Connecting the official Flipper mobile app over Bluetooth (time sync, device
  info, files; spec R15.7).
- **(planned)** Installing MINIX on a second Flipper from a MINIX Flipper over Bluetooth, without a
  PC (spec Phase 21).
- **(planned)** Wi-Fi devboard support (spec Phase 22, deferred).
