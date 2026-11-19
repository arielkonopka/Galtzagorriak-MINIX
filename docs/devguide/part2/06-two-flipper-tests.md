# 6. Testing radios against a second Flipper

Many drivers can only be tested with a partner: a reader for an emulated tag, a sender for a
receiver. A second Flipper Zero running the stock-based **Unleashed** firmware is that partner.
It has a command line on its USB port, so one computer can drive both Flippers at once.
`flipper/tools/twoflip.py` tries **everything the Unleashed Flipper can do with a partner**, in
both directions, and produces a **coverage map**: for each feature, does MINIX work, is it
broken, or is it not there yet.

This chapter is written so that it can be followed on a computer that has never seen this
project: it covers the hardware, the physical setup, installing Python, finding the serial
ports and their permissions on Linux, macOS and Windows, running the tests, reading the map and
what to do when something fails.

### What is covered

About 230 rows, each one direction of one feature. "U→M" (`u2m` in the row names): the
Unleashed Flipper sends or emulates, MINIX receives or reads. "M→U" (`m2u`): the other way.

| Area | Rows | What happens |
|---|---|---|
| `rfid` | all 25 Unleashed 125 kHz protocols × both directions | one emulates, the other reads; protocol and data must match |
| `rfid-write` | the same 25 × both | one writes a blank T5577, the other reads it (only with `--t5577`) |
| `ir` | all 14 protocols + raw timings × both | `ir tx` on one, `ir rx` on the other |
| `ibutton` | Dallas through `/dev/ibutton`; Dallas, Cyfral, Metakom × both; writing a Dallas key × both | one emulates on the 1-Wire line, the other reads (writing only with `--rw1990`) |
| `subghz` | signal level at 315, 433.92, 868.35 MHz; raw timings; Princeton `tx`; chat × both | Unleashed sends, MINIX measures; MINIX sends, Unleashed decodes |
| `subghz-decode` | 37 remote protocols × both | Unleashed's own test recordings (`.sub` key files) sent by one, decoded by the other |
| `nfc` | 7 tag types (ISO14443-3A, NTAG215, Ultralight 11/21/C, FeliCa, SLIX) × both | one emulates a tag file, the other scans it |
| `gmodem` | the data modems of spec section P: RFID `fast`/`robust`, Sub-GHz `narrow`/`std`/`wide`/`max`, NFC, BLE × both | **two MINIX Flippers** (`--peer-minix`, 6.12): one sends 1 KB with `gmodem send`, the other receives it; checksums must match. N/A against Unleashed |

Each row ends with one status:

| Status | Meaning |
|---|---|
| **PASS** | the other side got exactly what was sent |
| **FAIL** | MINIX has the feature, but the result is wrong (wrong data, wrong protocol, or the side it drove got nothing) |
| **MISSING** | MINIX does not have it yet: the command or option is not there, or MINIX got the signal but has no decoder for it |
| **BLOCKED** | the Unleashed Flipper could not do its part here (region settings forbid the frequency, its command line cannot express it); the reason is given |
| **N/A** | needs a blank tag or key that was not offered (`--t5577`, `--rw1990`), or a second MINIX Flipper (`--peer-minix`, the `gmodem` rows) |
| **INFO** | a measurement only |

Where MINIX does not have a feature yet, the row still runs: it uses the command the spec plans
for it, which mirrors the stock command line, and reports MISSING when the command is not there.
When the feature lands with that command, the row turns into PASS or FAIL by itself. The
commands MINIX is expected to have are listed in 6.10.

