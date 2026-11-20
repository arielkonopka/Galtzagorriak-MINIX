#!/usr/bin/env python3
"""ACS.2 on QEMU (spec S.10, T33a part 3): copy the files of
tools/updatetest.py (SRC: cases.txt, the .update files) to DST under short
names (c00, c01, ...; the root file system has 14-character names) and
write DST/run, the sh script m2test runs: `gzupdate check -e E -r R` on
each file must exit 0 (good) or 1 (refused, one line why on the console);
the good file's window text goes to the console once.  No pipes and no
temporary files, so m2test can check that nothing was written.  The script exits with the number of the first
wrong case + 1, or 0.

Usage: gzucases.py SRC DST"""
import os
import shutil
import sys

src, dst = sys.argv[1:3]
shutil.rmtree(dst, ignore_errors=True)
os.makedirs(dst)
lines = ["G=/bin/gzupdate", "D=/lib/gzu"]
shown = False
n = 0
for ln in open(os.path.join(src, "cases.txt")):
    if ln.startswith("#") or not ln.strip():
        continue
    name, epoch, rel, want = ln.split()
    short = "c%02d" % n
    shutil.copyfile(os.path.join(src, name), os.path.join(dst, short))
    code = 0 if want == "good" else 1
    out = " >/dev/null" if want == "good" and shown else ""
    shown = shown or want == "good"
    lines.append(f"$G check -e {epoch} -r {rel} $D/{short}{out}; "
                 f"[ $? = {code} ] || exit {n + 1}")
    n += 1
lines.append("exit 0")
with open(os.path.join(dst, "run"), "w") as f:
    f.write("\n".join(lines) + "\n")
print(f"gzucases: {n} files in {dst}")
