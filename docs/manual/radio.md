# Radio stacks on the SD card

The Flipper's second CPU (CPU2, a Cortex-M0+) runs the radio: Bluetooth LE,
or Thread, Zigbee, 802.15.4. Its program, the **wireless stack**, is a
signed, encrypted image from ST that only ST's FUS (firmware upgrade
services) can install. MINIX never runs its own code there. You can keep
several ST stacks on the SD card and see which ones fit. Switching between
them (`radio use`) is still planned (task T30b).

What works now:

- `radio-fetch.py` on your PC: download ST's stacks onto the card, with their checks.
- `radio list`, `radio add`, `radio rm` on the Flipper: check the images and
  reserve the flash a bigger stack would need (the **radio slot**).
- `pkg install --flash` and `pkg flash` keep out of the radio slot.

Planned: `radio use`, `radio repair`, `radio fus` (the FUS client, maintenance
mode), the stack line at boot (R2b.10), the Radio page in `settings`.

## 1. Getting the images (on your PC)

The Flipper has no internet, so the images come from your PC. The tool is
`flipper/tools/radio-fetch.py`. It downloads from ST's public `STM32CubeWB`
repository at one fixed release (now **v1.23.0**). It also carries a table
of that release's stacks, checked by hand: install address, the FUS version
each stack needs, size and SHA-256. A download whose SHA-256 differs from
the table is not written.

```
$ python3 flipper/tools/radio-fetch.py list
STM32CubeWB v1.23.0, install addresses for 1 MB parts
name                       KB       addr    fus  file
...
ble-full                  128 0x080D0000  2.0.0  stm32wb5x_BLE_Stack_full_fw.bin
ble-light                  99 0x080D7000  2.0.0  stm32wb5x_BLE_Stack_light_fw.bin
...
zigbee-ffd                335 0x080A0000  1.2.0  stm32wb5x_Zigbee_FFD_fw.bin
```

- `list --check` downloads the release notes of that tag again and stops if
  they and the table disagree.
- `list --fus 1.2.0` marks the stacks your FUS is too old for.

Copy stacks to the card (mounted on the PC at `/media/sd`, say). Each one
becomes a directory `radio/<name>/` with the image and a `manifest`:

```
$ python3 flipper/tools/radio-fetch.py get ble-light ble-full --to /media/sd/radio
radio-fetch: wrote /media/sd/radio/ble-light (100540 B)
radio-fetch: wrote /media/sd/radio/ble-full (130488 B)
```

An existing directory is replaced only with `--force`. With `--fus 1.2.0`
(the FUS version on your Flipper), stacks that need a newer FUS are refused
with a note that FUS must be updated first.

**No internet on the PC either?** A stock or Unleashed update directory
already holds a radio image. `from-fuf` takes the image and its address
from that directory's `update.fuf`:

```
$ python3 flipper/tools/radio-fetch.py from-fuf f7-update-0.99.1 --to /media/sd/radio --name ble-argia
```

(*argia*, "light": a name of your own for the stock light stack.)

You can name directories as you like with lower-case letters, digits and
`- _ .`. The `name` in the manifest must match the directory name.

### The manifest

A text file `manifest` with one `key<TAB>value` per line (spec R16.0.1):

```
name	zigbee-baserria
type	ble-zigbee-ffd
version	1.23.0
fus	2.0.0
addr	0x0807F000
size	460680
sha256	c2a9e3ca…
note	STM32CubeWB v1.23.0
```

`addr` is ST's install address on 1 MB parts (the Flipper's STM32WB55RG).
`fus` is the oldest FUS version that installs the image. `note` is
optional. Any other key is an error.

## 2. On the Flipper: `radio`

```
radio                  the flash layout: where CPU2 starts, programs, cache, the slot
radio list [-F 2.0.0]  the images in /ext/radio and whether they pass the checks
radio add [-F 2.0.0] NAME    (root) check /ext/radio/NAME, then update the slot
radio rm NAME          (root) delete /ext/radio/NAME, then update the slot
```

`radio add` checks the manifest, the image's size and SHA-256, the FUS
version (with `-F`), and whether the stack fits (below). Until the FUS
client exists, MINIX cannot read the FUS version itself. Give it with `-F`.
`radio` will print it in a later version.

### The radio slot

