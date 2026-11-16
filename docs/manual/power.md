# Power and battery

This chapter grows with the power work (spec R6.1, R7.1, tasks T14–T15). Today it covers reading
the battery current.

## Checking how much current the Flipper draws

The **Power** app in the launcher shows what the battery fuel gauge measures. You do not need to
open the Flipper or use a USB meter (a USB meter cannot show it: on USB the Flipper stays awake and
charges, and those meters only resolve about 10 mA).

- **Now** is the current flowing out of the battery at this moment, in mA. A minus sign means the
  battery is discharging. This number includes the screen's backlight and the app itself.
- **Idle** is the gauge's average current read while the backlight had been off for at least
  30 seconds. This is the number to compare between builds and settings.

To measure the idle current:

1. Unplug the USB cable. With USB connected the app says "USB: unplug to measure".
2. Open **Power** and leave the Flipper alone for about a minute. The backlight goes off after
   15 seconds; the app keeps reading in the background.
3. Press any button. The backlight comes on and the **Idle** line shows the value and how long
   ago it was read. Until there is one, the line says "Idle: leave it 1 min".

Below the large **Now** value the app shows the gauge's average current, the charge in %, the
battery voltage and temperature, and "on battery", "on USB" or "charging". Back returns to the launcher.
On a board without a gauge (the QEMU emulator) the app says "no gauge".

The gauge counts in whole milliamps, so below about 2 mA the value is coarse. For finer numbers
use the battery percentage over a day, or a meter on the battery leads.

From the shell, `cat /dev/power` prints the same values on one line:

```
soc=87 mv=3912 ma=-6 avg=-5 sby=-5 temp=243 flags=0x0181 usb=0 chg=0 vbus=0
```

`soc` is the charge in %, `mv` the battery voltage, `ma` the current now, `avg` the gauge's
average, `sby` the gauge's estimate of the standby current (all in mA, negative when
discharging), `temp` the battery temperature in tenths of a degree Celsius, `flags` the
gauge's raw status word, `usb` 1 while a computer is connected over USB, `chg` the charger's state (0 not charging,
1 pre-charge, 2 fast charge, 3 done) and `vbus` what the charger sees on the USB connector (0
nothing, 1 a computer, other values a power supply). The app shows "charging" or "charged".

## Screen power save

On battery, 2 minutes after the backlight has gone off, the screen itself goes blank to save a
little more current. Any key brings back the same picture at once (that first press only wakes the
screen and the light, as when the light is off). With USB connected the screen never blanks.

## Sleep when idle

When nothing runs and no USB cable is plugged in, the Flipper goes into deep sleep (the chip's
Stop2 mode) between clock events. A button press wakes it at once; the backlight's dimming steps,
alarms and timers wake it when they are due. A press still feels immediate (about 2 ms from the
press to the screen), and the clock keeps the 32 kHz crystal's time.

With USB connected the Flipper never sleeps, so the USB console, the SD card and everything else
behave exactly as on the bench.

The serial console on the GPIO header (UART) cannot wake the chip: characters typed while the
Flipper sleeps are lost. If you use it, keep USB connected as well.

On a latency-trace build, `lat` prints how long the Flipper spent in Stop2 since boot, how many
wake-ups came from a button, and why idle time did not sleep (USB, an alarm too close, and so on).

## Charging settings

As root you can change how the battery is charged by writing a command to `/dev/power`:

```
echo profile care > /dev/power    # charge to about 80 % (4.0 V): for a Flipper kept on a charger
echo profile full > /dev/power    # charge to full (4.208 V, as the stock firmware)
echo gentle on > /dev/power       # charge at 1 A instead of 2 A: slower, cooler
echo gentle off > /dev/power
echo otg on > /dev/power          # 5 V on the GPIO header (pin 1), for a module
echo otg off > /dev/power
```

The profile and gentle charging are kept until you change them (also over a reboot or power off,
as long as the battery is in). Nothing ever charges faster than the stock firmware does.
`cat /dev/power` shows the result at the end of the line: `vreg=` (charge-to voltage in mV),
`ichg=` (charge current limit in mA) and `otg=`.

## Power off

`halt` (as root) switches the Flipper off for real: the battery is disconnected from the system
(the charger's ship mode, as the stock firmware's power off). The screen and the backlight go dark
first. With USB connected the Flipper cannot cut its own power: it waits, dark, until you unplug
the cable. To switch it on again, hold **Back** for about a second, or plug in USB.
