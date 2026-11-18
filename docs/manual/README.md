# Galtzagorriak MINIX manual

The user and system manual of Galtzagorriak MINIX for the Flipper Zero. It describes the system
as it works on the main branch; planned features are marked **(planned)** with the spec section
that brings them.

1. [Installation](install.md): building, the SD card, flashing, first boot, updating, recovery
   and going back to the stock firmware, known limitations.
2. [Power and battery](power.md): reading the battery current and the idle draw.
3. [Hardware devices](devices.md): the `/dev` files for the Flipper's hardware (iButton, header pin voltages).
4. [Buttons and gestures](buttons.md): holding two buttons to force the backlight off or on (planned).
5. [The SD card](sdcard.md): power loss, the boot-time check and pulling the card out (planned).
6. [Swap](swap.md): idle programs on the SD card (`swap=plain`), what is never swapped, `free -v`.
7. [Timers and alerts](timers.md): `/var/timers`, `timed`, `alert`, do-not-disturb (`/etc/notify.conf`).
8. [The self test](selftest.md): the hardware self test (`stest` package, `selftest -a`).
9. [Reminders and the alarm clock](remind-alarm.md): the `remind` and `alarm` apps, the desk clock.
10. [The launcher](launcher.md): the app list, icons, the settings page, `/etc/apps`, window apps, the desktop (HOME, status line, modules, `guimod`).
11. [Data modems](modem.md): files between two Flippers over RFID (Sub-GHz, NFC and BLE planned), `gmodem`.
12. [USB gadgets](usb-gadgets.md): the Flipper as a USB keyboard, mouse and media keys (`/dev/hidkbd`, `/dev/hidmouse`, `/dev/hidcons`, layouts); Bad USB and Teklatua planned.
13. [Files to a PC over USB](usb-files.md): MTP, Windows and Linux copy files with their own drivers (`mtpd`; the USB side planned).

More chapters (the apps, the shell and commands, packages, settings, the
hardware devices, time and location) follow as the features settle. Release notes are in
[`CHANGELOG.md`](../../CHANGELOG.md); the design is in [`docs/spec/`](../spec/README.md).
Writing software for the system, or working on it: the
[Galtzagorriak MINIX Development Guide](../devguide/README.md).

Keeping it current: see the standing rule "Docs follow behaviour" in
[`docs/spec/next-tasks.md`](../spec/next-tasks.md).
