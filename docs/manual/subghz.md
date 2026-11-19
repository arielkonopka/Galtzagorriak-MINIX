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
| `subghz region [CC]` | show the radio region, or set it (once per boot; see below) |
| `subghz list` | every protocol, and whether it can decode, send, or is a rolling code |
| `subghz heap` | the heap the decoders take (about 6 KB) |

`.sub` files are the stock firmware's: copy them from a stock or Unleashed Flipper's
`/ext/subghz` to the card, for example to `/home/flipper/subghz/`. They are read a line at a
time, so a long RAW recording needs no more RAM than a short one.

## The region: what may be sent

Every frame and packet goes through the kernel, which checks it against the region's bands and
their duty cycle for the hour (spec R8.2). With no region set **nothing can be sent**. The
region is the **country you are in**, as its ISO 3166 two-letter code. `/etc/rc` gives it to the
kernel at boot from `/etc/flipper/region`; root sets it with `region set` (it shows the table
and asks first):

```
region list              # the countries with a table
region show PL           # a country's bands, power, duty cycle and the law each row is from
region set PL            # root: "I will use this device under the rules of PL [y/N]"; then reboot
region check US 433.92   # would a 1 s data burst be allowed there?  Sends nothing
```

`region` with no argument prints the code in `/etc/flipper/region` and the one the kernel uses.

| Countries | Tables (`/usr/share/region/`) | Law |
|---|---|---|
| PL, ES, FR, IE, FI, SE | `EU` + the country's own unchecked 870/915 MHz rows | Decision 2006/771/EC ((EU) 2022/180) |
| NO | `NO` | ERC/REC 70-03, Nkom |
| GB | `GB` | Ofcom IR 2030 |
| AL | `AL` | ERC/REC 70-03, AKEP |
| US | `US` | FCC part 15: 15.231, 15.247, 15.249 |
| CA | `CA` (the US numbers) | ISED RSS-210, RSS-247 |

Any other code is refused ("no table for XX; transmit stays off"). The European tables share the
ERC/REC 70-03 bands:

| Band (MHz) | Power | Share of each hour | Notes |
|---|---|---|---|
| 433.05–434.79 | 1 mW | no limit | |
| 434.04–434.79 | 10 mW | no limit | only signals ≤ 25 kHz wide (`gmodem -p narrow`) |
| 433.05–434.79 | 10 mW | 10 % | |
| 863–865, 868.7–869.2 | 25 mW | 0.1 % | |
| 865–868, 868.0–868.6, 869.7–870.0 | 25 mW | 1 % | |
| 869.4–869.65 | 500 mW | 10 % | |
| 869.7–870.0 | 5 mW | no limit | |
| 862–863 | 25 mW | 0.1 % | EU only; `?` (not checked) in NO, GB |

In the US and Canada 433.92 MHz is for **remote buttons** (at most 5 s, −15 dBm) or **short
bursts** (at most 1 s, then a pause of 30 times as long and at least 10 s, −30 dBm); 902–928
MHz allows any use at −2 dBm.

What the words in `region show` mean:

- **any use**: anything within the power and the share of the hour.
- **remote buttons only**: a remote's code sent when you press a button, at most 5 s.
- **short bursts with a pause**: any data, but each send at most 1 s and then a pause.
- **?** (not checked): a row we know about but whose national text has not been checked yet;
  it is shown and never used. The 870–873 and 915–918 MHz rows are all like this for now.

The Flipper sends at most +10 dBm: the "PA" column is the step it uses. A send outside the
bands says "refused: outside the region's bands"; one past the hour's share says "the band's
duty cycle for this hour is used up", and `gmodem` waits. A long transfer in Europe belongs on a
band with no limit. Unleashed lifts these limits, and the stock firmware's own region data allows
more than the law (+12 dBm and 50 % everywhere); MINIX keeps the legal limits for every program,
ported ones included. **You are responsible for using the radio legally: when you travel, set
the region of the country you are in.**

Today the kernel still has its own short band list: for Europe 433.05–434.79 (10 %),
868.0–868.6 (1 %), 868.7–869.2 (0.1 %), 869.4–869.65 (10 %) and 869.7–870.0 MHz; for the US and
Canada the stock bands (304.1–321.95, 433.05–434.79, 915–928 MHz, 50 %). It checks band and duty
cycle only; the power, the bandwidth and the remote/burst rules of the tables are checked by
`region check` (`gmodem` still uses its own short list, spec P.4, until T32a moves it to the
tables). Later (planned, spec RQ.2, T32b) `region set` writes the table once into the Flipper's
internal flash, so it holds even without the card, and the first-boot wizard asks for the
country. Transmit on the Flipper itself is still off until the CC1101 transmit path is written.

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
