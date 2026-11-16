# Hardware devices

The Flipper's hardware appears as files under `/dev`, so the shell and any program can use it.
This chapter grows as the drivers arrive; the apps built on them get their own chapters.

## iButton (`/dev/ibutton`)

Reads the ROM of a Dallas 1-Wire key (DS1990A and compatible) held on the iButton contacts.
Members of the `gui` group (the launcher's apps) and root may read it.

```
od -x /dev/ibutton
```

With a key on the contacts this prints its 8-byte ROM: the family code, the 6-byte serial
number and a CRC byte, the same number the stock iButton app shows under "Read". Each read
gives the ROM once, then end of file.

- With no key on the contacts the read fails at once with "No such device" (ENODEV).
- A bad touch (wrong CRC, or a line held low) fails with "I/O error" (EIO); touch again.

A read takes about 10 ms. Writing keys, emulating a key and the Cyfral/Metakom keys are
**(planned)** (spec R6.7, later parts), as is an iButton app in the launcher.
