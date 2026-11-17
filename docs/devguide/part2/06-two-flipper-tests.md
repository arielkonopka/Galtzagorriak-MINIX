# 6. Testing radios against a second Flipper

Many drivers can only be tested with a partner: a reader for an emulated tag, a sender for a
receiver. A second Flipper Zero running the stock-based **Unleashed** firmware is that partner.
It has a command line on its USB port, so one computer can drive both Flippers at once:
`flipper/tools/twoflip.py` makes one Flipper send or emulate, has the other receive or read,
compares the two and prints PASS or FAIL.

This chapter is written so that it can be followed on a computer that has never seen this
project: it covers the hardware, the physical setup, installing Python, finding the serial
ports and their permissions on Linux, macOS and Windows, running the tests and what to do
when they fail.

What is tested today (the cases marked *planned* are listed by the script as SKIP until MINIX
has the feature; they turn into real tests when it lands):

| Case | Direction | What happens |
|---|---|---|
| `rfid-u2m` | Unleashed → MINIX | Unleashed emulates an EM4100 tag, MINIX `rfid` reads it |
| `rfid-m2u` | MINIX → Unleashed | MINIX `rfid emulate`, Unleashed `rfid read normal` reads it |
| `rfid-proto-u2m` | Unleashed → MINIX | other 125 kHz protocols (only with `--rfid`, for T19 / issue #210) |
| `ir-u2m` | Unleashed → MINIX | Unleashed `ir tx`, MINIX `ir rx` decodes (NEC, NECext, Samsung32, RC5, RC6, SIRC) |
| `ir-m2u` | MINIX → Unleashed | MINIX `ir tx`, Unleashed `ir rx` decodes |
| `ibutton-u2m` | Unleashed → MINIX | Unleashed emulates a Dallas key on the 1-Wire line, MINIX reads `/dev/ibutton` |
| `subghz-u2m` | Unleashed → MINIX | Unleashed sends a 433.92 MHz remote code, MINIX `/dev/subghz` sees the level rise |
| `subghz-raw-u2m` | Unleashed → MINIX | the same, MINIX records the timings from `/dev/subghzraw` (INFO only, never fails) |
| `ibutton-m2u` | MINIX → Unleashed | *planned*: MINIX emulates a key (spec R6.7, later parts) |
| `subghz-m2u` | MINIX → Unleashed | *planned*: MINIX transmits (spec R8.1, later parts) |
| `nfc-u2m`, `nfc-m2u` | both | *planned*: no NFC driver on MINIX yet (spec Phase 14) |

A full run takes about two minutes.

## 6.1 What you need

- **The MINIX Flipper**, installed as in the [installation chapter](../../manual/install.md),
  with a build of the main branch, and its **root password**.
- **The Unleashed Flipper**: any Flipper Zero with Unleashed firmware **and its SD card**
  (see 6.2). The script was checked against Unleashed's source at commit `53818b1`
  (November 2026); later releases normally keep the same commands.
- **Two USB-C cables that carry data** (some phone-charging cables carry power only; if a
  Flipper does not show up as a serial port, try another cable).
- **One computer** with two free USB ports (a hub is fine) running Linux, macOS or Windows.
- **Two male-to-male jumper wires** ("Dupont" wires, 10–20 cm). The Flipper's GPIO header has
  sockets, so the wires need pins on both ends.
- **Something to hold the two Flippers back to back**: two rubber bands and a piece of thin
  cardboard (a cereal box, 1–3 mm), or a hardback book and adhesive tape.
- **A sheet of white paper** (for the infrared reflector).

No 3D printer and no soldering are needed.

## 6.2 Preparing the Unleashed Flipper

1. Install Unleashed if it is not on the Flipper yet: download the latest release from
   <https://github.com/DarkFlippers/unleashed-firmware/releases> (the `.tgz` file) and install
   it with **qFlipper** (<https://flipperzero.one/update>): connect the Flipper, choose
   "Install from file" and pick the `.tgz`. qFlipper also writes the firmware's resources to
   the SD card.
2. **The SD card must stay in.** In current Unleashed the `ir`, `rfid`, `ikey`, `subghz` and
   `nfc` console commands are plugins on the SD card. Without the card (or with a card whose
   resources are older than the firmware) they answer "command not found"; reinstall the
   firmware with qFlipper to fix it.
3. On the Flipper, leave every app and stay on the main screen (the dolphin). An open app holds
   its hardware, and the console then answers e.g. "INFRARED is busy". Apps that change the
   USB mode (Bad USB, U2F, USB keyboard/mouse) take the serial port away until they are closed.
4. Turn off **5 V on GPIO** if it is on (Unleashed: **GPIO → 5V on GPIO → OFF**). It is off by
   default.
5. Sub-GHz transmitting must be allowed at 433.92 MHz. It is by default in most regions. If
   the test says SKIP with "region settings", choose another frequency with `--freq` (one that
   is legal where you are) or check the region in the Unleashed settings.
6. Close **qFlipper** before running the tests: it keeps the serial port open, and then the
   script cannot use it.

## 6.3 Preparing the MINIX Flipper

1. Install or update it with a build of the main branch (see the
   [installation chapter](../../manual/install.md)). The tests use `rfid`, `ir`, `od`,
   `/dev/ibutton`, `/dev/subghz` and `/dev/subghzraw`.
2. Know its **root password** (set at the first boot). The script logs in by itself when the
   console shows `login:`; you can also log in by hand before (`screen`, PuTTY) and then
   **close that terminal program**, because only one program can hold the port.
3. Do not run the launcher's RFID, IR or iButton apps while testing: the device files are used
   by one program at a time.

## 6.4 The physical setup

### Back to back (125 kHz RFID, later NFC)

The 125 kHz RFID coil and the NFC antenna of a Flipper are both on its **back**. Laid back to
back, each Flipper's coil sits over the other's, which is what a reader and a tag need. The
distance must be **small and the same every time**, or a result depends on how the Flippers
happened to lie.

1. Take off silicone cases or covers (they add several millimetres).
2. Put the two Flippers back to back, screens facing outwards, **both top edges (the GPIO
   header) at the same end**, and line up their outlines exactly. Seen from the side:

   ```
        top edge (GPIO header, IR window) of both, same end
        ||
        v v
      +---------------------------+   <- Flipper A, screen facing up
      |                           |
      +===========================+   <- spacer: thin cardboard, 1-3 mm
      |                           |
      +---------------------------+   <- Flipper B, screen facing down
   ```

3. Put a **spacer** between them: one layer of thin cardboard (cereal box, 1–3 mm), cut to the
   size of a Flipper. Do not use anything thicker than about 5 mm: emulation reaches less far
   than reading, and a matchbox (15 mm) is too thick for it.
4. Hold the stack together with **two rubber bands**, one near each end. Keep the bands off the
   top edge (the GPIO header and the IR window) and off the USB-C ports.

**Alternative: a book.** Tape one Flipper, screen up, on the front cover of a hardback book,
near the edge, and the other one, screen down, right under it on the inside of the same cover,
so the cover (2–3 mm) is the spacer. Tape both firmly so they cannot slide; the edge of the
cover keeps them lined up.

Write down which spacer you used (for example "cereal box, 1 layer, ~1 mm"), and keep it for
all later runs, so results from different days compare.

### The 1-Wire (iButton) wires

The iButton contacts of a Flipper are wired to pin **17 (1W)** of its GPIO header. Two wires
make one 1-Wire line between the two Flippers:

- **pin 17 (1W)** of Flipper A ↔ **pin 17 (1W)** of Flipper B
- **pin 18 (GND)** of Flipper A ↔ **pin 18 (GND)** of Flipper B

The pin numbers and names are printed on the Flipper's case next to the header; 17 and 18 are
the last two at one end. The header, from pin 1 to 18:

```
  1   2   3   4   5   6   7   8   9   10   11  12  13  14  15  16  17  18
  5V  A7  A6  A4  B3  B2  C3  GND 3V3 SWC  GND SIO TX  RX  C1  C0  1W  GND
```

Because the Flippers lie back to back, pin 17 of one is at the opposite end from pin 17 of the
other: the wires cross over the top edge. That is expected. Push the pins fully into the
sockets.

**Never connect pin 1 (5V) or pin 9 (3V3) between the two Flippers**: two power outputs
connected together can damage both.

### Infrared

The IR window is on the top edge. In the back-to-back stack both windows point the same way,
not at each other. Fold the sheet of white paper into a "roof" and hold it (tape, or lean it
against something) 2–4 cm above the top edges of both Flippers: the IR light bounces off the
paper into the other receiver. Usually even the light scattered from a nearby wall or ceiling
is enough; the roof makes it reliable.

If IR still fails, take the stack apart for the IR cases only and lay the two Flippers on the
table with their top edges facing each other, 5–20 cm apart, then put them back for the rest.
Avoid direct sunlight on the windows.

### Sub-GHz

Nothing to set up: the antennas are inside, and a few centimetres is a very strong signal.
Keep other 433 MHz remotes away during the test. The test sends a short burst (a few
seconds, low power, in the 433 MHz band that remotes use); check that this is allowed where you
are.

### Connecting

Plug both Flippers into the computer with the data cables. Both must be switched on.

## 6.5 Preparing the computer

The script is one Python file, `flipper/tools/twoflip.py`, plus `twoflip_sim.py` next to it
for the self-test. It needs **Python 3.8 or newer**. **pyserial** is needed on Windows and for
`--ports`; on Linux and macOS the script also runs without it.

### Getting the files

With git (`git` must be installed; the repository may need you to be signed in to GitHub):

```
git clone https://github.com/arielkonopka/Galtzagorriak-MINIX
cd Galtzagorriak-MINIX/flipper/tools
```

Without git: open the two files `flipper/tools/twoflip.py` and `flipper/tools/twoflip_sim.py`
on GitHub, press **Download raw file** on each, and put both into the same folder.

### Linux

1. Python and pyserial (Debian, Ubuntu, Mint, Raspberry Pi OS):

   ```
   sudo apt install python3 python3-serial git
   ```

   Fedora: `sudo dnf install python3 python3-pyserial git`. Arch: `sudo pacman -S python
   python-pyserial git`.

2. **Permission to open the ports.** The Flippers appear as `/dev/ttyACM0` and `/dev/ttyACM1`.
   Add yourself to the group that owns them, then **log out and log in again** (or reboot):

   ```
   ls -l /dev/ttyACM*            # shows the group: dialout (Debian/Ubuntu/Fedora) or uucp (Arch)
   sudo usermod -aG dialout $USER    # or: uucp
   ```

3. **Keep ModemManager away from the Flippers.** On many desktops ModemManager opens every new
   USB serial port and sends modem commands to it for a few seconds, which garbles the
   consoles. Add a udev rule (it also makes the ports usable without the group, step 2):

   ```
   echo 'SUBSYSTEMS=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="5740", ENV{ID_MM_DEVICE_IGNORE}="1", MODE="0666"' \
       | sudo tee /etc/udev/rules.d/42-flipper.rules
   sudo udevadm control --reload-rules && sudo udevadm trigger
   ```

   Then unplug both Flippers and plug them in again.

4. Check: `ls /dev/serial/by-id/` shows two entries, one with `Flipper_Devices` (Unleashed) and
   one with `Galtzagorriak` (MINIX).

### macOS

1. Install Python 3 from <https://www.python.org/downloads/> (or `brew install python` with
   Homebrew), then pyserial:

   ```
   python3 -m pip install --user pyserial
   ```

2. No driver and no permissions are needed. The Flippers appear as
   `/dev/cu.usbmodem...` (Unleashed's name contains `flip_` and the Flipper's name). Use the
   `cu.` names, not `tty.`.

### Windows (10 and 11)

1. Install Python from <https://www.python.org/downloads/>. In the installer, tick **"Add
   python.exe to PATH"**. Then, in a Command Prompt (`cmd`):

   ```
   py -m pip install pyserial
   ```

2. Windows has the USB serial driver built in. The Flippers appear as COM ports (for example
   `COM5` and `COM6`): see **Device Manager → Ports (COM & LPT)**. If they show under "Other
   devices" with a warning sign instead, install qFlipper once (it brings the driver), then
   close it.
3. Run the script with `py` instead of `python3` in the commands below, for example
   `py twoflip.py --ports`.

## 6.6 Running the tests

All commands are run in the folder with `twoflip.py` (`flipper/tools` in a clone). On
Windows write `py` instead of `python3`.

1. **Self-test, no hardware.** It runs every case against two simulated Flippers and checks
   the script itself works on this computer:

   ```
   python3 twoflip.py --selftest
   ```

   The last line must be `selftest: ok (12 cases)`.

2. **Find the Flippers.**

   ```
   python3 twoflip.py --ports
   ```

   prints one line per port and who answered on it:

   ```
   /dev/ttyACM0   minix      Galtzagorriak MINIX on Flipper Zero [USB VID:PID=0483:5740 ...]
   /dev/ttyACM1   unleashed  Flipper Devices Inc. Flipper ... [USB VID:PID=0483:5740 ...]
   ```

   `login` instead of `minix` is fine (the script logs in). `None` means the port did not
   answer; `busy: ...` means another program holds it (qFlipper, screen, PuTTY, the Arduino
   IDE...). Both firmwares use the same USB id, so on Windows the description may be the same
   for both; the second column is what tells them apart.

3. **Run all cases.**

   ```
   python3 twoflip.py --log run.log
   ```

   It finds both Flippers by itself. If it does not, give the ports:
   `python3 twoflip.py -m /dev/ttyACM0 -u /dev/ttyACM1` (Windows: `-m COM5 -u COM6`; macOS:
   `-m /dev/cu.usbmodem... -u /dev/cu.usbmodemflip_...`).

   If the MINIX Flipper is at `login:`, the script asks for the root password. To avoid the
   question (for example when another program starts the script), set it in the environment:
   `TWOFLIP_PASSWORD=... python3 twoflip.py` (Windows: `set TWOFLIP_PASSWORD=...` first), or
   pass `--password`.

4. **Read the result.** One line per case:

   ```
   MINIX on /dev/ttyACM0, Unleashed on /dev/ttyACM1
   PASS  rfid-u2m        U>M  MINIX read EM4100 1A2B3C4D5E
   PASS  rfid-m2u        M>U  Unleashed read EM4100 1A2B3C4D5E
   SKIP  rfid-proto-u2m  U>M  no --rfid PROTO:HEX given
   PASS  ir-u2m          U>M  6 protocols
   FAIL  ir-m2u          M>U  RC6: nothing decoded
   PASS  ibutton-u2m     U>M  MINIX read ROM 01 A1 B2 C3 D4 E5 F6 8F
   SKIP  ibutton-m2u     M>U  MINIX cannot emulate an iButton key yet (...)
   PASS  subghz-u2m      U>M  quiet -96 dBm, during Unleashed tx -24 dBm
   INFO  subghz-raw-u2m  U>M  187 durations, 61 near TE/3TE (403/1209 us)
   ...
   ```

   - **PASS**: the receiving side got exactly what the other sent.
   - **FAIL**: it did not; the text says what was seen. The exit status of the script is the
     number of FAILs, so `0` means everything that can run passed.
   - **SKIP**: not run (planned feature, or not possible with these settings).
   - **INFO**: a measurement only, never a failure.

   `run.log` has everything both consoles said, with times. Attach it when reporting a FAIL.

### Useful options

| Option | Meaning |
|---|---|
| `CASE ...` | run only these cases, e.g. `python3 twoflip.py rfid-u2m rfid-m2u` |
| `--list` | list all cases, the planned ones too |
| `-r N` | run each case N times and report how many passed (e.g. `-r 10`): shows marginal distance or alignment as "7/10 passed" |
| `--em HEX` | the EM4100 number to use (10 hex digits; default `1A2B3C4D5E`) |
| `--ir P:A:C` | IR messages to send instead of the default set, hex address and command, repeatable: `--ir NEC:04:08 --ir RC5:01:10` |
| `--freq HZ` | Sub-GHz frequency (default `433920000`) |
| `--rfid P:HEX` | also test 125 kHz protocol P (see below), repeatable |
| `--log FILE` | full transcript of both consoles |
| `--password PW` | MINIX root password (else `TWOFLIP_PASSWORD`, else asked) |

### Other 125 kHz protocols (T19, issue #210)

`--rfid` makes Unleashed emulate any of its protocols and checks that MINIX's `rfid` names
the protocol and prints the same data:

```
python3 twoflip.py --rfid H10301:1A2B3C --rfid Indala26:1A2B3C4D rfid-proto-u2m
```

The data is hex, without spaces, exactly as many bytes as the protocol has. To see the names
and sizes, ask the Unleashed console (6.8) for an unknown protocol: `rfid emulate X 00` prints
the list ("H10301, 3 bytes long", ...). Until T19 lands, MINIX knows only EM4100, so these
FAIL; that is the gap list of #210 measured on hardware. "Radio Key" has a space in its name
and cannot be given on Unleashed's command line.

## 6.7 When something fails

| Symptom | What to do |
|---|---|
| `did not find both Flippers` | Run `--ports`. A missing port: other cable (data cable), other USB socket, is the Flipper on? `busy`: close qFlipper, screen, minicom, PuTTY, Arduino IDE. `None`: see the next lines. |
| `Permission denied` (Linux) | 6.5 Linux step 2 or the udev rule of step 3; log out and in after `usermod`. |
| `no '>: ' prompt from the Unleashed Flipper` | Go back to the Flipper's main screen (press Back until the dolphin shows); unplug and replug it; close Bad USB/U2F apps. |
| Unleashed says "command not found" in the log | Its SD card is missing or lacks the resources: put the card in, reinstall Unleashed with qFlipper (6.2). |
| `no root shell on the MINIX Flipper` | Wrong password, or a user other than root is logged in at the console (the script logs a `$` shell out and retries once). Log in by hand with a terminal program to check, then close it. |
| Random characters in the log right after plugging in (Linux) | ModemManager: 6.5 Linux step 3. |
| `rfid-u2m` / `rfid-m2u` FAIL | Line the Flippers up exactly, use a thinner spacer, take off cases, keep metal (laptop, table frame) a few cm away. Run `-r 10` to see whether it is marginal. Note which direction fails: emulation by MINIX (`m2u`) is the newer code. |
| `ibutton-u2m` FAIL "no ROM" | The wires: pin 17 to 17 and 18 to 18 (count from the printed numbers), pins pushed fully in. 5V on GPIO off on Unleashed. |
| `ir-...` FAIL "nothing decoded" | Paper roof above the top edges, or face the Flippers top edge to top edge (6.4). No sunlight or bright lamp on the windows. A wrong value (not "nothing") is a decoder bug: report it. |
| `subghz-u2m` SKIP "region" | Unleashed refuses to send there: use `--freq` with a frequency allowed in your region and band list, e.g. `--freq 868350000` in Europe. |
| `subghz-u2m` FAIL with a small rise | Another 433 MHz transmitter nearby during the quiet reading; repeat the case. |
| A case hangs for more than a minute | Press Ctrl-C, unplug and replug both Flippers, rerun with `--log` and report the log. |

## 6.8 Doing it by hand

Each case is just two console commands, so a single case can also be tried by hand with two
terminal windows (`screen /dev/ttyACM1` on Linux and macOS, PuTTY "Serial" on Windows; any
speed). The commands the script uses:

| | Unleashed console (`>: `) | MINIX shell (`# `) |
|---|---|---|
| RFID U→M | `rfid emulate EM4100 1A2B3C4D5E` (Ctrl-C ends) | `rfid -n 8` |
| RFID M→U | `rfid read normal` | `rfid emulate EM 1A 2B 3C 4D 5E` (Ctrl-C ends) |
| IR U→M | `ir tx NEC 04 08` | `ir rx -n 1` |
| IR M→U | `ir rx` (Ctrl-C ends) | `ir tx NEC 0x04 0x08` |
| iButton U→M | `ikey emulate Dallas 01A1B2C3D4E5F68F` (Ctrl-C ends) | `od -b /dev/ibutton` |
| Sub-GHz U→M | `subghz tx 74BADE 433920000 403 100 0` | `echo 433920000 > /dev/subghz`, `cat /dev/subghz`, `echo 0 > /dev/subghz` |
| Sub-GHz raw | (as above) | `ir rx -r -n 1 -d /dev/subghzraw` |

The Unleashed sources these were checked against: `applications/main/lfrfid/lfrfid_cli.c`,
`infrared/infrared_cli.c`, `ibutton/ibutton_cli.c`, `subghz/subghz_cli.c` and
`nfc/cli/` (commit `53818b1`). The prompt `>: ` comes from `lib/toolbox/cli/shell/`.

**Writing a blank T5577 tag** is not automated (it needs a blank tag in the stack): put the
tag on the MINIX Flipper, `rfid write EM 1A 2B 3C 4D 5E`, then hold it to the Unleashed one and
`rfid read normal`.

## 6.9 Reporting

Paste the result lines into the device-check issue (#109 for the backlog, #210 for the RFID
protocols) with: the date, the MINIX version (`ver` on the MINIX console), the Unleashed
version (**Settings → About** on the Flipper, or `info device` on its console), the spacer you
used and, for any FAIL, the `--log` file.

## 6.10 For developers: changing the script

- A case is a function `(ul, mx, opt) -> (status, detail)` in `twoflip.py`, listed in `CASES`.
  `ul.start(cmd)` / `ul.stop()` run a long Unleashed command (one that ends with Ctrl-C),
  `ul.run(cmd)` a short one; `mx.run(cmd)` runs a MINIX command and returns its exit status and
  output, `mx.start` / `mx.stop` the long ones. `wait(regex, secs)` waits for output.
- When MINIX gets a planned feature (iButton emulation, Sub-GHz transmit, NFC), replace the
  `None` in its `CASES` entry with a function and drop its `PLANNED` text.
- Teach the simulator (`twoflip_sim.py`) the new command and update its `EXPECT` table, and
  keep `python3 -I twoflip.py --selftest` passing.