The stock light stack takes the top ~200 KB of flash. A bigger stack (full
BLE, Zigbee) starts lower. To make sure a later switch never overwrites
installed programs, `/etc/radio.slot` holds the lowest install address of
the stacks on the card that pass the checks (one number, written by
`radio add` and `radio rm`; no file: no slot). The flash between the slot
and the running stack is the **radio slot**:

- `pkg install --flash` and `pkg flash` never put a program there. If a
  program doesn't fit, `pkg` says
  `radio slot: flash from 0x0807F000 reserved for CPU2 stacks on the card`,
  and the program runs from RAM as usual.
- After a switch, only the text cache (copies that can be thrown away) is
  lost. *Planned:* the cache also uses the slot while a smaller stack runs.
  Today the cache keeps its fixed 128 KB.
- If CPU2 ever starts lower than before (a bigger stack), at the next boot
  MINIX treats every installed program and cache copy that now lies past
  the end as gone. Such a program runs from RAM again until you
  `pkg flash` it once more. The console says
  `flash: program at 0x… past the region (SFSA moved): dead`. MINIX
  marks these programs dead in flash once (8 bytes each), so switching
  back to the smaller stack cannot bring back a copy that CPU2 or the
  cache has overwritten in the meantime. With the slot this does not
  happen, because no program is put there in the first place.
- `radio add` refuses a stack that would leave less than 32 KB of cache or
  reach the system, the shared libraries or the launcher. Example: on a
  1 MB part, `ble-thread` (static, 0x0806B000) fits, `ble-thread-dyn`
  (0x08069000) does not.

`radio rm` of the lowest stack makes the slot smaller and returns the space
to `pkg`.

Nothing here writes internal flash or talks to CPU2. `radio` keeps nothing
in memory after it exits.

## 3. The phone link (planned)

The official Flipper mobile app will talk to MINIX over Bluetooth LE, using
its own protocol: the stock serial service and a subset of the stock RPC
(spec R15.7). Nothing has to be installed on the phone. This needs CPU2
running a BLE stack (Phase 15), so it does not work over the air yet.
What exists is the part above the radio: the `rpcd` program on MINIX and
the same code on a PC:

- **libflipperpb**: the stock protobuf (version 0.25, the one stock firmware
  1.3.4 uses), generated with nanopb.
- **The RPC core** (`src/lib/rpcd`): one frame up to 1100 B in each
  direction, 6.1 KB of state per session. It answers:
  - `ping`, `protobuf_version` (0.25), `device_info` (the stock keys plus
    `firmware_origin: Galtzagorriak MINIX`), `power_info`;
  - get and set date and time. A date the phone sends is checked first
    (2000–2099, a real day of the month); it is never taken blindly as
    stock does;
  - storage `info`, `stat`, `list`, `read`, only below `/ext` (paths with
    `..` are refused).
  Anything else gets `ERROR_NOT_IMPLEMENTED` and the link stays up, so the
  app uses what is there.
- **The BLE serial service** (`src/lib/bled`): the stock service and its
  characteristics, MTU handling, and the flow-control credit (the phone
  never sends more than the 1 KB receive buffer has room for).

Pairing data will be kept on the SD card (R15.4), never in internal flash.

### `blescan`: the devices around, and a tracker finder

```
blescan [-t] [FILE]          read the controller's HCI events from FILE (or standard
                             input) until it ends, then list the devices around you,
                             strongest first; -t only the trackers
blescan -F ADDR [FILE]       finder: one line with bars for every report of the device
                             ADDR (AA:BB:CC:DD:EE:FF): the more `#`, the closer
