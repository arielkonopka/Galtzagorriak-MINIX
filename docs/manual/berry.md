# Berry scripts

Berry is the scripting language of Galtzagorriak MINIX (spec Phase 7c). It looks like Python
and Lua, has classes and closures, and was made for small devices. Install it with
`pkg install /lib/pkg/berry_1.0.tar` (the package `berry`).

## Running a script

```
berry /usr/lib/berry/hello.be         # a source script
berry /usr/lib/berry/primes.bec 50    # compiled bytecode; arguments in _argv
berry -e 'print(6 * 7)'               # code from the command line
berry -i                              # the REPL on the console; Ctrl-D ends it
```

`_argv` is a list: the script's name first, then its arguments. The examples are in
`/usr/lib/berry`, each as source (`.be`, for reading and editing) and as bytecode (`.bec`).

## Limits

Each script is its own process with its own small heap, so a bad script cannot slow down the
rest of the system:

- `-m BYTES`: the heap limit, 8192 by default (`-m 0`: none). Over it the script stops with
  "berry: heap limit reached" and exit status 4.
- `-n N`: the instruction budget, in units of 1024 instructions, 4096 by default (about four
  million instructions; `-n 0`: none). A script that runs too long, such as an endless loop,
  stops with `budget_error` and exit status 3. In the REPL each line gets a new budget.

Other exit statuses: 0 the script ended normally, 1 an error in the script (the message and the
line are printed), 2 wrong arguments, 5 the file could not be read.

## RAM

The interpreter is a shared library in internal flash (spec R7c.1): its code and its constant
tables (the built-in strings, classes and modules) are there once, for every running script. A
script's process holds only its own state: 1.1 KB of data (the C library's and Berry's
per-process variables) plus the room for its heap and stack, 12 KB, which the system reserves
when the script starts so that the default 8 KB heap (`-m`) always fits. One running script
takes about 13.3 KB; two take twice that. `free -v` shows the library ("shared Berry: 84 KB at
...") and its data per process.

Only use `-n 0` for testing: two endless scripts without a budget can still make the system stop
answering (being looked into).

## Bytecode

Packages carry their scripts compiled. The host tool `berryc` (`make -C src/lib/berry`) makes the
bytecode: `berryc -c script.bec script.be`. The Flipper loads `.bec` files without compiling them.

## Modules

- `tsv`: the tab-separated text that commands print with `-t`. `tsv.split(line)` gives the
  fields, `tsv.join(list)` makes a line, `tsv.records(text)` gives one map per line, keyed by
  the header line. Inside fields, `\t`, `\n` and `\\` stand for a tab, a newline and a backslash.
- `pipe`: `pipe.where(list, f)`, `pipe.map(list, f)`, `pipe.head(list, n)` and
  `pipe.sortby(list, key)`, where `key` is a function or, for records, a field name.
- `sensor`: `sensor.list()` names the sensors in `/dev/sensor`, `sensor.read(name)` gives a
  sensor's line or `nil`. There are no sensor drivers yet, so the list is empty.
- `string`, `global`, `sys` and `gc` from Berry itself.

`/usr/lib/berry/records.be` shows `tsv` and `pipe`.

## Not there yet

- **(planned, R7c.3)** More bindings: `timer`, `alert`, `gpio`, `adc`, `gui`, `storage` and the
  radios.
- **(planned, R7c.1)** Starting a script with `#!/usr/bin/berry`.
- **(planned, R7c.4)** A heap size per script package (`Heap:` in its manifest), so a small
  script reserves less than 12 KB; the spec's goal is 6 KB for an idle script.
- The `math` module, and `os`, `time` and `json`.
