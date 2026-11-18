# Data modems: files between two Flippers

`gmodem` moves a file from one Flipper to another that also runs Galtzagorriak MINIX, without a
PC or a phone. It is one command for four radios: 125 kHz RFID, Sub-GHz, NFC and Bluetooth LE.
The design is spec section P.

| Medium | Works today | Speed | Range | Notes |
|---|---|---|---|---|
| `rfid` | yes, one way | a few tens of bytes a second (estimate; measured numbers follow) | backs together, 1–3 cm | the sender is a tag, the receiver a reader |
| `subghz` | **(planned)**, needs the transmit path (spec RP.8) | 0.1–14 KB/s by preset, less where the law limits sending time | metres to hundreds of metres | presets choose bandwidth and speed |
| `nfc` | **(planned)**, Phase 13 | ≈ 5–10 KB/s | ≤ 4 cm | |
| `ble` | **(planned)**, Phase 15 | ≈ 10–80 KB/s | 10–30 m | the only encrypted one |

## Sending a file over RFID

1. Lay the two Flippers back to back (the RFID coil is on the back), screens outwards, top edges
   together. Nothing thicker than thin cardboard between them.
2. On the receiving Flipper:

   ```
   gmodem recv -m rfid -o /home/flipper/nota.txt
   ```

3. On the sending Flipper:

   ```
   gmodem send -m rfid /home/flipper/nota.txt
   ```

The sender repeats the file in frames of 24 bytes; the receiver shows `12/40 frames` while it
collects them and ends with `960 bytes, 40 frames, 31 s` when everything has arrived. Then stop
the sender with Ctrl-C (or give it `-n 3` to stop by itself after three rounds). Every frame
carries a checksum: a damaged frame is dropped and taken again in the next round, so what
arrives is exactly what was sent.

`-p robust` is half as fast and copes with a worse position. `-t 60` waits 60 seconds instead
of 30 for a new frame before giving up.

Small things can go through pipes: `echo "Kaixo, lagun!" | gmodem send -m rfid` and
`gmodem recv -m rfid | cat` (up to 1 KB; for more use files).

**Compression.** Add `-z` on the sender: `gmodem send -m rfid -z notes.txt`. The file is
packed first (heatshrink, a small LZSS) and the receiver unpacks it by itself, with no option.
Text usually shrinks to a fifth or a third, so it arrives three to five times sooner; "Egun on,
galtzagorriak!" repeated in a log shrinks even more. Photos, archives and random data do not
shrink and grow by up to 12 %, which is why `-z` is off by default. With `-z` a pipe may carry
more than 1 KB, since it passes through `/tmp`.

At most 98 KB per transfer over RFID (with `-z`: 98 KB after packing). It is one way only: the receiver cannot tell the sender
which frames it is missing, so the sender just repeats everything. A way back is planned
(spec RP.5c) and will make RFID faster.

## Sub-GHz (planned): bandwidth, speed and the law

`gmodem info -m subghz` already shows the presets and what the region allows:

| Preset | Data rate | Bandwidth used | For |
|---|---|---|---|
| `narrow` | 1.2 kBaud | ≈ 12 kHz | the longest range, slow |
| `slow` | 4.8 kBaud | ≈ 29 kHz | long range |
| `std` | 38.4 kBaud | ≈ 80 kHz | the default |
| `wide` | 100 kBaud | ≈ 195 kHz | fast, shorter range |
| `max` | 250 kBaud | ≈ 504 kHz | short range, only wide bands |

`-f MHZ` chooses the frequency (default 433.92 MHz in the EU, 915 MHz in the US). The region
(`/etc/flipper/region`, written by root: `EU` or `US`) decides what may be sent: the whole band
the signal uses must lie inside a band the region allows for data, at no more than its power.
In the EU most of these bands also limit the share of each hour a device may send (the duty
cycle): 10 % at 433 MHz, 1 % at 868.0–868.6 MHz. A long transfer there pauses ("duty limit:
next packet in 41 s"); 869.7–870.0 MHz has no such limit and is the place for long transfers.
In the US data may not be sent at 315 or 433 MHz; 902–928 MHz is used instead.

## Good to know

- **Nothing is encrypted** over RFID, Sub-GHz and NFC: anyone near enough can receive what you
  send. Send nothing secret this way.
- `gmodem` runs only while a transfer runs and takes about 11 KB of RAM; nothing stays in memory
  afterwards.
- Exit status: 0 when everything arrived, 1 when the transfer failed or timed out, 2 when it is
  not possible (wrong options, a medium that is not there yet).
- Testing two Flippers against each other: Development Guide, part 2, chapter 6.12.
