# iButton keys: the iButton app

**iButton** on the launcher is the stock iButton app as a window app (issues #413, #490). It runs
the `ikey` command (spec R6.7) and shows its lines as they come. It knows every protocol the stock
firmware (Unleashed) knows: **DS1990, DS1992, DS1996, DS1971, DS1420, DSGeneric** (any other
Dallas family), **Cyfral** and **Metakom**.

## Hookup

**Hookup** in the app's menu tells it on the Flipper itself. In short: touch the key to the
iButton pad on the top edge of the Flipper (the two metal contacts, data and ground), or wire it to
the GPIO header: **pin 17 = data (1-Wire), pin 18 = GND**. The line idles high through the board's
own pull-up. Reading and writing need the key
or blank held steady until the app says it is done (a second or two, a DS1996 about ten).

| Row | What it does |
|---|---|
| Read | `ikey read`: hold a key to the contacts (10 s). When a key was read, **Use this key** opens its page. A DS1992, DS1996 or DS1971 is read with its memory. |
| Saved | the `.ibtn` files of `/home/flipper/ibutton`, a page at a time; OK opens the key's page. |
| Add manually | the protocol, then the key in hex, then **Next** (see below). |
| Hookup | how to connect a key (the text above). |

**Add manually.** For a Dallas family you type the serial only, 12 hex digits; the family code and
the CRC byte are filled in (as stock does). **DSGeneric** takes 14 digits, family code and serial;
the CRC is filled in. **Cyfral** takes 4 digits and **Metakom** 8 (every byte of even parity, which
a reader checks). A DS1992, DS1996 or DS1971 made by hand has its memory filled with zeros.

A key's page shows its protocol and number, then:

- **Info**: the family code, whether the CRC is right, and the memory a family has (DS1992 SRAM
  128 B, DS1996 SRAM 8192 B, DS1971 EEPROM 32 B).
- **Emulate**: `ikey emulate`; the Flipper is the key until you press Back. Each time a reader
  got it, a line says so. The ROM is sent, **not the memory** of a DS1992, DS1996 or DS1971: a
  door reader that only asks for the number is served; one that reads the memory gets nothing.
- **Write** (a Dallas family): the ROM to a blank on the contacts, read back to compare. The app
  tries an RW1990 (both kinds), then a TM2004, and shows which one took it.
- **Write to TM01x**: a Dallas ROM to a TM01x blank, or a **Cyfral or Metakom key** to one (the
  only blank that takes them). A TM01x holding a Cyfral or Metakom key is **finalized**: it cannot
  be written again, so the app asks first.
- **Write memory** (DS1992, DS1996, DS1971): the memory of the saved key onto a same-family key
  on the contacts (a page at a time through its scratchpad), read back to compare. The ROM of such
  a key cannot be changed; use a key of that family you may overwrite.
- **Name** and **Save** (a key you read or added), or **Delete** (a saved one).

Files are stock's: version 2 (`Protocol: DS1990` and `Rom Data:`, a DS1992 or DS1996 also
`Sram Data:`, a DS1971 `Eeprom Data:`; `Protocol: Cyfral` / `Metakom` and `Data:`) is written;
version 1 (`Key type:`) is read too. A key just read or added waits in the hidden file
`/home/flipper/ibutton/.last.ibtn` until **Save** gives it a name (so the memory of a DS1996 never
passes through the app's own RAM); a new read replaces it. Archive's **Open** on an `.ibtn` file
starts this app on that file.

From the shell the same: `ikey read -o key.ibtn` (reads and saves), `ikey write key.ibtn`,
`ikey write -b tm01x Dallas 01A1B2C3D4E5F68F`, `ikey copy key.ibtn`, `ikey emulate key.ibtn`;
see [Devices](devices.md).

Under QEMU there are no contacts; the M2 self-test checks Add manually (a DS1990 and a DS1992 with
memory), Info, Hookup, Save and Saved. Reading, writing and emulating need the device.
