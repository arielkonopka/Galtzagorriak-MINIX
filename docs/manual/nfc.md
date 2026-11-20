# NFC

Reading, saving and emulating 13.56 MHz contactless cards (NFC): the `nfc` app, the `mfkey`
key-recovery command and the MIFARE Classic dictionary. Behaviour follows the stock Flipper
firmware (spec Phase 14).

> **Status.** The protocol layer is in the code and tested on a PC (`src/lib/libnfc`): card
> activation (ISO 14443-3A: request, anticollision, select), telling a card's type from its
> SAK, MIFARE Classic authentication, the dictionary attack and block read, and the MIFARE
> Ultralight/NTAG page reader all run against software card models. The parts that need the
> radio — reading a *real* card, emulation, the field detector — wait for the ST25R3916 driver
> (spec Phase 13), so the `nfc` **app** and the live commands below are **(planned)** until
> that driver lands. Saved-card files already work on dumps. The full `mfkey` key recovery
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
keys out. **(planned: the on-device nonce collection; the Crypto-1 cipher is in the code.)**

```
mfkey nonneak.txt            # nonneak = nonces; prints the recovered keys
```

## The `nfc` app (planned)

The screen app mirrors stock: **Read**, **Detect reader**, **Saved**, **Extra actions**
(MIFARE Classic keys, unlock with a reader) and **Add manually**. Peak RAM stays within the
Phase 14 budget (≤ 35 KB; the launcher is swapped out meanwhile and comes back). Examples:

```
nfc read                     # read a card, offer to save it
nfc read garraio-txartela    # read, save as garraio-txartela.nfc
nfc emulate sarbide-bbox     # pretend to be the saved card
nfc dict garraio-txartela    # run the dictionary attack on a card
```

See also: [Sub-GHz remotes](subghz.md) for the same "your own key file, no shipped secrets"
rule on the radio side, and the development guide's
[`/dev/nfc` driver interface](../devguide/nfc-driver-interface.md).
