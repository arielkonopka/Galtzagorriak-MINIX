# 4. Screen apps and the launcher

A *screen app* is an ordinary program that draws on `/dev/lcd` and reads `/dev/buttons`. The
launcher starts it, hands it the screen and buttons, and waits until it exits. This chapter
covers the small `gui` library the built-in apps use, the launcher's rules, and how to keep an
app quick.

The larger framework for ported stock apps (`libfuri`: views, scenes, menus, dialogs) and the
desktop with tiles and notifications are **(planned)** (spec Phase 5 and K.2). What is described
here keeps working when they come.

## 4.1 The `gui` library

`flipper/gui/gui.h` and `gui.c`, linked into each app (there are no shared libraries yet, so
each app carries its own copy, about 1–2 KB of text in flash). The screen is 128x64 pixels;
text uses a 4x6 font, so 32 columns by 10 rows.

| Call | What it does |
|---|---|
| `gui_open()` | opens `/dev/lcd` and `/dev/buttons` (blocking and non-blocking); clears the frame; 0 or -1 |
| `gui_close()` | closes them |
| `gui_clear()` | clears the frame in memory |
| `gui_pixel(x, y, on)`, `gui_hline`, `gui_vline`, `gui_fill`, `gui_frame` | drawing |
| `gui_text(x, y, s, inverse)` | text at pixel position; returns the width used |
| `gui_center(y, s, inverse)` | text centred on a line |
| `gui_text_big(x, y, s, k)` | text k times larger |
| `gui_title(s)` | an inverse title bar on the top line |
| `gui_flush()` | writes the frame to the screen (one `write`) |
| `gui_key()` | waits for the next button **press**; returns `KEY_*`, or 0 if a signal interrupted the wait |
| `gui_key_nowait()` | the next press or 0, without waiting |
| `gui_led(r, g, b)`, `gui_backlight(level)` | the LED and the backlight |

The frame is 1 KB of static data in the app. Drawing happens in memory; nothing reaches the
screen until `gui_flush()`.

## 4.2 A complete app

A counter: Up and Down change a number, OK buzzes, Back quits.

```c
/* tally - count things on the Flipper. */
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "gui.h"

static void buzz(void)
{
	int fd = open("/dev/vibro", O_WRONLY);

	if (fd >= 0) {
		write(fd, "60", 2);	/* 60 ms; the kernel ends it */
		close(fd);
	}
}

int main(void)
{
	int n = 0, k;
	char line[16];

	if (gui_open() < 0) return 1;
	for (;;) {
		gui_clear();
		gui_title("TALLY");
		sprintf(line, "%d", n);
		gui_text_big((GUI_W - 12 * (int) strlen(line)) / 2, 20, line, 3);
		gui_center(56, "UP/DOWN  OK buzz  BACK", 0);
		gui_flush();

		k = gui_key();
		if (k == KEY_BACK) break;
		if (k == KEY_UP) n++;
		if (k == KEY_DOWN && n > 0) n--;
		if (k == KEY_OK) buzz();
	}
	gui_close();
	return 0;
}
```

To build it as a system app, put it in
`flipper/gui/tally.c` and add `tally` to `GUIAPPS` in `flipper/minix.mk`; that links it with
`gui.c`. The small built-in apps are the best further examples: `lights.c` (LED and backlight),
`info.c` (About), `power.c` (a periodic refresh, 4.6), `console.c` (handing the screen to the
text console).

## 4.3 The look: the title bar

The launcher and every app share one visual convention, and it is a rule (owner decision
2026-11-16, spec K.2): **each screen starts with the title bar**, a black bar across the full
width of the top text row with the screen's name in white, centred, in capitals. Call
`gui_title("NAME")` first after `gui_clear()`, and draw your content below it (from y = 8 on).

- Use the app's launcher label as the title, in capitals (`TALLY`, `POWER`); a sub-screen may
  use its own name (`SETTINGS`, `HISTORY`).
- Keep the title short enough to fit with space around it (32 characters is the hard limit of
  the row; aim for under 16).
- Do not draw your own variant of the bar (another height, left-aligned text, a frame). If a
  screen needs more in the bar, such as a page number, propose a `gui` library extension so all
  apps get it the same way.
- Exceptions: the text console, full-screen content that a bar would hide (a picture, a
  graph that needs all 64 rows), and alert screens.

The planned `libfuri` views, the Berry `gui` binding and the desktop's windows draw the same bar,
so apps look alike whatever they are written in.

## 4.4 The launcher and `/etc/apps`

The launcher (`flipper/gui/launcher.c`) is started as root by `/etc/rc` and shows the entries of
`/etc/apps`, one per line:

