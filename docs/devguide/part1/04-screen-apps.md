# 4. Screen apps and the launcher

A *screen app* is an ordinary program that draws on `/dev/lcd` and reads `/dev/buttons`. The
launcher starts it, hands it the screen and buttons, and waits until it exits. This chapter
covers the small `gui` library the built-in apps use, the launcher's rules, and how to keep an
app quick.

The larger framework for ported stock apps (`libfuri` GUI: views, scenes, menus, dialogs) is in
section 4.9; the desktop with tiles and notifications is **(planned)** (spec K.2). What is
described here keeps working.

## 4.1 The `gui` library

`flipper/gui/gui.h` and `gui.c`, linked into each app (only libc is shared so far, spec R2b.9, so
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

`libfuri` apps in the Window layer get the same bar (section 4.9); the planned Berry `gui`
binding and the desktop's windows will draw it too, so apps look alike whatever they are
written in.

## 4.4 The launcher and `/etc/apps`

The launcher (`flipper/gui/launcher.c`) is started as root by `/etc/rc` and shows the entries of
`/etc/apps`, one per line:

```
# Label:user:icon:/program arg ...
Console:nobody:@console:/bin/console
Tally:nobody:#3C42A5A5423C:/usr/bin/tally
Old:nobody:/usr/bin/old          (no icon field: still valid)
```

- Up to 16 entries, 8 arguments each. Lines starting with `#` are comments.
- The icon (spec R5.6a.1) is `@name` (a built-in one: the list is in the manual's launcher
  chapter), `#` and 12 hex digits (8×6 pixels, a byte per row from the top, MSB left), one or two
  characters, or empty. The program must start with `/`: that is how the launcher tells the
  two forms apart. A bad icon is just not drawn. `flipper/gui/appline.c` parses the line; its
  host test is `flipper/tests/host/icontest.c`.
- Packages: `mkpkg.py -i @bell` puts `Icon: @bell` in the manifest; until the launcher reads
  manifests, the `postinst` writes the same value into its line, e.g.
  `echo 'Tally:nobody:@game:/usr/bin/tally' >> /etc/apps`.
- Keys that switch pages or toggle things on a release (Left/Right in the launcher) use
  `gui_input()` with those keys as clicks: a click is a key pressed alone and released within
  ~1 s, so a two-button gesture never clicks. Up/Down/OK keep acting on the press (latency L1).
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
  with `FGIOCPREEMPT` (R7b.2, below).
- **An alert over your app** (R7b.2): `ioctl(fd, FGIOCPREEMPT, 0)` on `/dev/lcd` or
  `/dev/buttons` takes the screen and the buttons from whoever has them, until
  `FGIOCRELEASE` or the alert's exit; a second alert meanwhile gets `EBUSY` and waits its turn.
  Your app keeps running: its frames are dropped and its button reads wait. A read that is
  waiting when the alert comes, or that you start meanwhile, **ends with 0 bytes** at the
  preemption and again at the release: treat a 0-byte read as "redraw and read again" (the
  `gui` library's `gui_key()` returns 0, and the launcher just redraws). MINIX 2 has no free
  signal number for `SIGWINCH`. The alert puts the picture it covered back itself.

When the app exits, the launcher clears the foreground and redraws its menu. If an app that
was given the screen exits without handing it back, the kernel clears it at the next clock
tick.

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
- `alarm()` has one-second resolution (ticks of 1/60 s underneath). For a sub-second wait use
  `poll()` with a timeout (ms, rounded up to the tick; R4b.6); `setitimer` is **(planned)**
  (R4b.2). `poll()` on `/dev/buttons` waits for a key (R4b.6), so an app can wait for keys, a pipe and a
  timeout at once.
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

## 4.9 The `libfuri` GUI (ported stock apps)

`flipper/lib/furi` (spec R5.2) gives ported stock apps the stock firmware's `gui` API with the
same names and behaviour: `#include <gui/gui.h>`, `<gui/view_dispatcher.h>`,
`<gui/scene_manager.h>`, `<gui/elements.h>`, `<gui/modules/submenu.h>` (also `popup.h`,
`dialog_ex.h`, `text_input.h`, `byte_input.h`, `variable_item_list.h`, `widget.h`,
`loading.h`), `<input/input.h>`, `<assets_icons.h>`; link `libfuri.a`. `flipper/lib/furi/hello.c`
(`furihello`) is a complete example. What differs from the stock firmware:

- The GUI service lives in the app's own process. The first `furi_record_open(RECORD_GUI)` starts
  it: a cooperative "gui" thread with a 2 KB stack (from the app's `chmem`) that reads
  `/dev/buttons`, turns the events into the stock sequence (Press; Short and Release on a release
  before 300 ms; else Long at 300 ms, Repeat every 150 ms, Release) and gives them to the top
  enabled ViewPort, and redraws when a ViewPort or View asks (`view_port_update()`, a model
  update). There is no periodic redraw: a frame is one 1024-byte write to `/dev/lcd`. Keys that
  were queued before the app started are dropped.
- Draw and input callbacks run on the gui thread; keep them short (spec AC5.R: no thread may hold
  the input thread more than 10 ms). Long work belongs in another furi thread.
- `GuiLayerFullscreen` gets all 128x64 pixels. `GuiLayerWindow` and `GuiLayerDesktop` keep the
  stock window (from y = 13), and the 13 pixels above carry the black title bar with the app's
  appid in capitals instead of the stock status bar.
- `ViewDispatcher` has no `FuriEventLoop`: `view_dispatcher_run()` waits on one queue (input and
  custom events, 16 of 8 bytes) with the tick period as its timeout. `view_dispatcher_alloc_ex()`
  and `view_dispatcher_get_event_loop()` do not exist.
- Icons are stored uncompressed (`tools/icons.py` makes the C data from PNGs, as the stock asset
  tool, so `canvas_draw_icon()` needs no decompression buffer); a heatshrink-compressed icon
  stops the app with `furi_check`. Fonts: the stock four (`FontPrimary` helvB08,
  `FontSecondary` haxrcorp 4089, `FontKeyboard` profont11, `FontBigNumbers` profont22).
- `malloc()` in files that include `<furi.h>` returns zeroed memory and never NULL, as the stock
  heap; the stock modules depend on it. Define `FURI_LIBC_MALLOC` first to get the C library's.
- No left-handed mode, no `ViewHolder`, `ViewStack`, `FileBrowser` (it needs storage, R5.3),
  `ButtonMenu`, `ButtonPanel`, `TextBox`, `NumberInput`, `DateTimeInput` or `EmptyScreen` yet.
- Costs: a Submenu-and-Popup app links about 20 KB of GUI code (the program is linked with
  `--gc-sections`, so only what it calls) plus the 8 KB core; RAM is the 1 KB frame, about 0.6 KB
  of GUI state, the gui thread's 2 KB stack and the views' models.

Host tests: `make -C flipper/lib/furi test` renders every module and compares it with the PBM
images in `flipper/lib/furi/tests/gui` (`make gui-ref` rewrites them; look at them before you
commit). The QEMU self-test runs `furihello` with fake buttons.
