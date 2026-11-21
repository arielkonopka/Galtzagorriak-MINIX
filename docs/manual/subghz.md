# Sub-GHz remotes: `subghz`

The Flipper's CC1101 radio hears the remotes of garage doors, gates, barriers, doorbells and
sockets (300–348, 387–464 and 779–928 MHz). The `subghz` command decodes them with the
protocols of the Unleashed firmware: 70 of them, every decoder and encoder, KeeLoq and the
other rolling codes included. It is the stock command line's `subghz` (spec Phase 9, R8.2,
issue #302).

| Part | State |
|---|---|
| `subghz rx`, `decode`, `list`, `region`, `heap` | **works**; on the Flipper `rx` decodes 31 of the 37 protocols a second (Unleashed) Flipper sends, checked 2026-11-19; real remotes are still to be tried |
| `subghz tx`, `tx_from_file`, `chat` | **in QEMU** (the frames show on the console); on the Flipper the transmitter is **(planned)**, they say "no transmitter on this device yet" |

## Installing

```
pkg install /lib/pkg/subghz_1.0.tar
```

The program is large (131 KB) and runs from flash, so it takes little RAM: about 28 KB while
`subghz rx` listens (27.5 KB measured in QEMU), all freed when it ends.

If `subghz` then says `no memory to start it`, its copy in flash is not in use yet. This
happens after the install and after every restart (a known problem, #109). Run, as root:

```
pkg flash /usr/bin/subghz     # only after a new system image was flashed
pkg bind /usr/bin/subghz
```

**Listening without gaps.** `subghz rx` takes every edge of the receiver into a 4 KB buffer while it decodes the one before, as the stock firmware does; a short burst of a remote is no longer lost while the previous one is being decoded. It needs 4 KB more heap while it listens. In a test with a Flipper sending five stock remotes, it decoded about a quarter more packets than reading frames one by one.

**Distance.** A remote held right against the Flipper can be too strong: the receiver is
overloaded and nothing decodes. A second Flipper lying back to back with this one was too
close in the check; about 1 m away worked.

## Commands

| Command | What it does |
|---|---|
| `subghz rx FREQ [-v]` | listen at FREQ Hz (e.g. `433920000`); one line per decoded packet, `Princeton 24bit Key:7C5703`. `-v` adds the protocol's own text, as stock prints it. Ctrl-C ends it and prints how many packets came |
| `subghz decode FILE [-v]` | the same for a recorded `.sub` RAW capture |
| `subghz tx KEY FREQ TE REPEAT` | send KEY (24-bit hex) as Princeton, TE µs per element, REPEAT times (stock's `subghz tx`) |
| `subghz tx_from_file FILE [REPEAT]` | send a `.sub` file at the file's `Frequency`: a key file through its protocol's encoder, a RAW file as recorded |
| `subghz edit IN OUT trim\|cut T0 T1` | rewrite a RAW `.sub`: **trim** keeps only the part between T0 and T1 (microseconds from the start of the capture), **cut** drops it and joins the two sides. OUT must differ from IN; a key file (not RAW) is refused. The file streams through, so it is never held whole in RAM. |
| `subghz chat FREQ` | text lines both ways, one packet per line (stock's `subghz chat`) |
| `subghz region [CC]` | show the radio region, or set it (once per boot; see below) |
| `subghz list` | every protocol, and whether it can decode, send, or is a rolling code |
| `subghz heap` | the heap the decoders take (about 6 KB) |

`.sub` files are the stock firmware's: copy them from a stock or Unleashed Flipper's
`/ext/subghz` to the card, for example to `/home/flipper/subghz/`. They are read a line at a
time, so a long RAW recording needs no more RAM than a short one.

## The Sub-GHz app (launcher)

**Sub-GHz** on the launcher is the stock app's menu as a window app (the launcher draws it). It
runs the `subghz` command for you and shows its lines as they come, so it needs the `subghz`
package (`pkg install subghz`); without it the window says `not installed: /usr/bin/subghz`.

| Row | What it does |
|---|---|
| Read | listens at the frequency of Radio settings; each decoded packet is a line, as `subghz rx` prints it (protocol, bits, key). Back stops. |
| Read RAW | **(planned)**: `subghz` has no RAW recorder yet. |
| Saved | the `.sub` files of `/home/flipper/subghz`, a page at a time. OK on one: **Send** (`subghz tx_from_file`), **Info** (`subghz decode`, RAW captures), **Delete**. |
| Add manually | a Princeton key: key (1–6 hex digits), TE, frequency, name; **Save** writes a stock key file (`Filetype: Flipper SubGhz Key File`). |
| Frequency analyzer | points you to the **Spectrum** app on the launcher (below). |
| Radio settings | frequency (stock's common list, 433.92 MHz first), modulation preset (AM270, AM650, FM238, FM476; written into the files you add), and the region. Kept only while the app runs. |

**Sending.** The kernel checks every frame against the region (see "The region" below). If it
refuses, the Send window shows the refusal as `subghz` words it, for example `refused: outside
the region's bands, or no region set (subghz region)`; the app never fails silently. On the
Flipper the transmitter is not written yet, so Send says `no transmitter on this device yet`.

Archive's **Open** on a `.sub` file starts this app on that file. No `.sub` files are shipped
(spec D.3).

## Editing a RAW capture

`subghz edit` trims or cuts a recorded RAW `.sub`. A RAW file is a list of signed durations in
microseconds (a positive value is the carrier on that long, a negative value off that long).
`trim` keeps only the window between two times, `cut` removes it:

```
subghz edit mysignal.sub clean.sub trim 0 120000     # keep the first 120 ms
subghz edit mysignal.sub clean.sub cut  40000 60000  # drop 40–60 ms
```

Durations that straddle a boundary are clipped, and two same-level runs left next to each other
by a cut are merged, so the result stays a well-formed RAW list. The editing logic is
host-tested (`flipper/tests/sgedit`).

## Sub-GHz Remote (launcher app)

**Sub-GHz Remote** maps up to five saved `.sub` files to the five keys of the pad (Up, Down,
Left, Right, OK); pressing a key sends that file through the `subghz` command, so the region gate
and `subghz`'s own words apply exactly as for Send above (on the Flipper the transmitter is not
written yet, so a key press says `no transmitter on this device yet`).

Map files live in `/home/flipper/subghz_remote`, in the stock format (Unleashed's
`SubGHz_Remote`):

```
Filetype: Flipper SubRem Map file
Version: 1
UP: /home/flipper/subghz/gate.sub
ULABEL: Gate
DOWN: /home/flipper/subghz/garage.sub
DLABEL: Garage
OK: /home/flipper/subghz/bell.sub
OKLABEL: Doorbell
```

`xLABEL` lines are the names shown; `xBUTTON` lines of multi-button files are ignored (the file
is sent as it is). The launcher list shows every `*.txt` map; open one to get the remote pad. No
map or `.sub` files are shipped (spec D.3): drop your own on the card.

## Spectrum: what is on the air (launcher app)

**Spectrum** on the launcher shows how strong the signals are across a band. It only listens
and never transmits; it is part of the base system (`/bin/spectrum`), no package needed.

First pick a band with Up/Down and OK:

| Band | Range |
|---|---|
| `433.05-434.79  EU SRD` | the European 433 MHz band of remotes and sensors |
| `868.0-868.6    EU SRD` | the European 868 MHz band |
| `315 MHz        US` | 314–316 MHz |
| `Custom` | any centre frequency: Left/Right pick a digit, Up/Down change it, OK starts with a 2 MHz span |

The band is cut into 32 steps, 4 pixels wide each. The app tunes each step in turn and reads
the strongest signal heard there in about 30 ms, so one sweep takes about a second. Two views,
switched with **OK**:

- **Bars**: one bar per step, from −110 dBm (empty) to −30 dBm (the full height). The dot
  above a bar is the highest level seen lately; it falls 2 dB per sweep.
- **Waterfall**: one row per sweep, the newest at the top, in four greys: white below −90 dBm,
  light from −90, dark from −70, black from −50 dBm. A remote shows as a dark spot; a signal
  that never stops shows as a dark column.

Keys in the view:

| Key | What it does |
|---|---|
| Left/Right | move the cursor (the small `v` at the top). The bottom line then shows its frequency and level for 2 seconds, e.g. `433.920 MHz  -87 dBm`. Past the edge the view moves by a quarter of its width |
| Up/Down | zoom in (half the span) or out (twice the span) around the cursor: from 64 kHz to 128 MHz |
| OK | bars or waterfall |
| Back | stop the radio and go back to the band list; Back there ends the app |

The title bar shows the span on the right (`1.74M`, `600k`). Steps outside the bands the radio
can receive (300–348, 387–464, 779–928 MHz) show a dotted line at the bottom and are not tuned.
If the radio does not answer, the app says **no radio**; Back ends it.

The keys are read between two steps, so a press is answered within one step (about 33 ms). The
radio draws about 15 mA while the app sweeps. Next to a PC the noise floor at 433.92 MHz reads
about −80 to −87 dBm, at 868 MHz about −97 dBm.

## The region: what may be sent

Every frame and packet goes through the kernel, which checks it against the region's bands and
their duty cycle for the hour (spec R8.2). With no region set **nothing can be sent**. The
region is the **country you are in**, as its ISO 3166 two-letter code. `/etc/rc` gives it to the
kernel at boot from `/etc/flipper/region`; root sets it with `region set` (it shows the table
and asks first). Only root can write that file or set the region; another user gets "only root
may set the region":

```
region list              # the countries with a table
region show PL           # a country's bands, power, duty cycle and the law each row is from
region set PL            # root: "I will use this device under the rules of PL [y/N]"; then reboot
region check US 433.92   # would a 1 s data burst be allowed there?  Sends nothing
region set -f PL         # root: the same with a table you changed yourself (a "custom table")
```

`region set` also checks that the table is the one shipped with this firmware: the build keeps
the SHA-256 of every shipped `.reg` file. If you edited `PL.reg` (or `EU.reg`, which every EU
country's table includes), `region set PL` says "custom table", prints the hashes of your file
and of the shipped one, and refuses unless you add `-f`; a custom table is your responsibility.
Every `region set` is logged to `/usr/adm/messages` with the time, your uid, "shipped" or
"custom" and the files' hashes. `set` also prints the size and CRC of the region record it
compiled: 16 bytes plus 16 for each row without `?` (192 bytes for an EU country, 528 at most).

`region` with no argument prints the code in `/etc/flipper/region` and the one the kernel uses.

| Countries | Tables (`/usr/share/region/`) | Law |
|---|---|---|
| AT, BE, BG, CY, CZ, DE, DK, EE, ES, FI, FR, GR, HR, HU, IE, IT, LT, LU, LV, MT, NL, PL, PT, RO, SE, SI, SK (the 27 EU member states) | `EU` + the country's own unchecked 870/915 MHz rows (none for DE, FR, LV, MT, NL) | Decision 2006/771/EC ((EU) 2025/105) |
| NO | `NO` | ERC/REC 70-03 (Nkom's text not read yet) |
| GB | `GB` | ERC/REC 70-03 (Ofcom IR 2030 not read yet) |
| AL | `AL` | ERC/REC 70-03 (AKEP's text not read yet) |
| BA | `BA` (no 865–868 MHz, no 869.7–870 MHz at 25 mW) | ERC/REC 70-03 (RAK's text not read yet) |
| MK | `MK` | ERC/REC 70-03 (AEK's text not read yet) |
| US | `US` | FCC part 15: 15.231, 15.247, 15.249 |
| CA | `CA` (the US numbers; no hopping row yet) | ISED RSS-210, RSS-Gen |

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
| 862–863 | 25 mW | 0.1 % | `?` (not checked) in BA |

In the US and Canada 433.92 MHz is for **remote buttons** (at most 5 s, −15 dBm) or **short
bursts** (at most 1 s, then a pause of 30 times as long and at least 10 s, −30 dBm); 902–928
MHz allows any use at −2 dBm.

What the words in `region show` mean:

- **any use**: anything within the power and the share of the hour.
- **remote buttons only**: a remote's code sent when you press a button, at most 5 s.
- **short bursts with a pause**: any data, but each send at most 1 s and then a pause.
- **?** (not checked): a row we know about but whose national text has not been checked yet;
  it is shown and never used. The 870–873 and 915–918 MHz rows are all like this for now.
  Every other row was checked against the law it names (November 2026); the texts and the
  exact places are listed in `docs/regulation/` in the source tree. The last column of
  `region show` says which: `[eu-2025-105] row 44a` is checked, `unverified: ...` is not.

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
tables). Later (planned, spec RQ.2, T32b) `region set` writes that record once into the Flipper's
internal flash (one flash write per change of region, nothing at run time), so it holds even
without the card or after a card swap, and the first-boot wizard asks for the country. Transmit on the Flipper itself is still off until the CC1101 transmit path is written.

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
