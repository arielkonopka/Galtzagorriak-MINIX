# Power plan: analysis and PR proposals for T14 (2026-11-15)

Owner request (2026-11-15, 22:42 Warsaw): "prepare a change proposal for the agent, with the
analysis, and propose the PRs; **the agent tests them**". Spec: R6.1, R6.8, R6.9, F.3, AC6.5,
AC6.6, R20a.5. Task list: `next-tasks.md` T14a–d. This file gives the agent the reasoning, the
code places to change, and the tests each PR must pass on the device.

Standing rules apply: responsiveness first (L1–L10 never worse, `lat` before/after in PORTING.md),
RAM budgets as stated, **no run-time flash writes and no option-byte writes**, don't rewrite
working MINIX core beyond what the step needs.

---

## 1. Analysis in short

### 1.1 Where the current goes today (estimate, to be measured in PR 1)

| Item | Today (`af55ab8`) | Why |
|---|---|---|
| CPU | 2–5 mA | `idle_task` only does `WFI` (`src/kernel/arm/klib.c`); PLL and HSE stay on; SysTick wakes the core 60×/s (`src/kernel/clock.c`, `HZ` 60); **SMPS not enabled**, so the core runs from the LDO (about 2× the run current) |
| CC1101 | ~1.7 mA after a cold start | after a power-on reset it sits in IDLE with the crystal running; nothing in MINIX touches SPI1 |
| IR receiver TSOP75338 | 0.35 mA | on SYS through 10 Ω, no control (hardware floor) |
| LCD ST7567 | ~0.1–0.3 mA | display on, never in power save |
| microSD | 0.1–1 mA | ext3.3V always on (PA3 not driven, 10k pull-up enables the regulator) |
| Charger, gauge, regulators, 5 V boost | ~0.1–0.2 mA | — |
| **Total** | **~5–8 mA** | stock firmware: **1.5 mA** (Flipper blog, FW 0.82) |

Targets: ≤ 1.5 mA after PR 2 (stock level), ≤ 0.6 mA after PR 4. The floor is the IR receiver.

### 1.2 Board facts the code depends on (official schematics, stock code)
- Power tree: BAT → BQ25896 BATFET → SYS → LM3281 → 3.3V (MCU, LCD, CC1101, ST25R3916 logic,
  buttons). Second LM3281 with EN = **PA3** → ext3.3V (**microSD and header pin 9 only**).
  FAN48610 5 V boost for the NFC/RFID board, always on. BAT → RS3221 LDO → **MCU VBAT**: RTC and
  backup registers survive power-off.
- **PC13 (Back) is also the charger's QON input.** Charger INT and gauge GPOUT are test pads only:
  no interrupt for USB plug or low battery.
- EXTI lines: buttons 3 (OK PH3, active high), 6 (Down PC6), 10 (Up PB10), 11 (Left PB11),
  12 (Right PB12), 13 (Back PC13); IR RX PA0 (0); CC1101 GDO0 PA1 (1); NFC IRQ PA2 (2);
  iButton PB14 (14). All different, no conflicts.
- Stock option bytes have `IWDGSTOP=1`: **the IWDG keeps counting in Stop2.** MINIX's IWDG is 2 s
  (`hal_wdt_start`, board.c) and is kicked by the clock task, woken by the clock interrupt at least
  every 32 ticks. Tickless idle breaks that assumption.
- The 60 Hz tick does more than time: `flipdev_tick()` samples the buttons, plays speaker notes,
  ends IR receive frames (`IR_GAP_MS`), flushes the LCD (`lc_dirty`), and runs the backlight
  timeout. Each of these must either get its own wake-up or keep the tick running while active.

### 1.3 Why Stop2 and not Standby/Shutdown
Stop2: ~2 µA, keeps all SRAM and peripheral registers, wakes in microseconds on HSI16. Standby and
Shutdown save at most ~1.5 µA more, but lose RAM (a wake is a reboot, L10) and the board floor
(~0.5 mA) stays anyway. Real "off" is the charger's ship mode (PR 3).

### 1.4 Wake latency (L1 ≤ 50 ms max)
EXTI → core runs on HSI16 (16 MHz) within microseconds. Handle the button at once on HSI16 and
switch HSE → PLL afterwards; the crystal start-up (order of 1 ms) then never reaches L1.

---

## 2. Proposed PRs

One branch per PR from **main**, PR title names the task. Order: PR 1 any time; PR 2 → PR 3 →
PR 4. Each PR lists RAM (`ramcheck`), flash, run-time flash writes (must be 0) and the test log.

