# Encrypting files to your OpenPGP key: `pgpenc`

`pgpenc` locks a file with your **public** OpenPGP key, so that only the owner of the secret key
can open it, for example on your PC with `gpg -d` and the PIN of your YubiKey. Nothing secret
is on the Flipper: if the Flipper is lost, the encrypted files stay closed. Use it for the
things you carry on the SD card and would hate to leak: notes, key dumps, `nfc` card files.

```
pgpenc [-a] -k KEYFILE [-o OUT] FILE
pgpenc -k KEYFILE -i
```

| Option | What it does |
|---|---|
| `-k KEYFILE` | your public key, as `gpg --export` writes it (binary) or `gpg --armor --export` (text) |
| `-o OUT` | the output file (default: `FILE.gpg`, or `FILE.asc` with `-a`) |
| `-a` | ASCII armor (text you can paste into mail) instead of binary |
| `-i` | only show the key id and fingerprint of the key it would use |

## One-time setup

On the PC, export just the key you need (the encryption subkey is used) and put it on the card:

```
$ gpg --export-options export-minimal --export you@example.org > /media/sd/home/flipper/my.pub
```

`pgpenc` takes the **last RSA subkey** in the file (the encryption subkey of a normal key), else
the primary key. RSA keys of 1024 to 4096 bits work; keys of Curve25519 or NIST curves (a YubiKey
can hold them) are **not supported yet**. The key file may be at most 3 KB: use
`export-minimal` for 4096-bit keys.

## Use

```
$ pgpenc -k /home/flipper/my.pub -i
key id 5C3D0F6A91E4B2A7
fingerprint 0123...
RSA 2048 bits, e 65537
$ pgpenc -k /home/flipper/my.pub /home/flipper/notes/etxea.txt
$ ls /home/flipper/notes
etxea.txt  etxea.txt.gpg
```

On the PC: `gpg -d etxea.txt.gpg > etxea.txt` asks for the PIN of the card and writes the file back.
Check the key id with `gpg --list-keys --keyid-format long`: the id `-i` shows is the encryption
subkey's.

The file can be any size: it is read and written in pieces of 512 bytes. While it runs
`pgpenc` needs about 12 KB of RAM (a large program: close other apps first if it says it cannot
start) and writes only the output file to the SD card. The session key and the padding come from
the random number generator of the chip (`/dev/random`). The output is a standard OpenPGP message
(RSA, AES-256, integrity protected), checked against `gpg` in the test suite, with files of 0 to
70 000 bytes and 2048 and 3072 bit keys.

## What comes next

- Reading the public key straight from the YubiKey over NFC, once, without the PIN, and saving it
  as `KEYFILE` (the NFC part is on the device lane; it only reads, it never writes to the card).
- Using the chip's AES hardware.
- A second mode with a key that lives in the YubiKey's FIDO2 `hmac-secret`.
- Curve25519 keys.
