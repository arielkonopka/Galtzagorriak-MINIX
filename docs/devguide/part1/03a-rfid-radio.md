# 3a. How the 125 kHz RFID "radio" works

This chapter is for someone who knows C and MINIX but not radio. After it you should understand
why `rfid` behaves as it does on the Flipper, and what to check when a read or an emulation
fails. It follows the code (`flipper/hal/flipper/board.c`, `include/minix/rfid.h`,
`flipper/cmds/rfid.c`) and the measurements of issue #210 and [chapter 6.4](../part2/06-two-flipper-tests.md).
Anything that is an assumption, not something the code or a measurement shows, is marked
**(assumption)**. The device interface itself is in [3.10a](03-devices.md).

## 3a.1 The physics, in plain terms

A 125 kHz tag has no battery. The reader's coil makes an alternating magnetic field at
125 kHz (a "carrier"); the tag's own coil picks up a little of it and that powers the tag's chip.
Tag and reader are a transformer with a very loose coupling.

The tag talks back by **load modulation**: its chip switches a load on and off across its coil.
The reader feels this as a tiny change of the current in its own coil, which shows as a slight
rise and fall of the carrier's amplitude. A reader circuit follows that amplitude (an *envelope
detector*), and the pattern of rises and falls is the tag's data.

Two things follow:

- **Distance and alignment are everything.** The coupling falls off steeply with the distance
  and with how well the two coils face each other. A tag that reads at 2 mm may not read at
  10 mm, and a tag turned 90° may not read at all.
- **The signal is small and slow.** What the reader sees is not a clean digital wave but a
  smeared envelope, so the software has to expect glitches, a settling time and stretched
  edges (3a.5).

## 3a.2 The Flipper's hardware path

Read the path from the field to the CPU (`hal_rfid_read` in `board.c`; the stock reference is
`targets/f7/furi_hal/furi_hal_rfid.c`):

| Step | Pin / block | What it does |
|---|---|---|
| carrier | TIM1 channel 1N on **PB13** (AF1) | 125 kHz at half duty (PSK: 62.5 kHz at 25 %; FDX-B: 134.2 kHz) drives the coil |
| antenna pull | **PA2** | held low when reading ("antenna not pulled"); in emulation it is the load, below |
| carrier node | **PA15** | a node on the antenna; analog while reading, TIM2's ETR input (AF2) while emulating |
| envelope | **PC5** into **COMP1** | the comparator compares the envelope with half of VREFINT, with high hysteresis (medium speed) |
| capture | COMP1's output to **TIM2 channel 4** | TIM2 runs free at 1 MHz; both edges are captured by DMA (DMA1 channel 4) as 32-bit timestamps into the caller's buffer |

PB14, the iButton line, is held low meanwhile, as in the stock firmware. TIM1 and TIM2 are also
the infrared transmitter's: the kernel lets only one user have them, the other gets `EBUSY`.

**Emulation turns it round** (`hal_rfid_emulate`): the *reader* now makes the field, so our
carrier output stays off and TIM2 counts the reader's own carrier (external clock mode 2 on ETR,
fed from PA15, inverted as stock). The tag answers by loading the antenna, which is PWM on the pull
pin PA2 (TIM2 channel 3): a segment's length comes from ARR, how long the pull is on from CCR3.
Two circular DMA channels feed those two values from the caller's buffers, so the whole pattern
repeats for as long as the field lasts with no CPU work, and since the clock is the reader's own
it cannot drift.

## 3a.3 What the comparator sees: the modulations

The comparator turns the envelope into a stream of edge times (in µs). What a tag's data look
like in that stream depends on its modulation:

- **ASK, Manchester or biphase** (EM4100 and most others): the load is on for half a bit or
  off for half a bit. A bit is 64 carrier periods (RF/64, 512 µs), 32 (RF/32) or 16 (RF/16);
  a bit has an edge in the middle (Manchester) or at its start (biphase), so the durations are
  one or two half-bits.
- **FSK** (HIDProx, AWID, Pyramid, ...): the tag switches the load on a *rate*, not on a level:
  a "0" is a burst of cycles of 8 carrier periods (64 µs), a "1" cycles of 10 (80 µs). The
  stream is therefore **many short edges**: about 27 000 edges a second.
- **PSK** (Indala, Keri, Idteck, Nexwatch, ...): the reader switches to 62.5 kHz at 25 % duty,
  and the tag's data is the **phase** of a subcarrier: the bits are where the phase of the
  cycles jumps.

The decoders (the stock `lib/lfrfid`, `src/lib/lfrfid`) turn these durations into protocol and
data; the kernel never decodes anything.

## 3a.4 The software path in MINIX

```
tag -- field -- COMP1 -- TIM2 capture -- DMA -- /dev/rfid -- rfid / liblfrfid
```

- **`/dev/rfid`** (`include/minix/rfid.h`). A plain `read()` raises the field, captures edge
  times for about half a second and drops the field: one reading, the first level unknown.
  `write()` plays field on/off times to program a T5577 or EM4305. `RFIDIOCFIELD` holds the
  field between a write and the read after it (a T5577 resets if the carrier drops).
  `RFIDIOCCARRIER` chooses 125, 62.5 or 134.2 kHz.
