# 7. Desktop modules **(planned)**

> **Outline.** Nothing in this chapter exists in the code yet. It describes the design decided in
> spec K.2–K.4 so that tools and modules can be written towards it. It becomes a full chapter
> when the desktop lands (with Phase 5 and the launcher work). Until then, write full-screen apps
> (chapter 4).

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
