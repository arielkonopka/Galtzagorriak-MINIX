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
- `radio add` refuses a stack that would leave less than 32 KB of cache or
  reach the system, the shared libraries or the launcher. Example: on a
  1 MB part, `ble-thread` (static, 0x0806B000) fits, `ble-thread-dyn`
  (0x08069000) does not.

`radio rm` of the lowest stack makes the slot smaller and returns the space
to `pkg`.

Nothing here writes internal flash or talks to CPU2. `radio` keeps nothing
in memory after it exits.
