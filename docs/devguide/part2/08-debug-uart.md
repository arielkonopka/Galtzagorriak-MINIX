# 8. Dev mode and the debug serial line

When the USB console is the thing that breaks (a hang, a lost transfer, a panic before USB is
up, a reset in the night), you need a second way to see what the kernel says. The Flipper has
one: the **debug UART** on the GPIO header. This chapter shows how to switch a board into dev
mode, how to read the UART with **a second Flipper running Unleashed's USB-UART Bridge** (the way
this project does it) or with a plain USB-to-serial adapter, and how to work with both lines at
once.

> Some details of the local debugging build are still being collected from the developer who
> uses it (issue #110, 2026-11-19). Sections marked **(to confirm)** will be updated.

## 8.1 Dev mode

Dev mode marks a board that is being developed on. Switch it on as root:

```
reboot -d
```

This restarts into ST's DFU loader (ready for `dfu-util`, chapter 2.9) and sets the **dev bit**
in RTC backup register 19, which survives resets and, with the battery in, power-off. A normal
`reboot` or `halt` clears it. `ver` and the boot banner show it:

```
reset: software; dev mode: a watchdog reset goes to DFU
```

While the bit is set:

| What changes | Why |
|---|---|
| A **watchdog reset goes to the DFU loader** instead of rebooting | a broken new image comes back to the loader by itself, ready for the next flash (PORTING.md, "Flashing and recovery") |
| The boot banner adds two lines: `power: smps on; cc1101 asleep (… us to wake); st25r3916 op 0 id …` and `charger: adc on demand` | what `hal_init` parked at boot (T14a, T14c) |
| The debug clocks stay on in Sleep, Stop and Standby (`DBGMCU_CR`) | a debugger on SWD keeps its connection while the CPU sleeps |

Nothing else changes: the same kernel, the same RAM, no extra output. Leave dev mode (`reboot`)
before tests that must behave like a user's board, such as the overnight battery test
(chapter 7): there a watchdog reset must reboot and be counted, not end in the loader.

If the board does not come up at all, the buttons still reach DFU: hold **Left + Back**, release
Back, keep Left; for an image broken before that check, **Left + Back + OK**, then keep OK.

## 8.2 What the debug UART carries

USART1 on header **pin 13 (TX, PB6)** and **pin 14 (RX, PB7)**, **115200 8N1, 3.3 V**, no flow
control. It is not a separate debug channel but the **same console** as the USB serial port
(`src/kernel/arm/cons.c`):

- every byte the console prints goes to the UART, to USB and to the LCD console if it is shown;
- characters typed on the UART arrive at the same console as those typed over USB, so you can log
  in and work on either;
- kernel `printf` (boot messages, warnings, panics) goes the same way.

What makes it useful when USB is the suspect:

- **It works before USB.** The banner, the reset cause and the dev-mode lines appear on the UART
  from the first moment; USB comes up later in the boot.
- **It does not depend on the USB stack.** Output is written straight to the USART register
  (polled), so it still gets out when the USB endpoint is stuck, the host has stopped reading,
  or the kernel has panicked.
- **It keeps working across a USB replug**, so you can watch what happens when the cable is
  pulled and put back.

Limits:

- **It cannot wake the CPU from Stop2** (USART1 has no wake-up from Stop2). Characters typed
  while the Flipper sleeps are lost. Keep USB attached while you use the UART: power on VBUS keeps
  the CPU out of Stop2.
- **Output is polled**: each byte waits for the USART, about 87 µs at 115200. Heavy console
  output therefore costs CPU time, the same for every board; a debug build that prints a lot
  changes timing (`lat` L4/L5).
- Pins 13 and 14 are reserved for it: `gpio` does not offer them.

## 8.3 Reading it with a second Flipper (USB-UART Bridge)

The project's way: the reference Flipper with Unleashed firmware (the same one used for the
two-Flipper tests in chapter 6) acts as a USB-to-serial adapter. Its GPIO pins are 3.3 V like the
MINIX Flipper's, so nothing else is needed.

### Wiring

Three jumper wires (female-female Dupont) between the two GPIO headers. TX of one goes to RX of
the other:

