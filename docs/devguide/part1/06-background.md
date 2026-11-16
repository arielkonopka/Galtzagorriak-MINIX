# 6. Daemons, jobs and background work

On a desktop Unix a daemon is cheap. Here every resident process costs at least 1.5–2 KB of RAM
(its stack, libc data, kernel and server table entries) and one of the 13 user process slots,
for as long as it waits. So the system is designed around **not having resident processes**
whenever possible: work that happens at a time is started at that time and exits; state that
must survive is kept in a file or in hardware.

This chapter says what you can do today, what the planned mechanisms are, and how to choose.
Much of it is **(planned)**: the timer service, alerts and the background tiers arrive with
spec Phases 7b and 12. Write new code so it can move onto them.

## 6.1 The rule: choose the cheapest tier

Spec 12d sorts background work into four tiers. An app declares its tier in its package
manifest (`Background: event|hibernate|park|stream`) and must use the cheapest one that does the
job (R12.16).

| Tier | While waiting | RAM while waiting | Typical use |
|---|---|---|---|
| 0 `event` | no process; a timer file, held by the RTC alarm hardware | 0 B | alarms, reminders, periodic logging, refreshing a tile |
| 1 `hibernate` | no process; a small state blob saved on the SD card | 0 B | an app that resumes where it left off |
| 2 `park` | process stopped, its image swapped to the SD card when RAM is needed | 0 B once swapped | ported apps with large in-memory state |
| 3 `stream` | the work runs in a kernel device task (radio, USB, DMA) | ≤ 64 B in the kernel | live radio or USB sessions |

Anything with a deadline (an alarm, a reminder, logging at a set time) must be tier 0: a fresh
`exec` always finds room, a swapped image may have to wait for its addresses.

Status: none of the four is wired up yet **(planned)**: tier 0 with `timed` (Phase 7b), tier 1
with `libapp` (R12.18, may land with 7b), tier 2 with swap (Phase 12), tier 3 per driver.
What exists today is described next.

## 6.2 What works today

### Long-running daemons started at boot

`/etc/rc` starts the system's own background programs with `&`: `update` (flushes the buffer
cache every 30 seconds) and the launcher. INIT only restarts the login lines of `/etc/ttytab`;
nothing else is restarted when it dies.

If you really need a resident daemon now:

- **Write it in C, not shell.** A shell loop costs about 12 KB resident; a small C daemon costs
  2–4 KB.
- **Block, never poll.** Wait in `read()` on a device or FIFO, in `pause()`/`sigsuspend()` for a
  signal, or in `sleep()`. A sleeping process uses no CPU and lets the CPU enter Stop2.
- **Start it from `/etc/rc`** after the file systems are mounted. A package adds the line in its
  `postinst` with `ed`, the way `base.postinst` edits `/etc/rc`, and removes it in `prerm`. There
  is no `rc.d` directory or service manager yet; if several packages need this, that is the
  moment to add one (ask in the project).
- **Drop root.** `/etc/rc` runs as root. Call `setgid()` then `setuid()` to a dedicated user as
  the first thing.
- **Detach.** Close or redirect stdin/stdout/stderr (to `/dev/null` or `/dev/log`), and call
  `setsid()` so a hangup on the console does not reach it.
- **Log sparingly** to `/dev/log`, not to a file on every event (the card is shared with the
  screen's bus).
- **Handle `SIGTERM`**: finish the current step and exit. `halt` and `reboot` send it.
- **Measure** with `free -t on` and `free -v` (chapter 2) and record the number in your PR.

The owner's expected everyday load is about three things in the background at once: the alarm
clock, timed reminders, and one more (an irrigation scheduler, a USB canary, a jiggler). If each
of those were a resident 4 KB daemon plus a shell, RAM for apps would shrink visibly; that is why
the planned design starts them on demand.

### Periodic jobs

There is no `cron`. Today a periodic job is a small C program that loops on `sleep()`. That
works but stays resident. Keep it tiny, and plan to turn it into a `timed` timer when Phase 7b
lands: write the job so that one run does one step and exits, and wrap that in the loop for now.

```c
/* one step of the job; exits non-zero on error */
static int step(void) { ... }

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "--once") == 0)	/* the timed form */
		return step();
	for (;;) {					/* the interim form */
		step();
		sleep(60);
	}
}
```

### Talking to a daemon

Use a FIFO (`mkfifo`): the daemon blocks reading it, clients write one line per request. A write
of up to `PIPE_BUF` bytes is atomic, so lines from several clients never mix. Answers go back
through a second FIFO per client, or a file. The desktop protocol (chapter 7) uses the same
pattern. There are no sockets.

## 6.3 Timers and alerts (`timed`, `alert`) **(planned, spec F.2)**

The planned tier-0 machinery, so you can design for it now:

- **Timers are files.** One file per timer in `/var/timers/`, owned by the app's uid, mode
  0600. Fields: `Kind` (`once`, `daily` with a weekday mask, `interval` inside a daily window),
  `Category` (`alarm`, `reminder`, `info`), `Action` (`notify` with title, text, sound, vibro,
  LED pattern and snooze, or `run`: a program of the same package started with `--timer <id>`),
  and `Missed` (`fire-once` or `skip`).
- **Nothing waits in RAM.** A short-lived `timed` reads the spool, fires what is due, programs
  the earliest next time into the RTC's Alarm A and exits. When the alarm fires, the kernel
  signals INIT, which forks `timed` again. The same happens after the clock is set.
- **Alerts take the screen, not the app.** `alert` grabs the screen and buttons with
  `ioctl(FG_PREEMPT)`, rings, vibrates and blinks (LED patterns run inside the LED chip), and
  gives the screen back. The foreground app keeps running.
- **Alarms always ring**, even in do-not-disturb, and never depend on the desktop or a script:
  scripts only edit the spool.
- **`libapp`** wraps it: `timer_add()`, `timer_remove()`, `timer_list()`, `notify_post()`,
  `settings_get/set()`.
- The device can even be **off**: before power-off the RTC alarm is wired to the charger's
  power-on input, so an alarm boots the Flipper and rings (spec F.3).

Until then: an alarm-like program must stay resident and use `sleep()`; it cannot wake the
device from off.

## 6.4 Hibernate and park **(planned, spec R12.18–R12.21)**

- **Hibernate** (tier 1): `app_hibernate(state, len, wake)` saves up to 4 KB to
  `/var/hib/<uid>/<app>` and exits; `app_resume(buf, max)` gets it back in the next run. The
  blob must contain **no pointers** (the next run lives at other addresses) and **no secrets**
  (it is not encrypted).
- **Park** (tier 2): an app marked `Background: park` is parked by the long Back press instead of
  ended. It must first release the screen, buttons and any radio or DMA device; it is woken by
  the launcher's Running list or by its timer. Target image size ≤ 16 KB, so that swapping it
  back in takes ≈ 15–55 ms.

You can already structure state for hibernation: keep what must survive in one plain struct with
offsets and ids, not pointers, and save it with `write()` to a file on exit.

## 6.5 Choosing, in short

1. Can it happen at a time, then stop? A `timed` timer (today: a sleep loop that you will turn
   into one).
2. Does it need to resume where it was? Save a small state file and exit (hibernate).
3. Does it watch a device? Block in `read()` in a small C daemon; for continuous radio or USB
   streams, the work belongs in the kernel driver (Part 2).
4. Is it a big app the user switches away from? Park it (later), otherwise let it end on Back.