A full run takes about 15 minutes today (rows that MINIX does not have yet end in a second); it
gets longer as MINIX gains features.

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
   a Sub-GHz row says BLOCKED with "transmit", choose another frequency with `--freq` (one that
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
4. Optional: `echo console cyan >/dev/led` as root makes the MINIX Flipper's LED flash with every
   burst of console traffic. During a run it then flickers all the time; when it stays dark for
   more than a few seconds, the script is waiting on something (or the console stopped
   answering: look at the log). `echo off >/dev/led` before power measurements.

## 6.4 The physical setup

### Back to back (125 kHz RFID, later NFC)

The 125 kHz RFID coil and the NFC antenna of a Flipper are both on its **back**. Laid back to
back, each Flipper's coil sits over the other's, which is what a reader and a tag need. The
distance must be **small and the same every time**, or a result depends on how the Flippers
happened to lie.

**The spacer matters, and no single one suits every RFID row** (measured 2026-11-19, both
Flippers in their silicone cases, reference Unleashed `unlshd-093`). The three setups:

- **thick**: the whole box of a Flipper Zero, folded along its own edges (about 10 mm);
- **thin**: half of that box, folded so that half of it presses the Flipper against the table
  (about 5 mm);
- **none**: the silicone cases touch.

| Spacer | MINIX emulates, Unleashed reads (M→U) | Unleashed emulates, MINIX reads (U→M) |
|---|---|---|
| **thick** | ASK and FSK read; PSK mostly not (Keri 1 in 3) | 18 of 25 (with `rfid` from #357) |
| **thin** | 3 of 5 PSK; most ASK/FSK not | ASK and PSK read; of the FSK ones HIDProx |
| **none** | all 5 PSK (5/5 each); FSK 0/5, ASK RF/32 0/3 | ASK and PSK read; of the FSK ones AWID |

For reading, ASK and PSK tags read at every spacer; each FSK row reads only at some (HIDProx
thick or thin, AWID thick or none), HIDExt and Pyramid at none of them, and EM4100 at RF/16
never. So `--spacer` gates the emulation rows only: run those ASK and FSK rows with the thick
spacer and the PSK rows (Keri, Indala26, Idteck, Nexwatch, Indala224) with none. Tell the script
which is in with `--spacer none|thin|thick` (rows that need another one are BLOCKED and say
so), or use `--spacer ask`: it runs the thick rows first and then asks you to take the spacer
out. Real tags (T5577 fobs written as HIDProx, AWID, Indala26, Keri, PAC/Stanley, Electra,
EM4100) read without any of this (#357); the spacer only matters for a Flipper's emulation.

1. Silicone cases may stay on if you use the table above; without them the distances differ,
   so find your own thick and thin spacers the same way.
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

The antennas are inside, but **take the stack apart for the Sub-GHz rows and put the two
Flippers about 1 m apart** (both stay on USB; unplug the 17/18 wires). Back to back the
receiver is overloaded: MINIX measures about -37 dBm during Unleashed's transmission, the
CC1101's demodulated output is choppy noise, and almost nothing decodes. At about 1 m it reads
-59 dBm and the frames are clean (checked 2026-11-19: 31 decode rows PASS, 0 back to back).
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

1. **Self-test, no hardware.** It runs every row against two simulated Flippers and checks
   the script itself works on this computer (a few minutes):

   ```
   python3 twoflip.py --selftest
   ```

   The last line must be `selftest: ok (250 rows)` (the number grows with the rows).

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

3. **Run everything and write the map.**

   ```
   python3 twoflip.py --log run.log --map coverage.md
   ```

   It finds both Flippers by itself. If it does not, give the ports:
   `python3 twoflip.py -m /dev/ttyACM0 -u /dev/ttyACM1` (Windows: `-m COM5 -u COM6`; macOS:
   `-m /dev/cu.usbmodem... -u /dev/cu.usbmodemflip_...`).

   If the MINIX Flipper is at `login:`, the script asks for the root password. To avoid the
   question (for example when another program starts the script), set it in the environment:
   `TWOFLIP_PASSWORD=... python3 twoflip.py` (Windows: `set TWOFLIP_PASSWORD=...` first), or
   pass `--password`.

4. **Read the result.** One line per row while it runs:

   ```
   MINIX on /dev/ttyACM0, Unleashed on /dev/ttyACM1
   MINIX Galtzagorriak MINIX 0.1.1, Unleashed unlshd-083; 234 rows
   PASS    rfid-EM4100-u2m                          MINIX read EM4100 1A2B3C4D5E
   PASS    rfid-EM4100-m2u                          Unleashed read EM4100 1A2B3C4D5E
   MISSING rfid-H10301-u2m                          not decoded: no EM4100; 812 changes, 7-90 us, mean 25 us
   MISSING rfid-H10301-m2u                          rfid emulate: usage: rfid [-r] [-n TRIES]
   ...
   PASS    ir-RC6-u2m                               RC6 0x94 0xA0
   FAIL    ir-Kaseikyo-m2u                          Unleashed decoded nothing
   ...
   BLOCKED subghz-level_868.35_MHz-u2m              Unleashed does not transmit at 868350000 Hz here: ...
   MISSING subghz-decode-CAME-u2m                   no subghz command (MINIX: subghz rx FREQ)
   ```

   and at the end a count per area:

   ```
                      PASS     FAIL  MISSING  BLOCKED      N/A     INFO
   rfid                  2        0       47        1        0        0
   rfid-write            0        0        0        0       50        0
   ir                   28        2        0        0        0        0
   ...
   ```

   `coverage.md` is the map: a summary table, then per area one line per feature with the
   U→M and M→U result side by side. `coverage.json` next to it has the same rows for scripts
   and for the next comparison. `run.log` has everything both consoles said, with times; attach
   it when reporting a FAIL.

   The exit status is the number of FAILs (plus regressions with `--baseline`), so `0` means
   nothing that MINIX has is broken.

5. **Next time, compare.** Keep `coverage.json` and pass it to the next run:

   ```
   python3 twoflip.py --map coverage2.md --baseline coverage.json
   ```

   It lists every row whose status changed: `NEW PASS` (a feature arrived), `REGRESSED` (a row
   that passed before and does not now; these count in the exit status) and the other changes.

### Useful options

| Option | Meaning |
|---|---|
| `PATTERN ...` | run only the rows whose id matches (shell pattern or prefix): `rfid`, `ir-*-m2u`, `subghz-decode-CAME*`, `rfid-EM4100-u2m`; `--list PATTERN` shows which |
| `--list` | list all rows |
| `-r N` | run each row N times; a row that passed only sometimes is FAIL "k/N passed" (shows marginal distance or alignment) |
| `--map FILE` | write the coverage map (Markdown) and FILE.json |
| `--baseline FILE.json` | compare with an earlier run |
| `--spacer none\|thin\|thick\|ask` | RFID: the spacer between the Flippers (6.4). RFID rows that need another one are BLOCKED; `ask` runs the thick-spacer rows first, then asks for the change. Written into the map. |
| `--t5577` | a blank T5577 tag is at hand: run the `rfid-write` rows. Interactive: the script tells you where to hold the tag and waits for Enter. Take the stack apart for these rows. |
| `--rw1990` | a blank RW1990 iButton key is at hand: run the iButton write rows (hold it to the contacts of either Flipper: the 1-Wire line is shared by the wires) |
| `--freq HZ` | Sub-GHz frequency of the raw, Princeton and chat rows (default `433920000`); refused if it is not a transmit band of `--region` |
| `--region EU\|US\|any` | where the rows may make the reference Flipper transmit (default `EU`: 433.05–434.79 and 868.0–868.6 MHz). Rows outside it are BLOCKED. Unleashed applies no region itself, so this is the only guard: use `US` or `any` only where that is legal |
| `--log FILE` | full transcript of both consoles |
| `--password PW` | MINIX root password (else `TWOFLIP_PASSWORD`, else asked) |

### Test data

- **125 kHz:** each protocol gets the first bytes of a fixed pattern (`1A2B3C4D5E...`) of its
  data length (Unleashed's `rfid emulate X 00` lists names and lengths). If a row says the
  protocol was read but the data differs in both directions, the protocol may not carry every
  bit of the pattern: compare with what Unleashed read in the M→U row and change the vector in
  `LF_PROTOCOLS`/`lf_data`. `LF_CANONICAL` holds such vectors (Paradox, Gallagher, Radio Key,
  FDX-A): the bytes the Unleashed code itself gives back after encoding and decoding the pattern. "Radio Key" has a space in its name, which Unleashed's command line
  cannot take, so its U→M row is BLOCKED.
- **Infrared:** one message per protocol, from Unleashed's infrared unit tests where it has them
  (`IR_VECTORS`).
- **Sub-GHz:** `flipper/tools/twoflip_data/subghz/*.sub`, the key files of Unleashed's own unit
  tests (real recordings, valid checksums), all at 433.92 MHz. Rolling-code protocols (KeeLoq,
  Security+, Phoenix V2) change their code on every send, so only the protocol name is
  compared for them. The script copies each file to `/ext/twoflip/` on the Unleashed SD card
  (and to `/tmp` on MINIX) before sending it.
- **NFC:** `flipper/tools/twoflip_data/nfc/*.nfc`, Unleashed's unit-test tag files plus one
  plain ISO14443-3A file. A row passes when the reader names the tag type.

## 6.7 When something fails

MISSING rows are not problems: they are the part of the map MINIX has not reached yet. The
table is about FAIL and BLOCKED rows and about the script not starting.

| Symptom | What to do |
|---|---|
| `did not find both Flippers` | Run `--ports`. A missing port: other cable (data cable), other USB socket, is the Flipper on? `busy`: close qFlipper, screen, minicom, PuTTY, Arduino IDE. `None`: see the next lines. |
| `Permission denied` (Linux) | 6.5 Linux step 2 or the udev rule of step 3; log out and in after `usermod`. |
| `no '>: ' prompt from the Unleashed Flipper` | Go back to the Flipper's main screen (press Back until the dolphin shows); unplug and replug it; close Bad USB/U2F apps. |
| Many Unleashed rows BLOCKED, or "command not found" in the log | Its SD card is missing or lacks the resources: put the card in, reinstall Unleashed with qFlipper (6.2). |
| `no root shell on the MINIX Flipper` | Wrong password, or a user other than root is logged in at the console (the script logs a `$` shell out and retries once). Log in by hand with a terminal program to check, then close it. |
| Random characters in the log right after plugging in (Linux) | ModemManager: 6.5 Linux step 3. |
| `rfid-EM4100-*` FAIL | Line the Flippers up exactly, use a thinner spacer, take off cases, keep metal (laptop, table frame) a few cm away. Run `python3 twoflip.py -r 10 rfid-EM4100` to see whether it is marginal. |
| `ibutton-Dallas_dev_ibutton-u2m` FAIL "no ROM" | The wires: pin 17 to 17 and 18 to 18 (count from the printed numbers), pins pushed fully in. 5V on GPIO off on Unleashed. |
| `ir-*` FAIL "received nothing" | Paper roof above the top edges, or face the Flippers top edge to top edge (6.4). No sunlight or bright lamp on the windows. A wrong value (not "nothing") is a decoder or encoder bug: report it. |
| `subghz-level_*` BLOCKED | Unleashed may not transmit at that frequency with its region settings. Normal for 315 or 868 MHz in some regions. |
| Every Sub-GHz row BLOCKED with `Device 0 failed to start` (`subghz chat` works) | A bug in Unleashed `unlshd-095`'s command line, not the radio: the internal CC1101's driver has no `begin()` (`cc1101_int_interconnect.c`), `subghz_devices_begin()` then returns false, and the new start check in `subghz_cli.c` takes that as a failure, so `subghz tx`, `rx` and `tx_from_file` on device 0 always stop there. Install another Unleashed release on the reference Flipper (the script was written against `53818b1`). The region is not the cause: Unleashed reports `hardware.region.provisioned : 00` even with a valid `/int/.region_data`. |
| `subghz-decode-*` MISSING "decoded nothing" for almost every protocol, levels PASS | The Flippers are too close: the receiver is overloaded. Put them about 1 m apart (6.4, Sub-GHz). |
| `subghz-decode-Freezer`, `-Proteco` BLOCKED "reboots" | Unleashed `unlshd-093` says `Error transmitter` and reboots (its USB port goes away) on `tx_from_file` of these two files, which ended the whole run. The script skips them (`UL_TX_CRASH`). |
| `subghz-level_*` FAIL with a small rise | Another transmitter nearby during the quiet reading; run that row again. |
| A row hangs for more than a minute | Press Ctrl-C, unplug and replug both Flippers, rerun that row with `--log` and report the log. |

## 6.8 Doing it by hand

Each row is just two console commands, so a single row can also be tried by hand with two
terminal windows (`screen /dev/ttyACM1` on Linux and macOS, PuTTY "Serial" on Windows; any
speed). The commands the script uses today, on main:

| | Unleashed console (`>: `) | MINIX shell (`# `) |
|---|---|---|
| RFID U→M | `rfid emulate EM4100 1A2B3C4D5E` (Ctrl-C ends) | `rfid -n 8` |
| RFID M→U | `rfid read` | `rfid emulate EM 1A 2B 3C 4D 5E` (Ctrl-C ends) |
| IR U→M | `ir tx NEC 04 08` | `ir rx -n 1` |
| IR M→U | `ir rx` (Ctrl-C ends) | `ir tx NEC 0x04 0x08` |
| IR raw | `ir tx RAW F:38000 DC:33 3000 1000 ...` / `ir rx raw` | `ir rx -r -n 1` / `ir tx raw 3000 1000 ...` |
| iButton U→M | `ikey emulate Dallas 01A1B2C3D4E5F68F` (Ctrl-C ends) | `od -b /dev/ibutton` |
| Sub-GHz level | `subghz tx 74BADE 433920000 403 100 0` | `echo 433920000 > /dev/subghz`, `cat /dev/subghz`, `echo 0 > /dev/subghz` |
| Sub-GHz raw | (as above) | `ir rx -r -n 1 -d /dev/subghzraw` |
| Sub-GHz file | `storage write_chunk /ext/twoflip/came.sub SIZE`, then `subghz tx_from_file /ext/twoflip/came.sub 10 0` (10 transmissions: most decoders report a frame only after seeing it twice in a row); `subghz rx 433920000 0` | `subghz rx 433920000`, `subghz tx_from_file FILE 10` |
| NFC | `nfc`, then `emulate -f /ext/twoflip/Ntag215.nfc` or `scanner`, then `exit` | (planned, 6.10) |

The Unleashed sources these were checked against: `applications/main/lfrfid/lfrfid_cli.c`,
`infrared/infrared_cli.c`, `ibutton/ibutton_cli.c`, `subghz/subghz_cli.c`, `nfc/cli/` and
`applications/services/storage/storage_cli.c` (commit `53818b1`). The prompt `>: ` comes from
`lib/toolbox/cli/shell/`.

## 6.9 Reporting

Attach `coverage.md` (and `coverage.json`) to the device-check issue (#109 for the backlog,
#210 for the 125 kHz protocols) with the spacer you used and, for any FAIL, the `--log` file.
The map already names the MINIX version (`ver`) and the Unleashed version (`info device`).

## 6.10 For developers: the commands MINIX is expected to have

The rows for features MINIX does not have yet use these commands; the ones marked
**present** exist on main. They mirror the stock
command line (spec 4.x "cli commands → Unix commands", and R9.0.3 for `rfid`), so the stock
apps' habits carry over. A different syntax is fine, but then change the row in `twoflip.py`
in the same PR.

| Command | Output the row looks for |
|---|---|
| `rfid -n 8` (read, any protocol) | `PROTOCOL HEXDATA` (spaces between bytes allowed), e.g. `H10301 1A 2B 3C` |
| `rfid emulate PROTOCOL HEX`, `rfid write PROTOCOL HEX` | emulate prints a line with "emulating" and runs until Ctrl-C; the name is quoted when it has a space (`"Radio Key"`). EM4100 keeps `rfid emulate EM B0 B1 B2 B3 B4` |
| `ikey read` | `Dallas HEX`, `Cyfral HEX`, `Metakom HEX` (as stock) |
| `ikey emulate TYPE HEX`, `ikey write Dallas HEX` | emulate prints "Emulating..." and runs until Ctrl-C |
| `subghz rx FREQ` (**present**, #302) | each decoded packet with the protocol name and its key in hex (as stock): `Princeton 24bit Key:7C5703` |
| `subghz tx KEY FREQ TE REPEAT`, `subghz tx_from_file FILE REPEAT` (**present**, #302; on the Flipper they say "no transmitter on this device yet" until the CC1101 transmit path exists) | sends, then exits 0 |
| `subghz chat FREQ` (**present**, #302; transmit as above) | lines typed are sent, lines received are printed |
| `nfc scanner` | the detected protocol names as stock (`ISO14443-3A`, `NTAG/Ultralight`, `FeliCa`, `SLIX`) |
| `nfc emulate -f FILE` | prints "Emulating..." and runs until Ctrl-C |

A command that is missing, or answers with a usage message or "unknown protocol", makes the row
MISSING; anything else that does not match is FAIL.

## 6.11 For developers: changing the script

- A row is a function `(ul, mx, opt, *args) -> (status, detail)` in `twoflip.py`, added in
  `build_rows()`. `ul.start(cmd)` / `ul.stop()` run a long Unleashed command (one that ends
  with Ctrl-C), `ul.run(cmd)` a short one, `ul.upload(path, bytes)` writes a file to its SD
  card; `mx.run(cmd)` runs a MINIX command and returns its exit status and output,
  `mx.start` / `mx.stop` the long ones, `mx.exists(word)` whether a command exists,
  `mx.upload(path, text)` writes a file. `wait(regex, secs)` waits for output.
- **A pattern that ends in a number must also match the character after it**: `(?=\D)`,
  `(?=[^0-9A-F])` or `(?=\r?\n)`. USB delivers a line in pieces. Without that,
  `command 0x08` matched as `0x0` (5 of 10 IR rows failed on the device) and `@@R=127` as `@@R=12`
  (a missing command counted as present). The simulator sends the last character of each line
  20 ms late, so `--selftest` catches such a pattern.
- New test files go into `twoflip_data/subghz/` or `twoflip_data/nfc/`; they become rows by
  themselves.
- The simulator `twoflip_sim.py` imitates both Flippers as main has them; when MINIX gains a
  command, teach the simulator the command and change `expected()`, and keep
  `python3 -I twoflip.py --selftest` passing.

## 6.12 Two MINIX Flippers: the data modems (`--peer-minix`)

The `gmodem` command (spec section P, manual chapter [Data modems](../../manual/modem.md)) moves
files between two Flippers that **both run MINIX**. The Unleashed firmware has nothing like it,
so the reference Flipper of the rest of this chapter cannot be the partner: against Unleashed
the 16 `gmodem` rows are **N/A**. To run them you need a second Flipper with MINIX for the
session.

### What you need

- The MINIX Flipper of 6.3 and **a second Flipper with the same MINIX build** (same
  `minix-base.bin` and SD card image, prepared exactly as in 6.3, with its own SD card). If the
  only second Flipper is the Unleashed reference: back it up with qFlipper, flash it with MINIX
  for the session (the manual's [installation chapter](../../manual/install.md)), and flash
  Unleashed back afterwards (installation chapter, section 8, "Recovery and going back"). Note it
  in the report, because the reference Flipper's own settings are then gone until restored.
- On both: logged in as root (the script logs in with `--password` or `TWOFLIP_PASSWORD`;
  both need the same root password, or log in by hand first).

### Physical setup

- **RFID rows:** back to back exactly as in 6.4 ("Back to back"), same spacer. The sending
  Flipper is the tag (it only loads the other's field), the receiving one is the reader. Keep
  the spacer thin (1–3 mm): the modem's tag side reaches no further than `rfid emulate`.
- **Sub-GHz rows (when `/dev/subghz` can transmit, T26b):** the two Flippers 1–2 m apart on the
  table, antennas (the top left corner) not touching anything metal. Set the region first, on
  both: `echo EU > /etc/flipper/region` (or `US`); `gmodem info -m subghz` must say `ok` for the
  preset before the row can pass.
- **NFC and BLE rows:** MISSING until their drivers exist (Phases 13 and 15).

### Running

```
python3 twoflip.py --peer-minix --ports          # both must say minix or login
python3 twoflip.py --peer-minix --log modem.log --map modem.md modem
```

Without `-m` and `-u` the first MINIX port found is "M" and the second the peer "P" (`u2m` in
the row names means the peer sends). Give them by hand to fix the order:
`-m /dev/ttyACM0 -u /dev/ttyACM1`. With `--peer-minix` only the `gmodem` rows run; everything
else needs the Unleashed partner.

Each row: the sender gets `/tmp/mdm.in` (1 KB of text, a few Basque words in it: "Kaixo,
galtzagorriak! Datuak irratiz doaz"), the receiver starts `gmodem recv -m MEDIUM -p PRESET -o
/tmp/mdm.out -t 60`, the sender `gmodem send -m MEDIUM -p PRESET /tmp/mdm.in`; when the receiver
ends, the script stops the sender (Ctrl-C) and compares `cksum` on both. A row takes up to three
minutes. PASS shows the bytes and the seconds (`1152 B in 14 s`): put those numbers in the
report, they are the measured rates the manual quotes.

| Row result | What to do |
|---|---|
| PASS | note the time |
| FAIL "did not finish in 180 s" | move the stack: the coils must lie on top of each other; try `-p robust` by hand (6.8) |
| FAIL "checksums differ" | a bug (a frame with a good CRC but wrong data must not happen): report it with the `--log` file |
| MISSING "not implemented yet" | the medium's driver is not there; expected for Sub-GHz until T26b, NFC, BLE |

### By hand

On the receiving Flipper, then on the sending one within a few seconds:

```
gmodem recv -m rfid -o /tmp/got.txt          # reader: waits, shows frames as they come
gmodem send -m rfid /etc/motd                # tag: plays the file until Ctrl-C
```

The reader prints `N bytes, F frames, T s` and exits 0 when everything arrived; then
`cksum /tmp/got.txt` on one and `cksum /etc/motd` on the other must agree. `gmodem info` on
either shows the rates and, for Sub-GHz, what the region allows.

