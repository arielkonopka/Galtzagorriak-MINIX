# NFC

Reading, saving and emulating 13.56 MHz contactless cards (NFC): the `nfc` app, the `mfkey`
key-recovery command and the MIFARE Classic dictionary. Behaviour follows the stock Flipper
firmware (spec Phase 14).

> **Status.** The protocol layer is in the code and tested on a PC (`src/lib/libnfc`): card
> activation (ISO 14443-3A: request, anticollision, select), telling a card's type from its
> SAK, MIFARE Classic authentication, the dictionary attack and block read, and the MIFARE
> Ultralight/NTAG page reader all run against software card models. The parts that need the
> radio — reading a *real* card, emulation, the field detector — wait for the ST25R3916 driver
> (spec Phase 13), so live reading, emulation and the field detector are **(planned)** until
> that driver lands; the `nfc` **app** runs today with test cards (`nfc -L`, below). Saved-card files already work on dumps. The full `mfkey` key recovery
> (Crypto-1 nonce/LFSR recovery) is still being ported and is **(planned)**.
>
> The stock (Unleashed) protocol stacks in `src/lib/nfc` now run on the PC as well, unchanged,
> through a small adapter (#404): **ISO 14443-4A** (RATS/ATS, I-blocks, the card's "wait"
> request), **ISO 14443-4B**, **MIFARE DESFire** (version, free memory, applications and every
> file the card lets you read without a key; DESFire Light's wrapped commands too) and
> **ISO 15693 / ICODE SLIX** (UID, memory blocks, lock status, NXP signature), and stock's
> MIFARE Classic reader with known keys. On top of them, 25 of stock's **card parsers** turn a
> dump into readable text (balance, trips, card number): aime, bambu, bip, charliecard,
> clipper, csc, gallagher, hi, hid, hworld, itso, kazan, metromoney, microel, mizip, myki,
> opal, smartrider, social_moscow, troika, two_cities, umarsh, washcity, zolotaya_korona and
> zolotaya_korona_online. A parser that knows its card's keys (MiZIP and Microel derive them
> from the UID) reads the card with them. Dates are shown as day.month.year, 24 h.
>
> Part 5 (#412) adds stock's **MIFARE Ultralight / NTAG** reader (version, every page,
> signature, counter; it tries the default password only when the card allows unlimited
> tries), **FeliCa** (IDm/PMm, the FeliCa Lite-S blocks), **ST25TB / SRIX** (every block and the
> system block) and **EMV** (the payment application's label, card number, expiry, PIN tries;
> shown on screen only, never written to a file by the reader). MIFARE Ultralight C (3DES) and
> Ultralight AES authentication now use a real cipher (the AES and DES of Mbed TLS, vendored in
> `src/lib/mbedtls`, part 6 #447), so their protected pages are read once the right key is in
> your dictionary. More parsers: all_in_one, ndef (Ultralight and MIFARE
> Classic tags: URLs, text, Wi-Fi, vCards), sonicare, trt, ventra, aic, mykey, disney_infinity,
> banapass and emv, 40 in all.
>
> **ID lists are your own files.** plantain, sevppk_tk, sk_tk, szppk_so and skylanders show
> station or figure names from lists, and emv shows country and currency names; none of these
> lists is shipped (D.3). Put your own in `/home/flipper/nfc/assets/` with the names stock uses
> (`sz_id.nfc`, `sev_id.nfc`, `sk_id.nfc`, `skylanders.nfc`, `country_code.nfc`,
> `currency_code.nfc`, `aid.nfc`). Without the file the card is still shown, with a line saying
> so:
>
> ```
> No ID list found (/home/flipper/nfc/assets/skylanders.nfc), showing the raw card.
> No ID list found (/home/flipper/nfc/assets/sz_id.nfc): codes shown instead of names.
> ```
>
> Part 7 (#451) adds the **service scanner** (below), the **Type 4 Tag** NDEF reader in the app,
> and, tested on a PC, stock's Type 4 Tag and **MIFARE Plus** pollers (EV1/EV2, 2K/4K, security
> level 0–3; the SL3 sector read with your AES keys is in the vendored poller but not yet in the
> app, **(planned)**).
>
> Like the rest, all of this reaches real cards once the driver is there.

## Card files

Cards are stored as stock stores them, as a Flipper NFC device file (`*.nfc`, a text file)
under `/home/flipper/nfc`. A file stock saved loads here, and a file saved here loads on a
stock Flipper (round-trips are checked in `flipper/tests/nfc`). MIFARE Classic and
Ultralight/NTAG dumps are modelled; the common header (type, UID, ATQA, SAK) loads for any
card type. The reader accepts the older file versions too, and writes the current one.

Example — a saved transport card and an access badge:

```
/home/flipper/nfc/garraio-txartela.nfc    # garraio txartela = transport card
/home/flipper/nfc/sarbide-bbox.nfc        # sarbide = access
```

## The key dictionary (MIFARE Classic)

To read a MIFARE Classic card the reader tries keys from a dictionary, exactly as stock does.
Two files under `/home/flipper/nfc/assets`:

- `mf_classic_dict.nfc` — the system dictionary (the public keys stock itself ships; kept
  under the licensing rule D.3).
- `mf_classic_dict_user.nfc` — **your** keys, one 12-hex-digit key per line, `#` starts a
  comment. Add the keys you know for your own cards here:

```
# nire gakoak (my keys) - /home/flipper/nfc/assets/mf_classic_dict_user.nfc
A0B1C2D3E4F5
112233445566
```

The dictionary is read a line at a time and never loaded into memory whole, so a long user
dictionary costs no RAM. **(planned: the live read; the attack loop and the streamer are in the
code and tested against a software card.)**

## `mfkey` — recovering keys from a reader

When a card's key is not in any dictionary but you can present the card to a reader that knows
it, `mfkey` recovers the key from the authentication the reader does (stock's "Mfkey" / nonce
collection). It runs as an ordinary command (swappable when idle), nonce-collection files in,
keys out. The recovery itself (the Unleashed `mfkey` engine) is in the code and host-tested in
two forms: **mfkey32** (two reader authentications on one sector) from a stock-format
`.mfkey32.log`, and **static_nested** (one tag-static sector, two tag nonces with their
keystream words) from a stock nested-nonce log. Each recovers a planted key in one to two
seconds per key on the host; mfkey32 also recovers the all-`F` transport key. A recovered key
is added to your user dictionary (`mf_classic_dict_user.nfc`), skipping any key already there,
so the next read finds it; the file is appended a key at a time and never loaded whole. No keys
are shipped (D.3), and — a rule of this port — any key or password a test writes to a card is
added to the dictionary first. **(planned: the on-device nonce collection; the static-encrypted
attack, which needs a per-card candidate-key dictionary.)**

```
mfkey nonneak.txt            # nonneak = nonces; prints the recovered keys, adds them to the dictionary
```

## The `nfc` app

**NFC** on the launcher (icon `@nfc`) is a window app: the launcher draws it, so it looks and
moves like Archive and the settings. One list a screen, OK opens, Back goes up:

- **Read** — hold a card to the back of the Flipper and press OK. The app finds the card,
  tells its type (MIFARE Classic 1K/4K/Mini, Ultralight/NTAG, a bare ISO 14443-3A tag) and
  shows UID, ATQA and SAK. For a MIFARE Classic it tries the keys of your dictionaries
  (`mf_classic_dict_user.nfc` first, then `mf_classic_dict.nfc`, read a line at a time) and
  reads every sector it has a key for: **Keys found** and **Blocks read** say how far it got;
  without a dictionary it says *No key dictionary* and keeps the UID. **Save** writes the card
  to `/home/flipper/nfc/<type>_<UID>.nfc` (`_2`, `_3` … when the name is taken), the stock
  format, so Archive's NFC tab lists it.
- **Detect reader** — needs `/dev/nfc` in listen mode; until the driver comes (Phase 13) the
  screen says so.
- **Saved** — your `.nfc` files, a page at a time: OK on one shows its type, UID and how much
  of it is known; **Delete** asks first.
- **Extra actions → MIFARE Classic keys** — how many keys the system and user dictionaries hold
  (*none* when a file is missing). No keys are shipped: put your own files in
  `/home/flipper/nfc/assets/`.
- **Add manually** — a type (*Classic 1K* or *NTAG215*), a UID in hex (4 or 7 bytes; NTAG215
  needs 7) and a name; **Save** writes a blank card of that type with that UID.
- **Scan services** — which services (applications) an ISO 14443-4 card answers to: a bank
  card, a passport, a transport card, a badge, a phone. See *The service scanner* below.

On **Read**, a card that speaks ISO 14443-4 (SAK bit 0x20) shows as *ISO14443-4A*; if it is an
NFC Forum **Type 4 Tag** (a DESFire or NTAG4xx with an NDEF application, a phone's tag emulation)
the app reads its NDEF message and shows the first record: *URI https://…*, *Text en: …*,
*MIME text/vcard*. Read only: nothing is written, no password is tried. Saving keeps the UID,
ATQA and SAK (a full Type 4 Tag file as stock writes it is **(planned)**).

## The service scanner

**Scan services** in the `nfc` app (and `nfc scan` on the command line) tells you which
services a contactless chip card offers, without reading anything from them. Hold the card to
the back of the Flipper and press OK. The app wakes the card (ISO 14443-3A, then RATS) and sends
one ISO 7816-4 **SELECT** command per AID (application identifier) from a dictionary file, one
after another, and notes each AID the card accepts. **Nothing else is sent**: no READ, no PIN,
no authentication, no payment command. SELECT changes nothing on a card; it only picks the
application a next command would go to, and there is no next command.

The result screen shows ATQA and SAK, the card's ATS, then one line per service found: its name,
with its group on the right. OK on a service tells what it is: the group's one-line description
from the dictionary (*Bank cards: what a shop terminal talks to when you tap*), the AID, and the
card's answer (*selected*, or *blocked* when the card has the application but has disabled it).
A card that is not ISO 14443-4 (a MIFARE Classic, an Ultralight) says *Not an ISO 14443-4 card*.

```
$ nfc scan -L                  # the loopback test card
UID 04517A22316E80 ATQA 4403 SAK 20
ATS 0578807002
Payments     325041592E5359532E4444463031     9000 Payment directory (PPSE)
Payments     A0000000031010                   9000 Visa credit/debit
Access       A0000006472F0001                 9000 FIDO U2F security key
Phones       D2760000850101                   9000 Type 4 Tag (NDEF)
nfc scan: 26 AIDs tried, 4 answered (/usr/share/nfc/aid_scan.nfc)
```

`nfc scan -L` scans the loopback test card (no driver needed), `nfc scan` the card on
`/dev/nfc`; `nfc scan FILE` uses another
dictionary. Up to 16 services are listed on the screen; `nfc scan` prints the count of the rest.

### The AID dictionary

The scanner reads `/home/flipper/nfc/assets/aid_scan.nfc` (on the SD card, `/ext/nfc/assets`) if
it is there, else the **example** the base package installs, `/usr/share/nfc/aid_scan.nfc`. The
file is read a line at a time and never loaded whole, so a long list costs no memory. It is a
text file:

```
# a comment
[Payments] Bank cards: what a shop terminal talks to when you tap
A0000000031010 Visa credit/debit
A0000000041010 Mastercard credit/debit

[Access] Badges and keys: doors, logins and signing
A0000006472F0001 FIDO U2F security key
```

- `[Group] description` starts a group. The description (up to 63 characters) is the
  "what is this?" line the app shows for the services of that group.
- An AID line is the AID in hex, 5 to 16 bytes, no spaces inside, then blanks, then the
  service's name (up to 27 characters shown). Upper or lower case hex.
- `#` starts a comment line. Bad lines are skipped (the scan counts them).

The example holds only public AIDs (payment schemes, ICAO e-passports, the ISO 18013-5 mobile
driving licence, Calypso, PIV, FIDO U2F, OpenPGP, SEOS, the German eGK, NFC Forum Type 4 Tag,
the Apple and Google wallet pass services), with no keys or secrets (rule D.3). **To add your own
AIDs**, copy it and edit the copy:

```
cp /usr/share/nfc/aid_scan.nfc /home/flipper/nfc/assets/aid_scan.nfc
mined /home/flipper/nfc/assets/aid_scan.nfc     # or edit it on the PC (/ext/nfc/assets)
```

DESFire transport cards (Opal, myki, ITSO and others) keep their data in 3-byte DESFire
applications that SELECT by AID cannot reach; **Read** shows those.

**On the device** the scan needs the `/dev/nfc` driver (Phase 13) like Read; until it is there
the app says *No NFC device* and `nfc scan -L` shows the flow with the test card.

**Who may use the reader.** `/dev/nfc` belongs to group `nfc` (14). The app is set-gid `nfc`,
but it opens the device only for root and the members of `nfc` (in `/etc/group`; `nobody`,
the launcher's user, is one), then drops the group; anyone else sees *Not in group nfc* on
Read. Cards set up before this version get the group at the next base install.

**Without the driver.** On the Flipper today Read says *No NFC device*: the ST25R3916 driver is
being written (Phase 13). `nfc -L` runs the app with two software cards in the field instead
(as `infrared -L`), so you can try the screens in QEMU or on the Flipper: the first Read finds a
MIFARE Classic 1K (UID A2450711, key A `FFFFFFFFFFFF` on every sector, key B in no
dictionary), the next an NTAG215, then the Classic again. To try it, put
`NFC test:nobody:@nfc:=/bin/nfc -L` in `/etc/apps` (a window app runs only from the launcher).

```
nfc -t /tmp/my.dic           # no screen: read both test cards (keys from my.dic), save,
                             # read back, Add manually; "nfc: N ok, M bad"
nfc -c                       # may I open /dev/nfc?  0 yes, 1 not in group nfc, 2 no device
```

**Memory.** The whole app, data and stack included, is about 18 KB (the AC14.3 limit is
35 KB; m2test checks it): the card being read (5.3 KB) and the keys found exist only during a
read or while a file is shown. It runs next to the launcher, which draws it; nothing stays
when you leave it. Emulation (`nfc emulate`) and the command-line `nfc read`/`nfc dict` wait for
the driver.

See also: [Sub-GHz remotes](subghz.md) for the same "your own key file, no shipped secrets"
rule on the radio side, and the development guide's
[`/dev/nfc` driver interface](../devguide/nfc-driver-interface.md).

### Copying a card with a password (coming)

When the copier writes an NTAG or Ultralight copy, it can give the copy a password too. In Settings you choose: **random** (a new one for each copy, the default), **own** (one you typed), **previous** (the one of the last copy), **dictionary** (a chosen line of `/home/flipper/nfc/assets/t2t_pwd_user.nfc`) or **legacy** (no password, as stock does). Whatever is chosen is written into that dictionary file first, with your note (for example *Tokia*), and only then onto the card, so a copy that was cut short can still be opened with the Flipper. Keep an eye on the file: it holds your secrets, and it never leaves the SD card.
