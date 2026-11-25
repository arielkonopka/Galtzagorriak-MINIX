#!/usr/bin/env python3
"""sizereport - flash size of every user program, against a budget.

Usage: sizereport.py BUDGET PROGRAM.elf ...
Prints one line per program, biggest first: code+rodata (text, runs from
flash in place), data (copied to RAM), bss, and the flash a program takes
(text + data).  BUDGET has lines "name max-flash-bytes  # reason" and one
line "total max-flash-bytes"; a program above its line, or a total above
its line, fails (exit 1).  Programs without a line are only reported: add
one to freeze a program's size, so a command cannot creep up unnoticed.
Set SIZEREPORT_TOP=n to print only the n biggest (default 25; 0 = all).
"""
import os, subprocess, sys

def budget(path):
    b = {}
    for l in open(path):
        l = l.split('#')[0].split()
        if len(l) == 2:
            b[l[0]] = int(l[1])
    return b

def size(elf):
    out = subprocess.run(["arm-none-eabi-size", elf], capture_output=True,
                         text=True, check=True).stdout.splitlines()[-1]
    t, d, b = (int(x) for x in out.split()[:3])
    return t, d, b

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    bud = budget(sys.argv[1])
    rows = []
    for elf in sys.argv[2:]:
        name = os.path.basename(elf).split('.')[0]
        t, d, b = size(elf)
        rows.append((t + d, name, t, d, b))
    rows.sort(reverse=True)
    top = int(os.environ.get("SIZEREPORT_TOP", "25"))
    total = sum(r[0] for r in rows)
    bad = 0
    print("%-14s %8s %7s %7s %8s" % ("program", "flash", "data", "bss",
                                     "budget"))
    for i, (f, name, t, d, b) in enumerate(rows):
        lim = bud.get(name)
        over = lim is not None and f > lim
        bad += over
        if top == 0 or i < top or over:
            print("%-14s %8d %7d %7d %8s%s" % (name, f, d, b,
                  lim if lim is not None else "-",
                  "  OVER" if over else ""))
    lim = bud.get("total")
    over = lim is not None and total > lim
    bad += over
    print("%-14s %8d %7s %7s %8s%s" % ("total", total, "", "",
          lim if lim is not None else "-", "  OVER" if over else ""))
    print("sizereport: %d programs, %s" % (len(rows),
          "%d over budget" % bad if bad else "within budget"))
    sys.exit(1 if bad else 0)

main()
