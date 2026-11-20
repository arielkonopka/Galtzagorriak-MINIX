# Infrared remotes: the `infrared` app

The **Infrared** app on the launcher does what the stock firmware's Infrared app does. It
learns the buttons of a remote, keeps them in files, sends them again, and has universal remotes
for TVs, audio players, projectors, air conditioners, fans and LED strips. It uses the stock
protocols and the stock file format: a remote saved on a stock Flipper works here, and one saved
here works on a stock Flipper (spec Phase 7, AC7.1).

Protocols (the stock firmware's own code, `src/lib/infrared`): NEC, NECext, NEC42, NEC42ext,
Samsung32, RC5, RC5X, RC6, SIRC, SIRC15, SIRC20, Kaseikyo, RCA, Pioneer. A signal that is none of
these is kept as raw timings (at most 1024, as stock).

## The menu

- **Learn New Remote.** Point the remote at the Flipper's IR window and press a button. The
  screen shows the protocol and the address and command, or `RAW` and how many timings were
  read. OK saves the button: type its name on the keyboard (for example `Piztu` or
  `Bolumena_gora`); Back skips it. Each button goes into a new remote file,
  `/home/flipper/infrared/Remote_N.ir`. Back at the "point the remote" screen ends learning.
  You can rename the file later in the Archive app (Infrared tab, Name).
- **Saved Remotes.** The `.ir` files in `/home/flipper/infrared`, then the buttons of the one
  you pick. OK sends the selected button; the LED is blue while it is sent.
- **Universal Remotes.** Pick a kind (TVs, Audio Players, Projectors, Air Conditioners, Fans,
  LEDs) and a button (Power, Mute, Vol_up, ...). The app sends every signal of that name in the
  kind's database, one after the other, as stock does, and shows how many it has sent. Back
  stops it. The databases are stock's files, read one signal at a time, never loaded whole.
- **Debug.** Shows every frame received: the protocol, address and command (or `unknown`), the
  number of timings and the first four. Back leaves.

## Files

| Path | What |
|---|---|
| `/home/flipper/infrared/*.ir` | your remotes (`Filetype: IR signals file`, version 1) |
| `/home/flipper/infrared/assets/tv.ir`, `audio.ir`, `projectors.ir`, `ac.ir`, `fans.ir`, `leds.ir` | the universal remote databases (`Filetype: IR library file`) |

Copy the databases from a stock Flipper's SD card (`/ext/infrared/assets/`, or the Unleashed
firmware's `applications/main/infrared/resources/infrared/assets/`) to
`/home/flipper/infrared/assets/`, for example with MTP ([Files to a PC](usb-files.md)) or
`rz`. Without one, the universal remote says "no database". An example of a remote file
with one button learned as a known protocol and one as raw timings:

```
Filetype: IR signals file
Version: 1
#
name: Piztu
type: parsed
protocol: NECext
address: EE 87 00 00
command: 5D A0 00 00
#
name: Bolumena_gora
type: raw
frequency: 38000
duty_cycle: 0.330000
data: 504 3432 502 483 500 484 510
```

## Who may use it: group `ir`

`/dev/ir` belongs to group `ir` (mode 660). The app is set-gid `ir`. It opens `/dev/ir` only
when the user who runs it is in group `ir` in `/etc/group`, either listed by name or with `ir`
as their own group, or is root. Otherwise it shows "cannot open /dev/ir". The launcher's apps
run as `nobody`, which is in the group as installed:

```
ir:*:13:root,nobody
```

To keep a user away from infrared, take their name out of that line. The `ir` command
(`ir rx`, `ir send FILE NAME`, ...) opens `/dev/ir` directly, so it works for root and members
of group `ir`. Cards made before this change get the group, and `/dev/ir` moves into it, when
the base package is installed (`base.postinst`).

## For testing (QEMU)

QEMU has no infrared hardware. `infrared -L` runs the app with a loopback instead of
`/dev/ir`: each frame it sends is the next frame it reads. `infrared -t FILE` sends every signal
of FILE over the loopback, decodes it and prints `infrared: N ok, M bad`. `infrared -c` only
tries to open `/dev/ir` as the app would, and exits with 1 when the user may not.

## Limits

- On the device, the carrier, the duty cycle and the gaps between repeats come from the
  kernel's `/dev/ir` driver. They are still to be checked against a real TV and with a camera.
- Learn names the file `Remote_N.ir`; stock asks for a remote name first.
- Universal remotes cannot be saved as a remote yet, and there is no "edit" (move or delete one
  button). For now, use the Archive app or `mined`.
