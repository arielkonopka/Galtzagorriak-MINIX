#!/usr/bin/env python3
"""flashcheck - flash erases of a QEMU test against flash-budget.txt.

Usage: flashcheck.py BUDGET TEST LOG
LOG is the console output of the test (make INIT=m2 minix-run); its line
"M2 FLASH erased cache=N inst=M" holds the pages erased since boot
(R2b.11 counters).  Exits 1 when a count is above its budget line or the
line is missing.
"""
import re, sys

def main():
    budget_file, test, log = sys.argv[1:4]
    limits = {}
    for l in open(budget_file):
        f = l.split('#')[0].split()
        if len(f) == 3 and f[0] == test:
            limits[f[1]] = int(f[2])
    m = None
    for l in open(log, errors='replace'):
        m = re.search(r'M2 FLASH erased cache=(\d+) inst=(\d+)', l) or m
    if not m:
        print("flashcheck: no 'M2 FLASH erased' line in %s" % log)
        sys.exit(1)
    got = {'cache': int(m.group(1)), 'inst': int(m.group(2))}
    bad = 0
    print("%-6s %6s %6s" % ("region", "erased", "budget"))
    for region, n in got.items():
        lim = limits.get(region)
        mark = ""
        if lim is None:
            mark, bad = "  (no budget line)", 1
        elif n > lim:
            mark, bad = "  OVER by %d" % (n - lim), 1
        elif n < lim:
            mark = "  (%d under: lower the budget)" % (lim - n)
        print("%-6s %6d %6s%s" % (region, n, lim if lim is not None else "-", mark))
    sys.exit(bad)

main()
