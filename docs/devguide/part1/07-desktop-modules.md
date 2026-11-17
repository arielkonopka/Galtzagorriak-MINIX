# 7. Desktop modules and window apps

> **Status.** Window apps (section 7.5, spec K.8) are in the code: the launcher is their
> window server, `libgm` is the library. Tiles, notifications and the module FIFO (sections
> 7.1–7.4, spec K.2–K.4, task T23a) are still **planned**: those sections describe the design so
> that modules can be written towards it.

## 7.1 The model

The GUI will be one base app, the **desktop**: status bar, home-screen tiles, a notification
list and the app menu. Other programs extend it as **modules**:

- **A module is its own process**, with its own uid and groups. A crash or a garbage-collection
  pause in one module never stalls the desktop or the other modules. Expect at most about four
  modules at once.
- **Modules may be written in C, Berry or shell.** All of them speak the same text protocol, so
  anything that can write a line can be a module. Modules whose logic has settled move to C
  (less RAM, no GC pauses); Berry and shell are for new, rarely run or user-written modules.
- **The desktop owns the screen and does navigation itself.** Scrolling a menu or moving focus
  never waits for a module to be scheduled. Modules get events like "item 2 selected", so the
  button-to-pixel budget (L1) does not depend on them.
- **Modules that refresh periodically are not resident.** A clock or weather tile is a `timed`
  timer that runs the module, which sends one `tile` record and exits (0 B between refreshes).
- **Alarms never depend on the desktop.** A module may edit alarm timers; ringing is always
  the C `alert` program (chapter 6).
- Complex apps still take the whole screen, as today.
- Every window the desktop draws for a module starts with the same black title bar as the
  launcher and today's apps ([chapter 4.3](04-screen-apps.md#43-the-look-the-title-bar)); a
  module only sends the title text and, in the `window` record's `hints` field, the button hints for
  the bottom line ("Pine meets modern UX", spec K.2 "Screen style"; chapter 4). The desktop
  draws both, so the module spends no RAM on the look.

## 7.2 The protocol (spec K.3)

- Modules write **records** to the desktop's FIFO (`/dev/gui` or `/var/run/gui`, to be decided
  in the implementation). One record per `write()`, at most `PIPE_BUF` bytes, so records from
  different modules never interleave.
- A record is one line of **tab-separated fields**: the verb, the module id, then the verb's
  fields. Escapes inside fields: `\t`, `\n`, `\\`.
- Verbs from modules: `tile`, `status`, `notify`, `window` (a menu, a text, a form), `close`.
- The desktop answers on a per-module reply FIFO with event records: `select`, `back`,
  `input`, `redraw`.
- The desktop keeps a fixed table (about 16 tiles and the last 8 notifications) in its own
  memory; nothing is written to flash.
- For debugging, a record typed on the USB console shows its effect, and the desktop gets a
  switch that logs its traffic.

The field lists of each verb are not fixed yet; this section gets them with the implementation.

## 7.3 What a module will look like

Sketches, to show the shape; the exact fields will differ.

A tile from the shell, run by a timer every ten minutes:

```sh
#!/bin/sh
# battery tile: one record, then exit
soc=`sed 's/.*soc=\([0-9]*\).*/\1/' /dev/power`
printf 'tile\tbattery\tBattery\t%s %%\n' "$soc" > /var/run/gui
```

A C module with a menu would open its reply FIFO, write a `window` record listing the items,
then block reading events and act on `select` and `back`. The same in Berry uses the `gui`
binding (chapter 8).

## 7.4 Related rules already in force

- **Structured output** (spec K.1): commands that print records support `-t` (TSV with a header
  line, units in the field names). Modules can then pipe command output into records with
  little code.
- Objects and pipelines of objects exist only inside one Berry process (`pipe` and `tsv`
  modules, chapter 8). Between processes everything is text lines.

## 7.5 Window apps (spec K.8)

A **window app** describes its screens in TSV records; the desktop (today the launcher) draws
them, moves the cursor, changes values, edits times and dates and types text on its own
keyboard. The app has no `libfuri`, no Canvas and no gui thread, and it sleeps in `read()` until
the person does something that needs it. An open window app costs about 4 KB of RAM (the
example `gmdemo`: 2.5 KB of stack and heap, 16 B of `.bss`, the shared libc's data); a
`libfuri` app costs about 15 KB. Use a full-screen app (chapter 4) only when the app has to draw
something of its own (a waterfall, a game, a picture).

**Starting.** The package says `Type: window` (`mkpkg.py --window`), and its postinst writes
its launcher line with `=` before the program:

```
Demo:nobody:@app:=/usr/bin/gmdemo
```

The launcher starts it as its child, as `user` in that user's group (no `gui` group: it never
opens `/dev/lcd`), with two pipes: **fd 3** carries events to the app, **fd 4** records to the
desktop. Nothing has a name in the file system, so no other process can talk to the window.

**Records** (app → desktop; one per line, tab-separated, `\t` `\n` `\\` escapes inside fields):

