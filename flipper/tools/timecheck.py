#!/usr/bin/env python3
"""timecheck - the Year-2038 audit of spec R2d.7 (Phase 2d).

time_t is a signed 64-bit integer since ABI 2; stored times (inodes, utmp)
are ftime32_t, unsigned 32-bit.  A time kept in an int or a long breaks in
2038 (signed) and a 64-bit time_t passed where printf or a varargs helper
expects a long breaks at once.  This script greps the C sources it is given
(files or directories) for the usual forms:

  long/int variable that holds time(...)       long now = time(0);
  a cast of a time to long or int              (long) time(0), (long) st.st_mtime
  time(&x.ut_time)                             ut_time is 32 bits: overflow
  a long/int variable or field named *time*    long current_time;
  %ld / %lu / %d on a line with a time value   printf("%ld", st.st_mtime)

A line that is right as it is (a tick count, a duration, a value that is
range-checked first) carries the comment  /* timecheck: ok */ .
Exit 1 and one line per finding if anything is found.
"""

import os
import re
import sys

TIMEVAL = r"(?:\btime\s*\(|\bst_[amc]time\b|\but_time\b|\bactime\b|\bmodtime\b)"
INTTYPE = r"\b(?:unsigned\s+)?(?:long|int)\b"
RULES = [
    ("long/int holds time()",
     re.compile(INTTYPE + r"(?:\s+\**\w+\s*(?:=[^,;]*)?,)*\s+\**\w+\s*=\s*"
                r"(?:\([^)]*\)\s*)?time\s*\(")),
    ("time cast to long/int",
     re.compile(r"\(\s*(?:unsigned\s+)?(?:long|int)\s*\)\s*\(?\s*"
                r"(?:[\w.>-]*(?:st_[amc]time|actime|modtime)\b|time\s*\()")),
    ("time() into the 32-bit ut_time",
     re.compile(r"\btime\s*\(\s*&\s*[\w.>-]*ut_time\s*\)")),
    ("long/int named *time*",
     re.compile(r"^\s*(?:static\s+|register\s+|extern\s+)*" + INTTYPE +
                r"\s+[^;(]*\b\w*_?time\b(?!\w)\s*[,;=]")),
    ("%l/%d next to a time value",
     re.compile(r"%[-+ #0-9.]*l?[duxL].*" + TIMEVAL)),
]
DECL = re.compile(r"^\s*(?:static\s+|register\s+)*" + INTTYPE + r"\s+([^;(]*);")
NAME = re.compile(r"\**\s*(\w+)\s*(?:=[^,]*)?$")
OK = "timecheck: ok"


def files(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, _, names in sorted(os.walk(p)):
                for n in sorted(names):
                    if n.endswith((".c", ".h")):
                        yield os.path.join(root, n)
        elif os.path.exists(p):
            yield p


def strip_comments(text):
    """Comments and strings' contents out, line numbers kept; a line whose
    comment says timecheck: ok is emptied."""
    out = []
    for line in text.split("\n"):
        out.append("" if OK in line else line)
    text = "\n".join(out)
    text = re.sub(r"/\*.*?\*/",
                  lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return text.split("\n")


def main(argv):
    if len(argv) < 2:
        print("usage: timecheck.py file-or-dir ...", file=sys.stderr)
        return 2
    found = 0
    for f in files(argv[1:]):
        with open(f, encoding="latin-1") as fh:
            lines = strip_comments(fh.read())
        ints = set()			# names declared long/int in the file
        for line in lines:
            m = DECL.match(line)
            if m:
                for part in m.group(1).split(","):
                    n = NAME.match(part.strip())
                    if n:
                        ints.add(n.group(1))
        later = None
        if ints:
            names = "|".join(sorted(ints))
            later = re.compile(r"(?:\b(?:" + names + r")\s*=\s*(?:\([^)]*\)\s*)?"
                               r"time\s*\(|\btime\s*\(\s*&\s*(?:" + names +
                               r")\s*\))")
        for i, line in enumerate(lines, 1):
            for what, rx in RULES + ([("long/int variable set from time()",
                                       later)] if later else []):
                if rx.search(line):
                    print(f"{f}:{i}: {what}: {line.strip()}")
                    found += 1
                    break
    if found:
        print(f"timecheck: {found} place(s) keep a time in 32 bits "
              f"(spec R2d.7); fix them or mark /* {OK} */")
        return 1
    print("timecheck: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