### PR 1 — T14a: power fixes at boot (`t14a-boot-power`)
Changes:
- `flipper/hal/flipper/board.c`, after `clock_init()`: SMPS step-down (stock:
  `LL_PWR_SMPS_SetMode(LL_PWR_SMPS_STEP_DOWN)` in `furi_hal_power_init`; check RM0434 for the SMPS
  clock selection and ordering against the PLL).
- New `spi1_park()` in board.c: SPI1 pins and clock; CC1101 CS (PD0) and NFC CS (PE4) high.
  CC1101: `SRES` (0x30), wait for MISO low (CHIP_RDYn), `SPWD` (0x39). ST25R3916: direct command
  `SET_DEFAULT`, then operation-control register 0. Register values from stock
  `furi_hal_subghz.c` / `furi_hal_nfc.c` (GPL, fine). Leave SPI1 clock off afterwards.
- Unused pins analog; `DBGMCU->CR` stop/standby debug bits cleared unless the debug flag (BKP19R)
  is set.
- QEMU board: no-op stubs.

Tests (agent, on the Flipper):
1. Before/after: read CC1101 `MARCSTATE` (status register 0xF5) — expect SLEEP after; read
   ST25R3916 operation control — expect 0. Print once at boot in the dev build.
2. Cold start (battery disconnect or ship mode, not just reset): repeat test 1.
3. Idle current before/after with the launcher on screen, backlight off: external meter on the
   battery leads (or USB meter with the battery full and charging done); record in PORTING.md.
   If no meter is at hand, say so in the PR.
4. Regression: `lat` L1–L10 unchanged; SD, LCD, USB console, IR send/receive still work; QEMU
   `M2 USER PASS`.
RAM: 0 B. Flash ≈ +300 B.

### PR 2 — T14b: EXTI buttons, tickless idle, Stop2 (`t14b-stop2`)
Changes:
- Buttons on EXTI 3, 6, 10–13 (both edges; OK is active high). `flipdev_tick()` stops sampling
  them; the EXTI handler records the change and timestamp. Debounce with a short LPTIM/tick window
  only while a button is down.
- LPTIM1 on LSE as the one-shot wake timer (also R4b.2). `idle_task` (klib.c): compute the next
  deadline (clock alarms in `clock.c`, ms timers, backlight timeout, LCD flush, notes, IR frame end,
  watchdog kick), arm LPTIM1, stop SysTick, sleep; on wake add the elapsed LSE time to uptime and
  `realtime`, restart SysTick, run due work.
- **Sleep blocker**: `u32_t pm_busy` in the kernel; drivers set a bit while they need fast clocks
  (SPI/I2C/DMA transfer, pulse engine armed, speaker note playing, USB attached, UART console
  active, IR frame in progress). `pm_busy == 0` → Stop2, else `WFI` as today.
- Stop2 entry/exit as stock `furi_hal_power_deep_sleep` without the CPU2 semaphores: PLL → HSE →
  HSI16, `RCC_CFGR.STOPWUCK = 1`, `PWR_CR1.LPMS = Stop2` and `PWR_C2CR1.LPMS = Stop2`,
  `SCB_SCR.SLEEPDEEP`, `WFI`; after: clear SLEEPDEEP, HSI16 → HSE → PLL (can be finished after the
  button event is queued), RTC shadow resync.
- Watchdog: choose one and document it in the PR: (a) before a sleep longer than 1.5 s set the IWDG
  to LSI/256, RLR 4095 (~32 s; wait for PVU/RVU) and restore /64, 1000 after waking, with LPTIM1
  waking every ~25 s to kick; or (b) never sleep longer than 1.5 s. Option bytes stay untouched.
- Statistics (≤ 16 B): ms in Stop2, wake count per source (button, RTC, LPTIM, other).
Tests (agent, on the Flipper):
1. AC6.R: 100 button presses from Stop2 (idle ≥ 5 s before each), `lat` L1 p99 and max; the
   first press after a long sleep must meet L1 (≤ 50 ms max). Compare with the PR 1 baseline.
2. AC6.6: 8 h idle on battery: no watchdog reset (BKP19R reset cause), `date` vs a reference
   clock within the LSE tolerance (about ±2 s), statistics show > 99 % time in Stop2.
3. Every tick user still works: a speaker sequence plays in time, an IR remote frame is received
   and ends correctly, the backlight goes off at its timeout and on at a key, the LCD console
   flushes, `sleep 5` sleeps 5 s, USB console works (USB attached blocks Stop2).
4. Idle current on battery, recorded in PORTING.md (target ≤ 1.5 mA).
5. QEMU: no Stop2 (WFI only), `M2 USER PASS`.
RAM: kernel ≤ +24 B. Flash ≈ +1.5 KB.

