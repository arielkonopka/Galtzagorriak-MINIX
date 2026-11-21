# iButton keys: the iButton app

**iButton** on the launcher is the stock iButton app as a window app (issue #413). It runs the
`ikey` command (spec R6.7) and shows its lines as they come.

| Row | What it does |
|---|---|
| Read | `ikey read`: hold a key to the contacts (10 s). When a key was read, **Use this key** opens its page. |
| Saved | the `.ibtn` files of `/home/flipper/ibutton`, a page at a time; OK opens the key's page. |
| Add manually | the protocol (Dallas, Cyfral, Metakom) and the key in hex (16, 4 or 8 digits), then **Next**. |

A key's page shows its protocol and number, then:

- **Emulate**: `ikey emulate`; the Flipper is the key until you press Back. Each time a reader
  got it, a line says so.
- **Write** (Dallas only): `ikey write` to an RW1990 blank on the contacts, read back to compare.
- **Name** and **Save** (a key you read or added), or **Delete** (a saved one).

Files are stock's: version 2 (`Protocol: DS1990` and `Rom Data:`, or `Protocol: Cyfral` /
`Metakom` and `Data:`) is written; version 1 (`Key type:`) is read too. Archive's **Open** on an
`.ibtn` file starts this app on that file.

Under QEMU there are no contacts; the M2 self-test checks Add manually, Save and Saved. Reading,
writing and emulating need the device.