| Record | Meaning |
|---|---|
| `win ID KIND TITLE [ROWS=N FIRST=K]` | a window begins; KIND `list`, `text`, `clock` or `confirm` |
| `item KEY LABEL [RIGHT]` | an entry that opens something (OK → `ok`) |
| `int KEY LABEL MIN MAX STEP VALUE [UNIT]` | a number, changed with Left/Right |
| `enum KEY LABEL INDEX OPT\|OPT\|…` | a choice, Left/Right |
| `bool KEY LABEL 0\|1` | on/off |
| `time KEY LABEL HH:MM` | edited a field at a time (OK, Left/Right, Up/Down) |
| `date KEY LABEL YYYY-MM-DD` | the same; years 2000–2127 |
| `days KEY LABEL MASK` | week days, bit 0 Monday … bit 6 Sunday |
| `input KEY LABEL TEXT MAX` | text up to MAX (≤ 63) characters, the desktop's keyboard |
| `line TEXT` | a static line (the text of `text`, `clock` and `confirm` windows) |
| `action KEY LABEL` | a button row (OK → `submit`, then `ok`) |
| `show` | the window is complete: the desktop draws it |
| `busy`, `idle` | a long job: the hourglass on / off; `idle` also means "done, nothing to show" |
| `close` | the app ends its window |

A window has at most 16 rows, 640 bytes of records from `win` to `show` and 512 bytes of text
in its fields; a bigger or malformed window is dropped (the message line says `bad window`), a
malformed row is dropped alone. A record line is at most 127 characters.

**Events** (desktop → app):

| Event | When |
|---|---|
| `ok WIN KEY` | OK on an `item` or `action` row (or on a `text`/`clock`/`confirm` window: its first action, `yes` on a confirm without one) |
| `submit WIN KEY=VALUE …` | the values that changed, before `ok`, `page`, `back` and `close`, as more lines if they do not fit one. Values: numbers, the enum's index, `0`/`1`, `HH:MM`, `YYYY-MM-DD`, the days mask, the text |
| `back WIN` | Back on a child window (the desktop already shows the parent) |
| `redraw` | send the current window again (after `back` when the desktop did not keep the parent) |
| `page WIN FIRST` | a long list (`ROWS=N`) needs the page starting at row FIRST: answer with `win WIN list TITLE ROWS=N FIRST=FIRST` and its rows |
| `close` | Back on the first window: save and exit. EOF on fd 3 means the same (the desktop is gone) |

**The window stack.** A `win` with a new ID opens over the current window; the desktop keeps
that parent, so Back shows it at once. The same ID again replaces the window (the cursor stays).
The ID of the kept parent goes back to it. Deeper stacks work too, but beyond two windows the
desktop asks for the parent with `redraw`.

**Never blocking.** The desktop writes events without blocking; when the app does not answer an
`ok` or `page` within 150 ms the title bar shows an hourglass, and keys still work. Back on the
first window closes the app at once (`close`, then both pipes closed); an app still alive 2 s
later gets `SIGTERM` (and `SIGKILL` 2 s after that). An app must not spend long between
reading an event and answering: send `busy` first, or answer with `idle`.

**`libgm`** (`src/lib/gm/gm.h`, static, link `gm.o`; no `malloc`, no stdio, 16 B of state):

```c
#include "gm.h"

int main(void)
{
	char buf[128], ev[128], *f[GM_FIELDS], *k, *v;
	int vol = 40, i;

	if (gm_init(buf, sizeof buf) < 0) return 1;	/* not started by the desktop */
	gm_win("main", "list", "Demo", 0, 0);
	gm_int("vol", "Volume", 0, 100, 5, vol, "%");
	gm_item("about", "About", "");
	if (gm_show() < 0) return 1;			/* the desktop is gone */
	for (;;) switch (gm_event(ev, sizeof ev, f, GM_FIELDS)) {
	case GM_SUBMIT:
		for (i = 2; f[i] != NULL; i++) {
			k = gm_kv(f[i], &v);
			if (strcmp(k, "vol") == 0) vol = atoi(v);
		}
		break;
	case GM_OK:			/* f[1] the window, f[2] the key */
		gm_win("about", "text", "About", 0, 0);
		gm_line("Hello from a window app");
		gm_show();
		break;
	case GM_CLOSE:
		return 0;
	}
}
```

`gm_init` takes the record buffer (records are built in it and written whole lines at a time,
at most 512 bytes per `write()`); rows go with `gm_item`, `gm_int`, `gm_enum`, `gm_bool`,
`gm_time`, `gm_date`, `gm_days`, `gm_input`, `gm_line`, `gm_action`; `gm_show` ends the window
and sends it; `gm_busy(1)`/`gm_busy(0)` send `busy`/`idle`. `gm_event` waits for the next event
in the same buffer each time and splits it into `f` (ended by `NULL`). The full example is
`flipper/apps/gmdemo.c`; the `gmdemo` package installs it with `gm.c` and `gm.h` in
`/usr/share/examples/gui/`. Build it as `minix.mk` does (`OBJS_gmdemo`, `CHMEM_gmdemo := 2560`).

**Debugging.** `GuiLog: on` in `/etc/launcher.conf` copies every record (`gui< `) and event
(`gui> `) to the console; dropped records show as `gui! dropped`. The parser is
`flipper/gui/winrec.c` (pure functions, host test `flipper/tests/host/wintest.c`), the server
`flipper/gui/winsrv.c`.