```

```
$ blescan -t /tmp/scan.hci
ADDRESS            RSSI BARS       KIND       NAME
DE:AD:BE:EF:00:01  -60 [#####   ] Tile       Tile
C0:11:22:33:44:55  -78 [###     ] Find My
$ blescan -F C0:11:22:33:44:55 /tmp/scan.hci
[##      ] -80 dBm (avg -80)
[####    ] -70 dBm (avg -78)
```

Walk around with the Flipper and watch the bars grow (hot and cold) to find a lost tracker
(*galdu duzun giltza*, the lost key).

What it knows from the advertisement alone, no connecting: the address, the signal strength
(averaged over the last reports, so the bars do not jump), the name, and a *kind* when the packet
says so: **Find My** (Apple's network: AirTags and others, manufacturer data `4C 00 12`), **Tile**
(service `FEED`/`FEEC`), **SmartTag** (Samsung, service `FD5A`), **Find Hub** (Google's network,
Eddystone service data with frame `40`/`41`), plus plain Eddystone, iBeacon and other Apple
packets. These are guesses from what the packet claims, not proof; a tag that rotates its
address (Apple and Google tags do, every few minutes to 24 hours) shows up as a new device each
time, so use `-F` with the address you see now. At most 16 devices are kept; a weaker newcomer
does not push out a stronger one.

**Status.** The command and its library (`src/lib/blescan`, tested on the PC with recorded
HCI frames) are ready. The source is a stream of HCI events (H4, LE Advertising Report): a file,
a pipe, or an external controller. The Flipper's own radio feeds it when CPU2 scanning exists
(`radio use` and the FUS client, a task for the device lane); until then the command works only
on a recording. It needs about 4 KB of memory and writes nothing to flash.

**Not possible: the Flipper as a Bluetooth speaker.** The Flipper's chip has Bluetooth Low
Energy only, not the Classic profile (A2DP) that MP3 players and phones use for music, so it
cannot play their sound. It can still play tunes of its own on the piezo.

### `rpcd` on MINIX

```
rpcd [device]       serve one RPC session on device (default: standard
                    input and output) until it closes or hangs up
```

`rpcd` is the RPC core with the MINIX side added: `device_info` gives
`firmware_version` (the Galtzagorriak release), `firmware_origin` and
`firmware_base`; `power_info` reads `/dev/power` (charge, voltage,
current). Storage requests see `/ext` as `/home/flipper`, the same tree
the stock firmware calls `/ext`. `rpcd` has the rights of the user who
starts it, so it reads only what that user may read.

Setting the time follows R15.7. The phone sends its local time, and MINIX
uses its own `TZ`:

- a difference of 2 s or less is only logged;
- a difference close to a whole number of quarter hours, 45 minutes or
  more, means the phone is in another time zone: `rpcd` logs a notice
  and leaves the clock alone;
- any other difference steps the clock, but only if `rpcd` runs as root.
  Otherwise the app gets an error.

It needs about 16 KB of memory while it runs (6.1 KB of that is the
session) and writes nothing to internal flash. Nothing starts `rpcd` yet.
The second USB serial port and BLE will start it when they exist (planned).
The QEMU self-test (`m2test`) sends it stock frames through a pipe.

### Testing with `rpc.py`

`flipper/tools/rpc.py` speaks the same frames as the app. Today it talks to
the PC build of the RPC core, or to any command that runs `rpcd` with its
input and output on a pipe (`--exec`). Later it will talk to the Flipper's
second USB serial port, and over BLE through a bridge:

```
$ python3 flipper/tools/rpc.py --exec 'RPCD_EXT=/media/sd flipper/tests/rpcd/build/rpcdhost' info
format_major: 3
...
firmware_origin: Galtzagorriak MINIX
$ python3 flipper/tools/rpc.py --exec ... ls /ext
d          0 radio
-          6 agurra.txt
$ python3 flipper/tools/rpc.py --port /dev/ttyACM1 ping kaixo     (planned)
```

Commands: `ping`, `version`, `info`, `power`, `time`, `settime`, `ls`,
`stat`, `df`, `cat`. Build the PC side with
`make -C flipper/tests/rpcd test`.

## Canaries: jamming and followers (coming)

The *canary* app listens and warns, it never transmits. Two of its rules are ready and wait for the radio side:

- **Jamming** (`subghz` canary): if the chosen band stays loud (default: -60 dBm or stronger) for 3 seconds without a gap longer than 0.2 s, the Flipper says so, and says when the band is quiet again for 2 seconds. A short burst, such as the doorbell of your neighbour in Bilbo, does not raise it. If the Flipper sleeps between samples the count starts again, so a sleeping gadget never cries wolf.
- **Follower** (`ble` canary): a device that your scans meet again and again, at least 6 times a minute or more apart, for 15 minutes or longer, is flagged once, for example a tracker that travelled with you from the bus to the *etxea*. Someone who only walked past is not flagged.
