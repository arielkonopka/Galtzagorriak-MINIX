#!/usr/bin/env python3
"""make versioncheck (spec J.4, RS.19): GZ_MAJOR/MINOR/PATCH, GZ_RELEASE and
the newest released section of CHANGELOG.md must agree, and GZ_EPOCH must go
up with every release: compared with the newest tag v* in the history, a new
GZ_RELEASE needs a higher GZ_EPOCH, the same GZ_RELEASE the same GZ_EPOCH (a
tag without GZ_EPOCH, before T33a, counts as epoch 0).  Without git or tags
(a tarball, a shallow clone) the epoch rule is skipped and says so.

Usage: versioncheck.py include/minix/gzversion.h CHANGELOG.md"""
import os
import re
import subprocess
import sys


def fields(h):
    v = {}
    for k in ("MAJOR", "MINOR", "PATCH", "RELEASE", "EPOCH"):
        m = re.search(rf"#define\s+GZ_{k}\s+(\S+)", h)
        v[k] = m.group(1) if m else None
    return v


hpath = sys.argv[1]
h = open(hpath).read()
v = fields(h)
nums = f"{v['MAJOR']}.{v['MINOR']}.{v['PATCH']}"
rel = v["RELEASE"].strip('"')
m = re.search(r"^## (\d+\.\d+\.\d+)", open(sys.argv[2]).read(), re.M)
log = m.group(1) if m else None
bad = []
if nums != rel:
    bad.append(f"GZ_MAJOR.MINOR.PATCH is {nums}, GZ_RELEASE is {rel}")
if log != rel:
    bad.append(f"CHANGELOG.md's newest release is {log}, GZ_RELEASE is {rel}")
if v["EPOCH"] is None or not v["EPOCH"].isdigit():
    bad.append("GZ_EPOCH missing (spec RS.19)")


def git(*a):
    r = subprocess.run(["git", "-C", os.path.dirname(os.path.abspath(hpath)),
                        *a], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


note = ""
if not bad:
    tag = git("describe", "--tags", "--abbrev=0", "--match", "v*", "HEAD")
    top = git("rev-parse", "--show-toplevel")
    if tag and top:
        rp = os.path.relpath(os.path.abspath(hpath), top)
        old = git("show", f"{tag}:{rp}")
        if old is None:
            note = f", no {rp} in {tag}: epoch rule skipped"
        else:
            o = fields(old)
            orel = (o["RELEASE"] or '""').strip('"')
            oep = int(o["EPOCH"]) if o["EPOCH"] and o["EPOCH"].isdigit() else 0
            ep = int(v["EPOCH"])
            if orel != rel and ep <= oep:
                bad.append(f"GZ_RELEASE {rel} is new since {tag} ({orel}) but"
                           f" GZ_EPOCH {ep} is not higher than its {oep}"
                           " (spec RS.19: +1 every release)")
            elif orel == rel and o["EPOCH"] and ep != oep:
                bad.append(f"GZ_EPOCH {ep} differs from {tag}'s {oep} but "
                           f"GZ_RELEASE is still {rel}: raise both together")
            else:
                note = f", epoch {ep} ({tag}: {oep})"
    else:
        note = ", no git tag v*: epoch rule skipped"
for b in bad:
    print("versioncheck:", b)
if bad:
    sys.exit(1)
print(f"versioncheck: {rel} ok{note}")
