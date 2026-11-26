# Card detect

**Card detect** (Tools on the launcher) answers one question: what is this card or key fob,
a 125 kHz RFID tag, a 13.56 MHz NFC card, or both in one body (the dual-frequency fobs)? It
reads whatever answers and saves it as the stock files, so you can use them later in the RFID
and NFC apps or in Archive.

## Taking turns

The Flipper has one coil for each band and **cannot run the two fields at the same moment**.
So the detector takes turns: a try of the 125 kHz reader (`rfid`), then a try of the 13.56 MHz
reader (`nfc -r`), again and again, until one of them reads something. A dual fob lying on the
back of the Flipper answers in both turns, so it is found as *RFID + NFC* within a few
seconds; if only one band answered, the detector makes one more turn to look for the other
one. **Back** stops it at any moment.

## Reading and saving

Detect shows *RFID only*, *NFC only* or *RFID + NFC* with the type and the number (or UID) of
each side. What was read is already saved, with a shared name:

| Side | File |
|---|---|
| RFID | `/home/flipper/lfrfid/cd001.rfid` |
| NFC | `/home/flipper/nfc/cd001.nfc` |

The number (`cd001`, `cd002` ...) is the first free one; a dual fob gives a pair with the same
name, a single tag gives one file. The names stay short (13 characters with the extension)
for a card whose `/home` still has MINIX V2 names; rename the files in Archive if you like (a
pair stays a pair in the list of Card detect only while the names stay alike).
When the result is on the screen: **OK** writes it to blank tags (below), **Left** forgets it
(deletes the files) and scans again, **Right** keeps it and scans for the next card.

## Saved

Saved lists every `cdNNN` once (*R* RFID, *N* NFC, *R+N* both). OK on one: **Write pair** or
**Delete** (deletes both files).

## Writing a pair

**Write pair** goes one after the other: first *1: RFID* (hold a blank 125 kHz fob; this is
`rfid write -f`, so a T5577 or EM4305 gets the saved number, and the password rule of
[the RFID copier](devices.md) applies), then the NFC side.

> **Not yet.** The Flipper has no NFC card writer for real cards so far (see
> [NFC](nfc.md), *Copying a card*), so the NFC step only says so and keeps the `.nfc` file.
> The RFID half is written today.

No pins or wires are needed; the readers are the ones in the back of the Flipper.

## Memory

The app keeps nothing in memory: it starts `rfid` and `nfc` one after the other, and the
saved list is read from the folders a row at a time.
