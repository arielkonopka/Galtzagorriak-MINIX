# Small apps: Music Player, Clock, Dice, Passport, U2F

These are ports of stock firmware apps (issue #420). Each one is a window app: the launcher draws
it in the same style as every other list, with the title bar at the top and the button hints at
the bottom. Long Back always ends the app.

They come in three packages, so you only install what you want:

    pkg install /lib/pkg/music_1.0.tar     # Music Player
    pkg install /lib/pkg/utils_1.0.tar     # Clock, Dice, Passport
    pkg install /lib/pkg/u2f_1.0.tar       # U2F (below)

Installing adds the launcher lines (`/etc/apps`), and `pkg remove` takes them away again. The apps
run as `nobody` in group `gui`, except U2F, which runs as `u2f`.

## Music Player

It plays songs on the speaker, from `/ext/music_player` (on MINIX `/home/flipper/music_player`,
the stock card's place). It reads two formats, as the stock Music Player does:

- **Flipper Music Format** (`.fmf`):

      Filetype: Flipper Music Format
      Version: 0
      BPM: 120
      Duration: 4
      Octave: 5
      Notes: 8P, 8G5, 4A#5., C6

- **RTTTL** (`.txt` or `.rtttl`), the old ringtone format: `name:d=4,o=5,b=120:8p,8g,4a#.,c6`.

Each note is written `[duration] letter [#] [dots] [octave] [dots]`. The duration is 1 to 128,
where 4 is one beat. The letter is `A`–`G`, or `P` for a rest. The octave is 0 to 8. A part you
leave out takes the header's value (`Duration`, `Octave`, or RTTTL's `d=`, `o=`). One dot makes a
note 1.5 times as long, and each further dot adds half of what the previous one added.

- **The list** shows the songs, paged when there are many. OK plays one.
- **The playing screen** shows the song's name and its state (`Playing, 120 BPM`, `Done`,
  `Stopped`, or `Error: bad note, line 6`). **Volume** (0–100 %) changes at once with
  Left/Right. OK on **Stop** stops the song, and OK on **Play again** starts it over. Back goes to
  the list and stops the sound.

The song is never loaded whole: the app reads the file as it plays, so a long song costs no more
memory than a short one. Notes go to the speaker in pieces of at most 100 ms, so Stop and Volume
take effect within 100 ms. The volume is kept only while the app runs, and nothing is written.
The package brings one song, `scale.fmf`, to try it out. Copy your own songs there over USB
([Files to a PC](usb-files.md)), with `pkg get`, or with an SD card reader.

## Clock

The time in big digits, with the weekday and the date below it. OK switches the date between
`Sat 21 Nov 2026` and `2026-11-21`. The screen changes once a minute. Unlike stock's clock it
shows no seconds, so the device stays asleep between minutes. The app itself wakes only once an
hour and at midnight, to change the date.

## Dice

Pick the **Die** (Coin, d4, d6, d8, d10, d12, d20, d100) and the **Count** (1–9) with Left/Right,
then press OK on **Roll**. The result line shows each die, or `H`/`T` for a coin, and the sum. The
numbers come from `/dev/random`, the Flipper's hardware random number generator, and every face is
equally likely.

## Passport

Who this Flipper is: the name (the host name, `/etc/hostname.file`), the system and its release,
how long it has been on, and the battery charge. Stock's passport shows its dolphin's mood and
level; there is no dolphin on MINIX. OK on **Refresh** reads the values again.

## U2F

U2F makes the Flipper a two-factor security key for websites, as on stock.

    pkg install /lib/pkg/u2f_1.0.tar

The app runs as its own user `u2f` (group `usb`). **The USB part is still (planned):** the Flipper
does not yet show up to the computer as a U2F key. Until it does, the app checks your files and
says `No USB U2F interface yet`. Everything else is in place and tested on the PC
(`make -C flipper/tests/u2f test`):

- the U2F messages (registering a site, logging in, the version query) and the USB HID framing,
  byte for byte in stock's format and with stock's key derivation;
- the P-256 signatures, from micro-ecc (`src/lib/u2f/uecc`, BSD licence, which older stock firmware
  used too), checked against the RFC 6979 key;
- the key files below.

When a site asks, the app shows **Register** or **Log in**. OK on **Confirm** allows it (stock's
"press OK"), and Back refuses. Nothing is signed without that OK, unless the site only checks
whether a key is yours, or asks for a login without presence.

### The key files

All in `/ext/u2f` (on MINIX `/home/flipper/u2f`, owned by `u2f`, mode 700):

| File | What | Who makes it |
|---|---|---|
| `assets/cert.der` | the attestation certificate, DER, at most 768 bytes | **you** |
| `assets/cert_key.u2f` | its private key, stock's format with `Type: 2` (not encrypted) | **you** |
| `minix.key` | the device key: 32 random bytes from `/dev/random` | the app, at its first start |
| `minix.cnt` | the login counter | the app, at every login |

- **No certificate or key is shipped.** Make your own pair, or use the one you already have. As
  root, copy both files into `assets`, then `chown u2f` and `chmod 600` them. `cert_key.u2f` looks
  like this (32 bytes in hex):

      Filetype: Flipper U2F Certificate Key File
      Version: 1
      Type: 2
      Data: 1A 2B ... (32 bytes)

  A `cert_key.u2f` of `Type: 0` or `1` is encrypted with a key that only the stock Flipper's radio
  core (CPU2) holds. MINIX does not use CPU2 for this, so the app refuses such a file and says so.
- Stock's own `key.u2f` and `cnt.u2f` are encrypted the same way, so MINIX leaves them alone and
  keeps its own `minix.key` and `minix.cnt` next to them. A card moved back to stock still has the
  stock keys. The other side of this: sites registered on stock must be registered again on
  MINIX, and the other way round.
- `minix.key` and `minix.cnt` are mode 0600 and live on the SD card, never in internal flash. Each
  file is written to a new `.tmp` file that then replaces the old one, so a power cut leaves the
  old file or the new one. A login is one small write to the card.
- The counter is saved before the signature leaves. If it cannot be saved, the site gets an
  error and nothing is signed, so no counter value is ever used twice. If `minix.cnt` (or
  `minix.key`) exists but cannot be read, the app stops and says so instead of starting again
  from 0. Fix or remove the file by hand.
- Anyone who can read the SD card can copy `minix.key`, so keep the card safe. On stock the key is
  encrypted by CPU2.
