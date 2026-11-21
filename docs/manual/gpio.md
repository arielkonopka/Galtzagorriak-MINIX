# The GPIO header: the GPIO app

**GPIO** on the launcher shows the eight header pins (PA7, PA6, PA4, PB3, PB2, PC3, PC1, PC0,
with their pin numbers) as the `gpio` command does, through `/dev/gpio`.

- Each row is a pin with its level (`=0`, `=1`). Left and Right choose its mode: Out 0, Out 1,
  In, In up, In down, Off. The change is made at once and the levels are read again.
  The kernel keeps no mode to read back, so a pin shows `-` until you set it here; it stays as
  set after the app ends.
- **Read the levels** reads them again.
- **5V on GPIO**: **(planned)**. The kernel has no switch for pin 1 yet. Stock's warning applies
  when it comes: 5 V on pin 1 may damage what is connected.
- **USB-UART Bridge**: **(planned)**; for now use the console on USB.
