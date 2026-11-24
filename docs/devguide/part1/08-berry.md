# 8. Scripting in Berry **(planned)**

> **Outline.** Berry is not in the system yet (spec Phase 7c, after Phases 5 and 7b). This
> chapter records what is decided so scripts and bindings can be planned. It fills in with real
> examples when the runtime lands.

## 8.1 Why Berry

The user chose [Berry](https://github.com/berry-lang/berry) (MIT) on 2026-11-14 over Lua,
MicroPython, mJS and Forth (comparison in spec 7c.0). The deciding points for this machine:

- it keeps library tables **and** compiled scripts in flash without patches (constant objects
  and `solidify`), so code costs almost no RAM;
- it starts in under 4 KB of heap;
- its syntax is close to Python and Lua;
- it is proven for exactly this kind of sensor and gadget scripting (Tasmota).

## 8.2 How scripts will run

- **Each script is its own process** with its own interpreter state and heap. A GC pause stalls
  only that script, never the foreground app or the system.
- **The interpreter is shared, in flash.** Ten scripts cost ten small states, not ten
  interpreters (R7c.1).
- **Precompiled at install** (R7c.2): `pkg` compiles scripts to bytecode and puts bytecode and
  constants into flash; the source stays on the SD card for editing.
- **`#!` scripts** run from the shell and the launcher through a small `berry` binary; script
  apps are packages like any other, with a manifest, their own uid and groups.
- **REPL**: `berry -i` on the USB console (and later the Bluetooth serial) for trying things live
  (R7c.5).

Expected cost per running script: about 4–6 KB of RAM including its stack, plus its own data;
nothing between timer events (AC7c.1 measures it on the device).

## 8.3 Bindings (R7c.3)

One C module each, in flash. A binding checks access through the normal device permissions, so a
script gets no more than a C app with the same manifest.

| Module | For |
|---|---|
| `sensor` | `/dev/sensor/*` readings (spec H) |
| `timer` | `timed` timers: register a `run` action and be started with `--timer id`, so the script is not resident between events |
| `alert` | notifications (`notify_post`) |
| `gpio`, `adc` | header pins and voltages |
| `ir`, `subghz`, `nfc`, `rfid` | high-level read and send; raw streams stay with C apps |
| `gui` | canvas, menus, dialogs, input events |
| `storage` | files under the app's data directory |
| `usbhid` | USB keyboard and mouse |
| `pipe`, `tsv` | object pipelines inside one script (`where`, `sortby`, `head`, `map`), reading and writing TSV at the edges (spec K.1) |

## 8.4 Limits (R7c.4)

- Heap limit per script from its manifest, default 8 KB.
- An instruction budget per event callback: a runaway loop ends with an error, not with the
  launcher killing things.
- The long Back press ends a script app like any other app.
- Target: GC pauses ≤ 10 ms with an 8 KB heap (AC7c.3).

## 8.5 Examples that will ship (R7c.6)

A thermometer logger with an alarm in about 20 lines, a sonar beeper, a GPIO blink and button
counter, and a "drink water" reminder. They will be the first sections of this chapter.

## 8.6 Writing for Berry before it exists

- Put sensor and device logic you may later script behind the device files of chapter 3; the
  bindings are thin layers over them.
- Keep periodic work in the run-once form of chapter 6, so a script can be a `timed` action.
- Prefer text protocols (TSV lines) between programs; Berry's `tsv` module reads them directly.