### PR 3 — T14c: `/dev/power`, ship-mode power-off, alarm from off, battery profiles (`t14c-power-dev`)
Changes:
- I2C1 drivers for BQ27220 and BQ25896 (stock `lib/drivers/bq27220.c`, `bq25896.c`); `/dev/power`
  read: SoC, voltage, current, temperature, charge state, VBUS present, profile, PR 2 statistics.
  Charger ADC one-shot on demand (stock keeps it continuous).
- Polling: 1 s while the screen is lit, every few minutes in Stop2: SYSDWN → `SIGPWR` to INIT;
  VBUS appeared → start USB; below a low-voltage threshold no flash writes.
- **Battery profiles** `full` (VREG 4.208 V) / `care` (~4.0 V), kept in a free RTC backup register
  (add it to the map in PORTING.md). **Do not touch ICHG, the input current limit or ICO**
  (owner). Charging may only be paused: while Sub-GHz transmits (later, Phase 8 hook) and outside
  0–45 °C by the gauge temperature.
- Power-off (`ioctl`): RTC alarm output on PC13 (Alarm A, open-drain, active low), ext3.3V on,
  short vibro, BQ25896 ship mode. With VBUS present: stay in Stop2 with the screen off instead.
- Boot: read the wake reason (RTC ALRAF, backup register) before boot key combos; turn the alarm
  output off; send `SIGRTC` (F.2). **Boot lead**: `timed` arms Alarm A early by the measured boot
  lead (first default 10 s, measured at each start from off, kept in a backup register) and the
  alert waits for the real time.
Tests (agent, on the Flipper):
1. AC6.1: `cat /dev/power` vs the stock battery screen (flash stock, note values, flash back)
   within 2 %.
2. AC6.2: power off, wait 10 min, power on with Back: clock still right.
3. Alarm from off: set an alarm 3 min ahead, power off; it boots and rings within 1 s of the
   alarm time (record the boot lead). Repeat 3 times.
4. After an alarm boot, Back is not taken as held; after a normal power-on, Up+Back still starts
   the self test.
5. Profile `care`: charge from ~50 %; charging ends near 4.0 V (gauge voltage), `full` goes on to
   4.2 V. ICHG register read back equals the stock default before and after.
6. USB plugged while in Stop2 is noticed within the polling period; unplugged likewise.
7. `lat` L1–L10 unchanged while polling runs.
RAM: kernel ≤ +32 B. Flash ≈ +3 KB.

### PR 4 — T14d: screen-off state, SD rail, backlight dim stage (`t14d-screen-off`)
Changes:
- Backlight: default idle timeout **15 s** (owner; change the 300 fallback in `hal_bl_state()`),
  optional dim stage before off (off by default) by the LP5562 engine ramp (#81 code); also the
  short up-ramp on wake (open item from #81).
- LCD power save (ST7567: display off + all points on; RAM kept) N minutes after the backlight
  went off, on battery only; leave it on any key, no redraw. Desk mode keeps it on.
- SD: with the screen off and the card idle N minutes (cache flushed, no open writes, not busy),
  PA3 low (ext3.3V off); at the first key start re-initialising the card in parallel with the
  backlight ramp. Never while `/dev/gpio` uses header 3V3.
Tests (agent, on the Flipper):
1. From screen-off, the first key gives a frame within L1; the image is the same as before.
2. Opening an SD-resident app right after waking from SD-off: L6 within budget or the PR states
   the miss.
3. Power cut test: write a file, let the SD rail go off, power on again: `fsck`-clean, data there.
4. Idle current with screen off and SD off (target ≤ 0.6 mA), recorded in PORTING.md.
5. 8 h idle again (AC6.6) with this PR.
RAM: 0 B (flags in existing words). Flash ≈ +1 KB.

---

## 3. Measuring
- External meter on the battery leads (e.g. Nordic PPK2 or a µCurrent with a multimeter); the
  BQ27220 has 1 mA resolution, too coarse below 1 mA.
- Without a meter: battery-SoC drop over 24 h idle (coarse) plus the Stop2 statistics.
- Every PR adds a "Power" block to PORTING.md: date, build, state, current, how measured.

## 4. After T14: the `power` app (T15, spec R7.1)
The user controls how deeply and how soon the device powers down (profiles; sleep depth; the idle
ladder dim → backlight off → screen off → SD off → automatic power-off; wake sources incl. a pocket
mode; radios; header power; battery profile), sees who keeps it awake, powers off and reboots. GUI
app `power` plus the `pm` command. Settings in RTC backup registers with a copy on the SD card, no
internal-flash writes. The T14 kernel code should read its thresholds from one policy struct from
the start, so T15 only adds the editor.

## 5. Out of scope here
CPU2/BLE low-power (Phase 16, R16.1.8), wake-on-radio (R8.5), phone-tap wake (R14.2.1), IR
remote wake (R20a.5).