```
# Label:user:/program arg ...
Console:nobody:/bin/console
Tally:nobody:/usr/bin/tally
```

- Up to 16 entries, 8 arguments each. Lines starting with `#` are comments.
- The app runs as the named user, in group `gui` (screen, buttons, LED, vibro, speaker, GPIO,
  infrared, iButton), with the user's home directory as the working directory. Only an entry
  naming `root` keeps root's rights. If the user does not exist or the ids cannot be set, the
  app does not start: an app never runs as root by accident.
- `/etc/apps` is read with `cfopen()`: the card's copy if there is one, else the default in
  `/rom/etc/apps`. A package that adds an app appends its line in its `postinst` script
  (chapter 5); the base package does it for `Power` (see `flipper/rootfs/base.postinst`).
- Manifests in packages (label, icon, category, uid, groups) that the launcher reads by itself
  are **(planned)** (spec R5.5, R5.6, F.2).

## 4.5 The foreground app and the long Back press

When the launcher starts an app it makes it the **foreground app** with
`ioctl(fd, FGIOCSET, &pid)` on `/dev/lcd` (`<minix/fg.h>`, spec R4b.8). From then on:

- only the foreground app's frames reach the screen; others are dropped;
- only the foreground app gets button events; other readers wait;
- **holding Back for one second sends `SIGTERM`** to the foreground app, and `SIGKILL` three
  seconds later if it is still there. This is the stock "leave the app" gesture, and it works
  even if the app hangs.

What that means for an app:

- **A short Back press is yours.** Use it for "back one level" and quit from the top level.
- **`SIGTERM` means "quit now".** The default action ends the process, which is fine for most
  apps. If you have something to save, catch it, save quickly (well under three seconds), and
  exit. Do not ignore it.
- If an app starts helper processes, they are not the foreground: the screen belongs to the
  app's own pid. A helper that needs the screen gets it handed over with the same `ioctl` by the
  current foreground process.
- A background program (a daemon, a script) that writes to `/dev/lcd` while an app is in front
  draws nothing. That is intended; notifications that may interrupt an app go through `alert`
  with `FG_PREEMPT` **(planned)**, spec F.2.

When the app exits, the launcher clears the foreground and redraws its menu.

## 4.6 Doing something while waiting for a key

`gui_key()` blocks, and a blocked process costs no CPU. To refresh the screen every few seconds
without polling, use `alarm()`: the signal interrupts the wait, `gui_key()` returns 0, and the
loop redraws. The Power app does exactly this:

```c
static void tick(int sig)
{
	signal(SIGALRM, tick);		/* MINIX signal(): re-arm the handler */
}
...
	signal(SIGALRM, tick);
	for (;;) {
		read_and_draw();
		alarm(1);			/* next refresh in a second */
		k = gui_key();			/* 0: the alarm came */
		alarm(0);
		if (k == KEY_BACK) break;
		handle(k);
	}
```

Notes:
- `alarm()` has one-second resolution (ticks of 1/60 s underneath). Sub-second timers for
  programs (`setitimer`) and `poll()` with a timeout are **(planned)** (R4b.2, R4b.6).
- Refresh slower when nobody looks: the Power app reads every 10 s while the backlight is off
  (it reads `/dev/backlight` to know). Each wake-up costs battery.
- Never spin on `gui_key_nowait()` in a loop. It keeps the CPU out of its sleep states and steals
  time from everything else. It is meant for draining pending keys, not for waiting.

## 4.7 Keeping an app quick

The budgets of [chapter 1](01-platform.md#18-responsiveness-is-a-requirement-not-a-wish) apply.
In practice:

- **Draw the first frame before anything slow.** `gui_open()`, a title, `gui_flush()`, and only
  then read files or hardware. L6 (launch to first frame) is 300 ms, and most of it is already
  spent by `exec`.
- **Redraw right after the key.** Compute after the frame if the computation is long, and show
  progress; the user must see the press was taken.
- **Exit promptly.** L7 (app exit to menu) is 150 ms.
- **Keep data small.** The app's data, bss and chmem are what it costs; text is in flash. A
  1 KB frame buffer is fine; a 20 KB table is not (make it `const`).
- **Write files to the SD card, never flash**, and not on every key press; the card is shared
  with the screen's bus.
- **Install it into flash** (`pkg install --flash`) when it is finished, so its text is not
  copied into RAM at every start.

## 4.8 Shell scripts as screen apps

Any program can be in `/etc/apps`, including a shell script, but a shell costs about 12 KB and
cannot draw text conveniently. Use scripts in the launcher for actions (toggle something, run a
job, then exit) and C for anything with a screen. Desktop modules written in shell or Berry that
only send text records to the desktop are **(planned)** (chapter 7).
