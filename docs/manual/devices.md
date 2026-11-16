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

## Voltages on the header pins (`/dev/adc`)

Measures the voltage on the GPIO header pins that have an analog input: PA7, PA6, PA4, PC3,
PC1 and PC0 (PB3 and PB2 have none). Anyone may read it.

```
cat /dev/adc
pa7=1650 pa6=- pa4=- pc3=- pc1=- pc0=> ref=2500
```

Values are in millivolts, from 0 to 2.5 V: the converter measures against the chip's internal
2.5 V reference (`ref`, as the stock firmware does), so a pin above 2.5 V shows `>`. A pin must
never get more than 3.3 V. Only a pin left in its default "off" state (`gpio N off`) is
measured; a pin set up as an input or output shows `-`, so reading never disturbs it. Each read
takes one fresh measurement (16 samples averaged, corrected with the chip's factory
calibration), usually in a few milliseconds while the reference settles, and the converter and
the reference are switched off again afterwards. Continuous sampling and an app with a
voltmeter screen are **(planned)** (spec R6.4, later parts).

## Sub-GHz radio, listening only (`/dev/subghz`)

The Sub-GHz chip (CC1101) can listen on one frequency and report the signal strength there. It
never transmits through this device.

```
echo 433920000 > /dev/subghz    # listen at 433.92 MHz (in Hz)
cat /dev/subghz                  # rssi=-95 freq=433919830
echo 0 > /dev/subghz             # stop
```

Frequencies are those the stock firmware accepts: 300–348, 387–464 and 779–928 MHz; anything else
is refused. The frequency read back is what the chip really tuned (in steps of about 400 Hz). The
signal strength is in dBm: around −100 means quiet, a remote pressed nearby shows −40 or more. The
radio keeps listening until `echo 0`, or until nobody has read it for about a minute (it draws
about 15 mA while listening). A spectrum view and
recording will build on this.

