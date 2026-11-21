# Small apps: Music Player, Clock, Dice, Passport, U2F

These are ports of stock firmware apps (issue #420). Each one is a window app: the launcher draws
it in the same style as every other list, with the title bar at the top and the button hints at
the bottom. Long Back always ends the app.

They come in two packages, so you only install what you want:

    pkg install /lib/pkg/music_1.0.tar     # Music Player
    pkg install /lib/pkg/utils_1.0.tar     # Clock, Dice, Passport

Installing adds the launcher lines (`/etc/apps`), and `pkg remove` takes them away again. The apps
run as `nobody`, in group `gui`.

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

## U2F (planned)

U2F makes the Flipper a two-factor security key for websites, as on stock. The protocol part is in
place and tested on the PC (`src/lib/u2f`, `make -C flipper/tests/u2f test`). It handles
registering a site, logging in, the version query and the USB HID framing, and its answers
are byte for byte in stock's format, with stock's key derivation. The P-256 signatures come from
micro-ecc (`src/lib/u2f/uecc`, BSD licence, which older stock firmware used too), and the tests
check them against the RFC 6979 key. Still **(planned)**: the app and the USB HID FIDO interface
(the USB side waits for the USB HID work, as Bad USB did).

These rules will apply to the key files when the app comes:

- **You bring the attestation certificate and its key.** They go in `/ext/u2f/assets/cert.der` and
  `cert_key.u2f`. The package ships none, never someone else's.
- **The device key** (`/ext/u2f/key.u2f`, 32 random bytes from `/dev/random`) is made at the first
  start. **The counter** (`cnt.u2f`) goes up with every login. Both belong to the user `u2f`, mode
  0600, and live on the SD card, never in internal flash. A login is one small write to the card.
- The counter is written to a new file that then replaces the old one. If it cannot be saved, the
  app answers with an error and signs nothing, so no counter value is ever used twice. If the
  counter file is unreadable, the app stops instead of starting again from 0.
- Stock encrypts `key.u2f` with a key held in the radio core (CPU2). MINIX does not use CPU2 for
  this, so stock's `key.u2f` cannot be read here. Your sites must be registered again. Anyone who
  can read the SD card can copy the key, so keep the card safe.
