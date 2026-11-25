# Galtzagorriak MINIX Development Guide

How to write software for Galtzagorriak MINIX on the Flipper Zero, and how to work on the
system itself.

Galtzagorriak MINIX started as MINIX 2.0.0, but it is no longer the MINIX of the book. It runs
on a Cortex-M4 without an MMU, executes the kernel and most program text straight from flash,
has about 100 KB of RAM for all user processes, owns a 128x64 screen and six buttons, and has
its own rules for RAM, responsiveness and flash wear. Whatever you know about MINIX 2.0 or a
Unix-like system mostly still holds. The places where it does not are what this guide is about.

The guide is written in installments. It describes the system as it is on the main branch.
Features that the spec plans but that are not in the code yet are marked **(planned)** with the
spec section that brings them, as in the [user manual](../manual/README.md). A chapter that only
has an outline so far says so at the top.

## Part 1: Writing software for the platform

For advanced users who want to write programs, apps, scripts and background jobs.

1. [The platform as a programmer sees it](part1/01-platform.md): what a process gets, the
   memory model without an MMU, program text in flash, `chmem`, the C dialect and library,
   signals, the limits that bite first.
2. [Your first C program](part1/02-first-program.md): where the source goes, building,
   sending it to the Flipper, running it, measuring its memory, testing on the PC.
3. [Using the hardware: device files](part1/03-devices.md): `/dev/lcd`, `/dev/buttons`, LED,
   backlight, vibro, speaker, GPIO, ADC, infrared, iButton, 125 kHz RFID, Sub-GHz, the RTC,
   `poll()` and the pulse engine; NFC and Bluetooth **(planned)**; from C and from the shell.
3a. [How the 125 kHz RFID radio works](part1/03a-rfid-radio.md): the coil, load modulation, the
    pins and timers, ASK/FSK/PSK, what we measured, a troubleshooting table.
4. [Screen apps and the launcher](part1/04-screen-apps.md): the `gui` library, the
   "Pine meets modern UX" screen style, `/etc/apps`, the foreground app, the long Back press,
   and how to keep an app quick.
5. [Packages and installing into flash](part1/05-packages.md): package format, `mkpkg.py`,
   `pkg`, scripts run at install, settings with defaults, putting program text into flash.
6. [Daemons, jobs and background work](part1/06-background.md): what works today, the
   background tiers, timers and alerts (`timed`, `alert`, `libapp`), swap, and how to choose.
7. [Desktop modules and window apps](part1/07-desktop-modules.md): window apps with `libgm`
   (spec K.8, in the code), and modules in C, Berry or shell that talk to the desktop over a
   TSV line protocol (spec K.2–K.4, **planned**).
8. [Scripting in Berry](part1/08-berry.md) **(planned, outline)**: the Berry runtime, its
   bindings and limits (spec Phase 7c).

## Part 2: Working on Galtzagorriak MINIX itself

For people changing the kernel, the servers, the drivers and the build.

1. [Architecture](part2/01-architecture.md): the source tree, the boot, the system images, how
   processes run on ARM without an MMU, the flash layout, the RTC backup registers.
2. [Building, testing and CI](part2/02-build-test.md): toolchain, make targets, QEMU, the
   self-test, the security tests, power-cut and flash checks, latency measurements, CI,
   flashing a board during development.
3. [Writing a driver](part2/03-drivers.md): a minor in the device task, interrupts, the ms
   timer, sleep blockers, the HAL for both boards, the `/dev` node on old and new cards;
   user-space drivers **(planned)**.
4. [Servers and system calls](part2/04-servers-calls.md): how MM and FS are extended, the
   `MEMINFO` controls, kernel calls, and what each costs.
5. [Rules and review](part2/05-rules-review.md): the RAM, responsiveness, flash-write and
   security rules every change is reviewed against, and the PR checklist.
6. [Testing radios against a second Flipper](part2/06-two-flipper-tests.md): a stock
   (Unleashed) Flipper as the partner for RFID, IR, iButton and Sub-GHz tests, the physical
   setup without special parts, and `twoflip.py` on Linux, macOS or Windows.
7. [The overnight battery test](part2/07-overnight-power-test.md): AC6.6 (no reset, clock,
   more than 99 % in Stop2) and the night's current, set up from a fresh computer, `nightpower.py`,
   reading and recording the result, and the nights measured so far.
8. [Dev mode and the debug serial line](part2/08-debug-uart.md): `reboot -d` and what dev mode
   changes, the debug UART on pins 13/14, reading it through a second Flipper's USB-UART Bridge or
   a USB-serial adapter, and a debugging session with both lines.
9. [Signing the update file in CI](release-update-job.md): the one-time key and GitHub
   environment steps for the user, and the `release-update` job to add to `build.yml` (RS.18).

## Where else to look

- [`docs/manual/`](../manual/README.md): the user manual (installing, devices, power).
- [`docs/spec/`](../spec/README.md): the design. The spec decides; this guide explains.
  Section numbers like R6.3 or K.3 point there.
- [`flipper/PORTING.md`](../../flipper/PORTING.md): the port's engineering log, with
  measurements and the reasons behind many decisions.
- [`CHANGELOG.md`](../../CHANGELOG.md): what changed in each release.

## Keeping this guide current

The guide follows the code like the manual does (standing rule "docs follow behaviour" in
[`docs/spec/next-tasks.md`](../spec/next-tasks.md)). A PR that changes an interface described
here (a device file, a library call, a build target, a rule) updates the chapter, or removes a
**(planned)** mark when it brings the feature. New chapters arrive as the features they describe
land: desktop modules with Phase 5 and the desktop, Berry with Phase 7c, timers and alerts with
Phase 7b, user-space drivers with Phase 4b.