- **`RFIDIOCSTREAM`**: capture without end, the way a long FSK or PSK frame needs. The DMA
  writes timestamps round and round into a ring you give it (even, 64 to 65534 words, pinned);
  `read()` blocks until half of the ring is full and returns 0 or 1, the half to use. If you
  are more than half a ring late you get `EOVERFLOW` once, and the next whole half: an edge is
  never lost silently.
- **`RFIDIOCEMU`** plays a list of segments counted in the reader's carrier periods (at most 512)
  and **`RFIDIOCEMURUN`** does the same for long frames (FSK, PSK) with run lists refilled while
  it plays. Your arrays stay pinned until you stop it.
- **liblfrfid** is the Unleashed `lib/lfrfid` with its decoders and encoders; `rfid` (the
  command; `flipper/gui/lfrfid.c` is the app) feeds it the durations and chooses the strategy:
  which carrier, how long, which polarity.

The `rfid` read strategy is: raise the field, wait for the envelope to settle (3a.5), despike
the edge times, and feed the decoders. The first level is unknown, so each decoder is fed with
both polarities, until the CPU cannot keep up (3a.5).

## 3a.5 What we measured on the device

The numbers are from #210 and chapter 6.4; keep them when you change the code.

- **Envelope stretch.** A reader's envelope detector stretches a load by about two carrier
  periods. That is why emulation trims each load: `EMU_TRIM 2` in `lfrfid_read.c`. Without the
  trim a real reader sees every bit too long.
- **Settling.** After the field comes on, the envelope needs about **300 ms** before FSK cycles
  cross the comparator's threshold; until then it gives long pulses that are not the tag's
  cycles. The stock firmware waits 450 ms (`LFRFID_WORKER_READ_STABILIZE_TIME_MS`), and `rfid`
  skips the first `RS_SETTLE_US` (450 ms) of a read. Before that wait was added, FSK tags read
  as "no tag (no signal)".
- **Spike filter.** The stock reader drops pulses of 16 µs or less
  (`LFRFID_WORKER_READ_MIN_TIME_US`); `rfid`'s `despike()` does the same and merges the level
  before and after the spike. A spike inside a PSK bit otherwise reads as two phase changes.
- **CPU load.** FSK is about 27 000 edges a second: half of a ring (512 edges) every 19 ms.
  Feeding every edge to every decoder in both polarities took longer than that: **38 of 40 ring
  halves overflowed** and nothing was read. So both polarities are fed only until halves start
  to get lost; then one polarity at a time, the other after `RS_SLICE_US` (600 ms). The receive
  hardware is fine; the strategy was the fault. With one polarity decoded, a real T5577 fob
  written as HIDProx by MINIX (`rfid write HIDProx 1A2B3C4D5E60`) is read by the reference
  Unleashed and by MINIX.
- **Distance, two Flippers back to back** (the silicone cases on; chapter 6.4 has the table).
  ASK and FSK want about **5 mm** of spacer between them (a "thin" spacer, or a "thick" one
  of about 10 mm for some), PSK wants **contact**. For reading, ASK and PSK tags read at every spacer; each FSK row only at some. The spacer matters
  most for emulation.
- **Canonical data.** Some protocols do not carry all their data bits: HID Prox has 44 of 48 bits
  of payload, so a reader leaves the rest 0. Compare data after reading the card back, not before.

## 3a.6 Troubleshooting

| Symptom | Likely cause | What to try |
|---|---|---|
| `no tag (no signal)` with an FSK tag | the envelope had not settled (the first ~300 ms), or the tag is too far | Hold the tag flat against the back of the Flipper; read again. **(assumption: where the coil sits on the back is not measured here)** |
| Nothing read at all | PSK tags need the 62.5 kHz field and FDX-B animal tags 134.2 kHz; the 125 kHz field does not read them | Let `rfid` try its list; with the API, `RFIDIOCCARRIER`. |
| Reads at one distance only, not closer | the signal may be too strong at contact, so the comparator saturates **(assumption)** | Move the tag a few mm away; try a spacer. **(assumption: seen with two Flippers, not measured with every tag)** |
| Emulation reads at one distance only | the same, in the other direction: the reader's coupling, and the stretch trim | Try the thick spacer (ASK/FSK) or contact (PSK) (6.4). |
| Last bits wrong or zero | the protocol does not carry them (canonical data, 3a.5) | Compare the decoded data of a real tag read by the same reader. |
| `EOVERFLOW` on a stream | the process fell more than half a ring behind (FSK: a half every 19 ms) | Use a bigger ring, decode one polarity at a time, or do less per half. |
| `EBUSY` | IR transmit, the ADC stream or another RFID user holds TIM1/TIM2 | Close the other user first. |
| A T5577 write succeeds but the card reads as before | the field dropped between write and read-back (the chip reset), or the card has a password | Hold the field with `RFIDIOCFIELD`; try the dictionary passwords. |
| Works with the reference Unleashed, not with MINIX | read strategy or timing, not the hardware | Read the same card with both and compare the data; the capture (`RFIDIOCSTREAM`) timings are the place to look. |
