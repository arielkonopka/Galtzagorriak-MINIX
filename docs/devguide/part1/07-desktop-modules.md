# 7. Desktop modules and window apps

> **Status.** Window apps (section 7.5, spec K.8) and the desktop core (task T23a: the HOME
> page, the status line, the FIFO `/var/run/gui` with `tile`, `status`, `notify` and `close`,
> `guimod`) are in the code. Still **planned**: the `window` record and reply FIFOs for
> background modules, the Notifications page (T23c), the C helper in `libapp` and the Berry
> binding.

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

- Modules write **records** to the desktop's FIFO **`/var/run/gui`** (root's, mode 0620, group
  `gui`; the launcher makes it at start). One record per `write()`, at most **128 bytes** with
  its newline, so records from different modules never interleave. Longer records are dropped.
- A record is one line of **tab-separated fields**: the verb, the module's name, its uid, its
  process id, then the verb's text. Escapes inside the text: `\t`, `\n`, `\\` (tabs and
  newlines are shown as spaces: a tile is one line).

  | Record | What it does |
  |---|---|
  | `tile NAME UID PID TEXT` | sets or changes the module's row on HOME (31 characters shown) |
  | `status NAME UID PID TEXT` | its short text on the status line (6 characters shown; the newest status wins) |
  | `notify NAME UID PID TEXT` | counts a notification and shows the text for 2 s |
  | `close NAME UID PID` | removes the module's tile and status |

- **A module is UID + NAME** (1–10 characters). A record changes only that module's entries, so
  one user cannot change or close another user's tile with the same name.
- **PID** is the module's process. A `tile` or `status` record of a process that has exited is
  dropped, and the desktop removes the entries of modules that have exited (`kill(pid, 0)`, at
  each key and once a minute while HOME is shown). So a module is a resident process (a loop with
  `sleep`); killing it removes its tile. (`timed`-run modules that refresh and exit, spec K.4,
  need a rule for tiles that outlive their writer: a follow-up.)
- A FIFO does not tell the reader who wrote into it, so **`guimod`** is set-uid root: it writes
  the caller's real uid and its parent's pid (the shell running the module) into the record and
  drops root right after opening the FIFO. A C module in group `gui` may write records itself
  (see `dk_record()` in `flipper/gui/desktop.c`); the desktop believes the uid it writes.
- The desktop keeps a fixed table, **16 entries of 48 B** for tiles and statuses together, in its
  own `.bss`; a full table drops new modules' records. Nothing is written to flash, nothing is
  allocated. The parser (`dk_parse`, `dk_apply`, `dk_purge`) is a pure function, tested on the PC
  by `tests/host/desktest.c`.
- **Responsiveness.** The launcher reads the FIFO only when `poll()` says so, keys first, at
  most four reads at a time; a key never waits for a module. It redraws only for a change that
  is on the screen (tiles only on HOME). Writers never block: `guimod` opens and writes with
  `O_NONBLOCK`, so with no desktop (`ENXIO`) or a full FIFO (`EAGAIN`, while a full-screen app
  is open) the record is dropped and `guimod` exits 1.
- **No card writes.** Records live only in memory (spec K.6): FS keeps pipe and FIFO data in
  the buffer cache as `PIPE_DIRTY` blocks that `sync`/`update` never write, and pipe I/O does
  not dirty the FIFO's inode. A module may write as often as it likes without wearing the SD
  card or leaving the file system unclean (ACK.5: 0 sectors in M2).
- Planned: the `window` record and per-module reply FIFOs with event records (`select`,
  `back`, `input`, `redraw`), the Notifications page with the last 8 (T23c).

## 7.3 Writing a module

A battery tile from the shell, `/usr/share/examples/gui/battile.sh` (package `gmdemo`):

```sh
IFS='= '
while read key soc rest </dev/power
do guimod tile battery "Battery $soc%"; sleep 60; done
```

Run it with `sh battile.sh &`. `/dev/power` gives one line `soc=87 mv=3912 ...`; with `=` and
space as separators `read` puts the charge in `soc`. The loop costs a shell's RAM while it
sleeps; a C module costs much less.

A C module builds the same record with `dk_record()` (`flipper/gui/desktop.c`) and writes it
with one `write()`:

```c
char rec[DK_LINE];
int n = dk_record(rec, sizeof rec, "tile", "clock", getuid(), getpid(), "12:30 lunch");
int fd = open(DK_PATH, O_WRONLY | O_NONBLOCK);	/* group gui, or root */

if (fd >= 0 && n > 0) write(fd, rec, n);	/* EAGAIN: the desktop is busy */
```

A module with a menu will use the `window` record and its reply FIFO when they come; the same
in Berry uses the `gui` binding (chapter 8).

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
| `win ID KIND TITLE [ROWS=N FIRST=K] [LIVE]` | a window begins; KIND `list`, `text`, `clock` or `confirm`. `LIVE` (`gm_win_live()`): every value change is sent at once, see `submit` |
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
| `submit WIN KEY=VALUE …` | the values that changed, before `ok`, `page`, `back` and `close`, as more lines if they do not fit one; in a `LIVE` window also right after each change (a time, date or days row: when its edit ends), so the app can apply it at once (the settings groups: brightness, volume, the clock); the app need not answer. Values: numbers, the enum's index, `0`/`1`, `HH:MM`, `YYYY-MM-DD`, the days mask, the text |
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
in the same buffer each time and splits it into `f` (ended by `NULL`). An app that also waits on another file (a
child's pipe, as Bad USB does) `poll()`s `GM_EVFD` together with it, but asks `gm_pending(buf)`
first: `gm_event` may already hold the next event in its buffer, and then fd 3 stays quiet. The full example is
`flipper/apps/gmdemo.c`; the `gmdemo` package installs it with `gm.c` and `gm.h` in
`/usr/share/examples/gui/`. Build it as `minix.mk` does (`OBJS_gmdemo`, `CHMEM_gmdemo := 2560`).

**A real app: Kanpaia** (`flipper/apps/remind/remind.c`, T25a) adds timers (libapp), paged lists
and a form. Two things keep it at 4.0 KB open:

- **GOT words.** User programs are PIC with `-mno-pic-data-is-text-relative`, so every static
  variable and string literal that sits in its own section costs a 4-byte GOT entry in RAM.
  `minix.mk` builds a window app and its own copies of `gm.c`, `nat_core.c` and libapp with
  `-fno-data-sections -fno-merge-constants` (`WIN_CFLAGS`). Each object then has one `.data`,
  `.bss` and `.rodata` that share one entry through section anchors, which cut remind's GOT from
  868 B to 124 B.
- **No heap, so no gap.** MM's 1.5 KB stack/heap gap is checked only by `brk()` and when a caught
  signal is delivered. An app with no `malloc`, no `opendir` (it mallocs 1 KB; read the
  directory in 16 B `struct _v7_direct` entries instead) and no signal handlers can use its whole
  `CHMEM` as stack. Measure it with `free -t on` (the `memuse:` line at exit).

**Debugging.** `GuiLog: on` in `/etc/launcher.conf` copies every record (`gui< `) and event
(`gui> `) to the console; dropped records show as `gui! dropped`. The parser is
`flipper/gui/winrec.c` (pure functions, host test `flipper/tests/host/wintest.c`), the server
`flipper/gui/winsrv.c`.
