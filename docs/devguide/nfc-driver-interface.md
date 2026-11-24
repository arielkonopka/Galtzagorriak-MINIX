# The `/dev/nfc` driver interface

This is the contract between the NFC protocol stack (`src/lib/libnfc`, written and
host-tested on the cloud lane, spec Phase 14) and the ST25R3916 driver (the kernel, spec
Phase 13, **lane:local** — the local agent writes it against the hardware). The stack above
is finished and tested over a software mock before the driver exists; the two meet here.

The header is [`include/minix/nfc.h`](../../include/minix/nfc.h). The library never touches
`/dev/nfc` directly: it speaks to one `nfc_xfer_t` (`src/lib/libnfc/nfc_xfer.h`), and the only
job of the on-Flipper backend is to fill that struct in with ioctls on this device. A host
test and a QEMU mock fill the same struct a different way, so the same protocol code runs in
all three places.

> **Scope.** The cloud worker defines this interface and does **not** write the driver
> (Autonomy rule in `docs/spec/next-tasks.md`: a new hardware path merges off by default). A
> `/dev/nfc` that does nothing but return `ENOSYS`/`ENOTTY` is the starting point the local
> agent fills in.

## The model

`/dev/nfc` (group `nfc`) is the RF front end. One open at a time; it is exclusive with
`/dev/lfrfid` (shared SPI1 and antenna path). The chip moves whole frames between its 512-byte
FIFO and the user buffer with no SRAM copy; the kernel keeps only a small mode word, so the
device stays inside the Phase 13 budget (**kernel ≤ +200 B**).

Everything is counted in **bits**, not bytes: a REQA is a 7-bit short frame, MIFARE Classic
Crypto-1 needs per-byte parity. The transfer flags (`NFC_XF_*`) are passed straight from the
library through `NFCIOCXFER`; the same numeric values are defined in both headers (kept in
step, a benign redefinition if both are included).

## Reader (poller) flow

| step | call | meaning |
|---|---|---|
| raise field | `ioctl(fd, NFCIOCFIELD, &tech)` | `tech` is `NFC_TECH_A|B|F|V`; 0 drops the field |
| poll | `ioctl(fd, NFCIOCPOLL, &seen)` | one poll cycle; `seen` = technologies that answered |
| transceive | `ioctl(fd, NFCIOCXFER, &io)` | send `io.tx` (`tx_bits`), wait `io.fwt` fc units, receive into `io.rx`; `io.rx_bits` set on return |

`struct nfc_xfer_io` carries the two buffers (caller's memory), the bit counts, the frame-wait
time in `fc` units (`NFC_FC` = 13.56 MHz; 0 means the technology default) and the flags:

- `NFC_XF_CRC_TX` / `NFC_XF_CRC_RX` — the chip appends / checks the ISO 14443 CRC.
- `NFC_XF_PARITY` — keep per-byte parity bits (transparent mode, for Crypto-1). Both
  directions are then a stream of 9 bits a byte (the data LSB first, then its parity bit),
  `tx_bits`/`rx_bits` count all of them, and a short answer (a 4-bit ACK/NAK) comes back as
  it is. The driver does this in the poller without `NFCIOCTRANSP` (the chip only stops
  making and checking parity); a CRC flag together with it is refused (`ENOTTY`).
- `NFC_XF_NO_RX` — send only.
- `NFC_XF_SHORT` — a 7-bit short frame (REQA / WUPA).

Return as errno: `ETIMEDOUT` no answer, `EIO` CRC/parity/framing, `EOVERFLOW` answer longer
than `rx` room, `ENOTTY` the mode is not supported by this front end.

## Listener (card emulation) flow — R13.2

`ioctl(fd, NFCIOCLISTEN, &l)` with the anticollision values (`atqa`, `sak`, `uid[]`,
`uid_len`, `tech`). The chip answers REQA/ATQA/SELECT by itself from its registers; after the
card is selected, `read(fd, …)` returns each later reader frame and `write(fd, …)` sends the
response within the chip's frame-wait timer. A zero `uid_len`, or `close`, stops listening.

## Transparent mode — R13.3

`ioctl(fd, NFCIOCTRANSP, &on)` turns on raw bit mode for MIFARE Classic emulation, where every
bit and its parity must be under software control. While on, `NFCIOCXFER` with `NFC_XF_PARITY`
moves bits unframed.

## Capabilities and test hooks

`ioctl(fd, NFCIOCFEAT, &bits)` reports what the front end can do: the `NFC_TECH_*` it supports
OR'd with `NFC_CAN_LISTEN`, `NFC_CAN_TRANSP`, `NFC_CAN_FIELDDET` (external-field detect,
R14.2.1). A mock reports less than the chip, so the app can grey out what is missing.

`ioctl(fd, NFCIOCFAKE, &f)` (QEMU and the host mock only; `ENOTTY` on the Flipper) makes the
next transceive's answer come from `f.rx` instead of the air, so the whole stack and the `nfc`
command run end to end under QEMU against a scripted card.

## What the library expects of the driver (checklist for the local agent)

1. `NFCIOCFIELD`, `NFCIOCPOLL`, `NFCIOCXFER` for NFC-A at least (AC13.1 reads a UID).
2. CRC and parity handled per the flags; bit-granular lengths.
3. Frame-wait time honoured from `fwt` (the chip's own timer; no CPU spin — responsiveness
   rule 0.8, nothing long in the kernel fast path).
4. `NFCIOCLISTEN` + `NFCIOCTRANSP` for emulation (AC13.2, AC13.3); may come in a later step.
5. `NFCIOCFEAT` truthful, so the app degrades gracefully before (4) exists.
6. Kernel ≤ +200 B: keep the per-open state to the small mode word; no resident frame buffers
   (the chip FIFO and the user buffer are the only copies).

The protocol side (authentication, parsers, `.nfc` files, the dictionary attack, `mfkey`) is
all above this line and already done or in progress on the cloud lane; the driver only has to
make these ioctls true.
