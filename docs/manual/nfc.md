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
> shown on screen only, never written to a file by the reader). MIFARE Ultralight C and
> Ultralight AES cards need a cipher library this port does not have yet: their protected pages
> show as not read, the open ones are read. More parsers: all_in_one, ndef (Ultralight and MIFARE
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
seconds per key on the host; mfkey32 also recovers the all-`F` transport key. **(planned: the
on-device nonce collection; the static-encrypted attack, which needs a per-card candidate-key
dictionary.)**

```
mfkey nonneak.txt            # nonneak = nonces; prints the recovered keys
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
