# Sub-GHz remotes: `subghz`

The Flipper's CC1101 radio hears the remotes of garage doors, gates, barriers, doorbells and
sockets (300–348, 387–464 and 779–928 MHz). The `subghz` command decodes them with the
protocols of the Unleashed firmware: 70 of them, every decoder and encoder, KeeLoq and the
other rolling codes included. It is the stock command line's `subghz` (spec Phase 9, R8.2,
issue #302).

| Part | State |
|---|---|
| `subghz rx`, `decode`, `list`, `region`, `heap` | **works**; `rx` on the Flipper still needs its check with real remotes (device checks, #109) |
| `subghz tx`, `tx_from_file`, `chat` | **in QEMU** (the frames show on the console); on the Flipper the transmitter is **(planned)**, they say "no transmitter on this device yet" |

## Installing

```
pkg install /lib/pkg/subghz_1.0.tar
```

The program is large (131 KB) and runs from flash, so it takes little RAM: about 28 KB while
`subghz rx` listens (27.5 KB measured in QEMU), all freed when it ends.

## Commands

| Command | What it does |
|---|---|
| `subghz rx FREQ [-v]` | listen at FREQ Hz (e.g. `433920000`); one line per decoded packet, `Princeton 24bit Key:7C5703`. `-v` adds the protocol's own text, as stock prints it. Ctrl-C ends it and prints how many packets came |
| `subghz decode FILE [-v]` | the same for a recorded `.sub` RAW capture |
| `subghz tx KEY FREQ TE REPEAT` | send KEY (24-bit hex) as Princeton, TE µs per element, REPEAT times (stock's `subghz tx`) |
| `subghz tx_from_file FILE [REPEAT]` | send a `.sub` file at the file's `Frequency`: a key file through its protocol's encoder, a RAW file as recorded |
| `subghz chat FREQ` | text lines both ways, one packet per line (stock's `subghz chat`) |
| `subghz region [EU\|US\|JP]` | show the radio region, or set it (once per boot; see below) |
| `subghz list` | every protocol, and whether it can decode, send, or is a rolling code |
| `subghz heap` | the heap the decoders take (about 6 KB) |

`.sub` files are the stock firmware's: copy them from a stock or Unleashed Flipper's
`/ext/subghz` to the card, for example to `/home/flipper/subghz/`. They are read a line at a
time, so a long RAW recording needs no more RAM than a short one.

## The region: what may be sent

Every frame and packet goes through the kernel, which checks it against the region's bands and
their duty cycle for the hour (spec R8.2). With no region set **nothing can be sent**.
`/etc/rc` sets it at boot from `/etc/flipper/region` (two letters); root can write that file:

```
echo EU > /etc/flipper/region       # then reboot, or once: subghz region EU
```

| Region | Bands (MHz) and the share of each hour you may send |
|---|---|
| EU | 433.05–434.79 (10 %), 868.0–868.6 (1 %), 868.7–869.2 (0.1 %), 869.4–869.65 (10 %), 869.7–870.0 (100 %) |
| US | 304.1–321.95, 433.05–434.79, 915–928 (50 %) |
| JP | 312–315.25, 920.5–923.5 (50 %) |

The region is set once per boot: a second `subghz region` with another region is refused. A
send outside the bands says "refused: outside the region's bands"; one past the hour's share
says "the band's duty cycle for this hour is used up". Unleashed lifts these limits; MINIX keeps
them for every program, ported ones included.

## Rolling codes and the keystore

**Warning: sending a rolling code (KeeLoq, Nice Flor-S, Came Atomo, Somfy and the others
`subghz list` marks "rolling code") from the Flipper moves the code ahead of the real remote.
The receiver may then ignore your real remote until you pair it again.** `tx_from_file` writes
the new counter back to the file, as stock does, so the file stays usable, but the real remote
falls behind each time. Only send rolling codes of systems you own and can re-pair.

Reading a rolling code fully (its button and counter) needs the manufacturer keys. Galtzagorriak
MINIX ships **no keys** (spec D.3): the decoders are there, the key file is yours to provide.
Without it, KeeLoq remotes are still recognised and their fixed part shown; the encrypted part
is not decoded and cannot be sent anew.

The keys are read from `/home/flipper/subghz/assets/`:

| File | What |
|---|---|
| `keeloq_mfcodes`, `keeloq_mfcodes_user` | KeeLoq manufacturer keys |
| `nice_flor_s`, `alutech_at_4n` | the rainbow tables of those two protocols |

Only **plain** files load. The stock firmware's own files are encrypted with a key kept inside
the radio core (CPU2), which MINIX cannot use, so `subghz` refuses them. A plain key file looks
like this, one `KEY:TYPE:NAME` line per manufacturer key (as the stock files are once
decrypted):

```
Filetype: Flipper SubGhz Keystore File
Version: 0
Encryption: 0
0123456789ABCDEF:1:Manufacturer_name
```

At most 512 keys are kept. Keep the file readable only by you (`chmod 600`).

## Testing it without a remote

In QEMU the transmitter prints what it sends as console lines (`sg> tx 433920000 …`), and the
test tool `/usr/tests/sgfake FILE` (package `tests`) plays a recorded `.sub` capture into a
running `subghz rx`. `tools/nattest.py subghz` does both; the developer guide (3.10b, 6.10) has
the details.
