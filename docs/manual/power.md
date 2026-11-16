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
soc=87 mv=3912 ma=-6 avg=-5 sby=-5 temp=243 flags=0x0181 usb=0
```

`soc` is the charge in %, `mv` the battery voltage, `ma` the current now, `avg` the gauge's
average, `sby` the gauge's estimate of the standby current (all in mA, negative when
discharging), `temp` the battery temperature in tenths of a degree Celsius, `flags` the
gauge's raw status word, and `usb` 1 while a computer is connected over USB.

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
