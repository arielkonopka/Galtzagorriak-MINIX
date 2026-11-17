# Test data for `twoflip.py`

- `subghz/*.sub`: the Sub-GHz key files of the Unleashed firmware's unit tests
  (`applications/debug/unit_tests/resources/unit_tests/subghz/`, unleashed-firmware commit
  `53818b1`, GPLv3), the `*_raw.sub` recordings left out. Each becomes a `subghz-decode` row.
- `nfc/*.nfc`: tag files from the same unit tests (`.../unit_tests/nfc/`), plus
  `Iso14443_3a.nfc`, a plain ISO14443-3A tag written for these tests. Each becomes an `nfc` row.

A file added here becomes a row by itself. See `docs/devguide/part2/06-two-flipper-tests.md`.