| MINIX Flipper | | Unleashed Flipper (bridge on pins 13/14) |
|---|---|---|
| pin 13 (TX) | → | pin 14 (RX) |
| pin 14 (RX) | ← | pin 13 (TX) |
| pin 8, 11 or 18 (GND) | — | pin 8, 11 or 18 (GND) |

Do **not** connect the 3.3 V or 5 V pins: each Flipper runs from its own battery or USB.
If the bridge is set to pins 15/16 instead, use Unleashed's pin 15 (RX) and pin 16 (TX).

### The bridge **(to confirm)**

On the Unleashed Flipper: **GPIO → USB-UART Bridge**, then in its **Config**:

| Setting | Value |
|---|---|
| USB Channel | 1 (keeps the CLI on channel 0, so `twoflip.py` can still use it); 0 replaces the CLI |
| Baudrate | 115200 |
| UART Pins | 13,14 |
| RTS/DTR Pin | None |

Leave the app open; the screen shows the bytes going each way. The bridge appears on the PC as
another USB serial port.

### On the PC

Connect both Flippers by USB. On Linux, run `ls /dev/ttyACM*` before and after starting the
bridge: the port that appears is the bridge (`ls -l /dev/serial/by-id/` shows which device each
port belongs to). Then open it with a log file, for example:

```
picocom -b 115200 --logfile uart-$(date +%F-%H%M).log /dev/ttyACM2
```

or `screen -L /dev/ttyACM2 115200`. Close it with `Ctrl-A Ctrl-X` (picocom) or `Ctrl-A k y`
(screen). On macOS the port is `/dev/cu.usbmodem…`; on Windows a `COM` port in Device Manager,
opened with PuTTY at 115200.

A quick test: press Enter in the bridge's terminal. The MINIX console answers with its prompt or
`login:` on the UART, and the same text appears in the USB terminal.

## 8.4 With a plain USB-to-serial adapter

Any **3.3 V** adapter works the same way (CP2102, CH340, FT232, PL2303 boards): its TX to MINIX
pin 14, its RX to MINIX pin 13, GND to pin 8, 11 or 18; 115200 8N1. Watch for:

- **3.3 V levels only.** A 5 V adapter can damage the STM32's pins; many boards have a jumper for
  3.3 V/5 V; set it before connecting.
- **Do not connect the adapter's VCC pin.**
- On Linux the adapter is `/dev/ttyUSB0` (CP2102, CH340, FT232) or `/dev/ttyACM*`; use
  `/dev/serial/by-id/` to be sure which is which. ModemManager may probe a new port for a few
  seconds and garble it: `sudo systemctl disable --now ModemManager`.

## 8.5 The debug build **(to confirm)**

For the USB hang fixed in #298 the developer used a local debug build that adds USB counters and
prints a **state dump when Ctrl-] is typed on the UART** (for example
`[usb] configured 1 dtr 1 tx_busy 1 … tx_fill 247` and the raw endpoint register `EP1R`). It is
not in main. How it is built, what the dump covers and whether it becomes an opt-in make target
are being asked in issue #110; this section will describe it once answered.

The general pattern, for your own investigations:

1. Add the counters or the dump to the driver under an `#ifdef` (or a local patch), never in the
   release build: RAM and timing are budgeted (chapter 5).
2. Trigger the dump from the UART side, so it works even while USB is the stuck path.
3. Build with `make BOARD=flipper …`, flash with `reboot -d` and `dfu-util`, keep dev mode on so
   a hang that trips the watchdog lands back in DFU.

## 8.6 A working session

1. Flash the image under test; the board is in dev mode from `reboot -d`.
2. Open two terminals: the MINIX USB console and the bridge (UART), both logging to files.
3. Reproduce the problem from the USB side (or with the tool that triggers it, such as
   `twoflip.py`).
4. When USB stops answering, look at the UART: is the kernel alive (Enter gives a prompt)? Did it
   print a panic or a warning? Take the debug dump if the build has one.
5. Compare the two logs: what was sent, what the host received, what the kernel says it did.
6. A panic prints on both lines and then stops kicking the watchdog; in dev mode the board then
   returns to DFU after about 2 s. The UART log keeps the panic text even when the USB terminal
   lost it.
7. When done, `reboot` (as root) to leave dev mode, or flash the next image.

Record findings in the PR that fixes them, with the log excerpt (as #298 does).
