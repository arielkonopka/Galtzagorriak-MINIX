#!/usr/bin/env python3
"""bsscheck - .bss of the base commands against a 4 KB limit (spec R2a.5).

Usage: bsscheck.py EXCEPTIONS LIMIT PROGRAM.elf ...
Every program whose .bss is above LIMIT bytes must have a line
"name max-bytes  # reason" in EXCEPTIONS and stay within max-bytes.
Prints the programs over the limit and exits 1 on any failure, so a
command cannot creep back above 4 KB unnoticed.
"""
import os, subprocess, sys

def exceptions(path):
    ex = {}
    for l in open(path):
        l = l.split('#')[0].split()
        if len(l) == 2:
            ex[l[0]] = int(l[1])
    return ex

def bss(elf):
    out = subprocess.run(["arm-none-eabi-size", elf], capture_output=True,
                         text=True, check=True).stdout.splitlines()[-1]
    return int(out.split()[2])

def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    ex = exceptions(sys.argv[1])
    limit = int(sys.argv[2])
    bad = 0
    big = []
    for elf in sys.argv[3:]:
        name = os.path.basename(elf).split('.')[0]
        b = bss(elf)
        if b > limit:
            big.append((b, name))
    for b, name in sorted(big, reverse=True):
        lim = ex.get(name)
        if lim is None:
            mark = "  OVER %d B, no exception line" % limit
            bad = 1
        elif b > lim:
            mark = "  OVER its exception %d by %d" % (lim, b - lim)
            bad = 1
        else:
            mark = "  (exception, max %d)" % lim
        print("bsscheck: %-10s .bss %6d%s" % (name, b, mark))
    print("bsscheck: %d programs, %d above %d B of .bss, %s"
          % (len(sys.argv) - 3, len(big), limit, "FAIL" if bad else "ok"))
    sys.exit(bad)

main()
