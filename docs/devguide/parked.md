# Parked: ideas and work set aside for later

Things the user and the local agent talked through and decided not to do now. Each has an
issue with the details; this page is the index, so that nothing is lost between sessions.
When one is taken up, remove it here and link the PR in its issue.

Last updated: 2026-11-27 (evening).

## Infrared

- **Recognise a remote and save the whole of it** ([#484](https://github.com/arielkonopka/Galtzagorriak-MINIX/issues/484)).
  Press a few buttons, the Flipper finds the model in the remote database and saves all its
  buttons, not only the ones pressed. Today's library match takes every `tv.ir` entry with the
  same protocol and address, which mixes several TVs and gave a Power button that did not work.
  - First version: a text index built on the PC from Flipper-IRDB.
  - **Revisit when `kutxa` exists** (spec section R names an IR code library as a use): a tree
    of codes → models and a tree of model → buttons.
  - Candidates tie often (35 Samsung models for one remote): show a few, or ask for the button
    that tells them apart.
- **Usability of a saved remote** (same issue). A whole remote is a flat list of 49 rows; a
  sequence such as Source → Down → Enter is too slow to reach. Ideas: a pad view (the Flipper's
  arrows and OK send the remote's), the common buttons first, held keys repeating.

## Bluetooth LE

- **The full BLE stack and `blescan`** ([#390](https://github.com/arielkonopka/Galtzagorriak-MINIX/issues/390)), in progress,
  not parked: listed here for what waits behind it.
  - The user's goals: find a tag by signal strength and make it beep (the `iTAG`: Immediate
    Alert 0x1802, write `02` to 0x2A06), and the Flipper as a tag itself.
  - **Later:** the tag's own settings (0xFFE2..0xFFE7, e.g. the beep on a lost link), not
    worked out yet.
  - **Later:** `ble-full-ext` if something needs L2CAP channels, extended advertising or
    Enhanced ATT. `ble-full` does not have them (ST's notes), and the extended stack costs
    36 KB more flash and more of SRAM2a.
- **A way to lift the cache's daily erase budget in dev mode.** A day of many DFU flashes
  spends the 32 pages; the day turns with the RTC's date, which is UTC.

## Clock and calendar

- **Today's holiday on the locked desk clock** ([#493](https://github.com/arielkonopka/Galtzagorriak-MINIX/issues/493)).
  In place of the unlock hint on the bottom line, from the calendar app. There is no calendar
  yet (holidays are planned as a `kutxa` file per country, spec section R); an interim text
  file is sketched in the issue. Open: which holidays, the language (the small font is ASCII
  only), and whether the unlock hint stays.

## The Flipper as a login key

- **FIDO/U2F** ([#485](https://github.com/arielkonopka/Galtzagorriak-MINIX/issues/485)). USB first, then NFC; BLE last and
  only if the user's devices support it. It will be a working authenticator, not a certified
  one: no secure element, no vendor attestation. Fine as a spare key, not as the only key to
  a bank.

## Radio links between computers

- **Two computers, two Flippers, data over Sub-GHz** ([#486](https://github.com/arielkonopka/Galtzagorriak-MINIX/issues/486)).
  `gmodem` already has the Sub-GHz medium (106 to 1741 B/s peak at 433.92 MHz, 10 % duty in
  PL). Missing: the PC side, encryption, a range table through walls, relaying.
  - **The user wants it on changing frequencies**, to cope with interference: that is spec
    R19.11 (hopping in TDD slots, the sequence from the pairing key, adaptive hopping with a
    bad-channel map). The single channel is the first step and the range test. In the EU
    hopping does not raise the duty budget, and it needs MINIX on both Flippers.
  - **Decided by the user (2026-11-27):** the second bench Flipper stays on stock firmware
    as the reference for as long as the tests need it; it gets MINIX once stock is no longer
    needed for testing. Until then a two-way test needs a stock-side script that speaks
    `gmodem`'s frames, or waits.
- **A Bluetooth mesh** was the first form of this idea. Not pursued: 2.4 GHz does not go
  through the walls between flats, ST's stacks have no Coded PHY, and SIG Mesh is not part of
  the CPU2 stack.

## Size

- **A size review of the whole system** ([#494](https://github.com/arielkonopka/Galtzagorriak-MINIX/issues/494),
  the user, 2026-11-27; for the cloud lane). The kernel's, MM's and the launcher's static RAM
  are exactly at their budgets, and the full BLE stack took 36 KB from the installed programs.
  Wanted: a list of what can be cut or shared, with the bytes each change gives, and then
  refactoring with no feature lost.
