#!/usr/bin/env python3
"""ramcheck - static RAM of the system images against ram-budget.txt.

Usage: ramcheck.py BUDGET IMAGE.elf ...
Prints .data/.bss/total per image (arm-none-eabi-size) and the budget;
exits 1 if any image's .data + .bss exceeds its budget line.
"""
import os, subprocess, sys

def budget(path):
    lines = {}
    for l in open(path):
        l = l.split('#')[0].split()
        if len(l) == 4:
            lines[l[0]] = int(l[3])
    return lines

def main():
    b = budget(sys.argv[1])
    bad = 0
    print("%-8s %7s %7s %7s %7s" % ("image", "data", "bss", "total", "budget"))
    for elf in sys.argv[2:]:
        name = os.path.basename(elf).split('.')[0]
        out = subprocess.run(["arm-none-eabi-size", elf], capture_output=True,
                             text=True, check=True).stdout.splitlines()[-1]
        text, data, bss = (int(x) for x in out.split()[:3])
        tot = data + bss
        lim = b.get(name)
        mark = ""
        if lim is None:
            mark = "  (no budget line)"
            bad = 1
        elif tot > lim:
            mark = "  OVER by %d" % (tot - lim)
            bad = 1
        elif tot < lim:
            mark = "  (%d under)" % (lim - tot)
        print("%-8s %7d %7d %7d %7s%s" % (name, data, bss, tot,
                                         lim if lim is not None else "-", mark))
    sys.exit(bad)

main()
