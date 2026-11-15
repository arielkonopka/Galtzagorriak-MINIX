#!/usr/bin/env python3
"""make versioncheck (spec J.4): GZ_MAJOR/MINOR/PATCH, GZ_RELEASE and the
newest released section of CHANGELOG.md must agree.

Usage: versioncheck.py include/minix/gzversion.h CHANGELOG.md"""
import re
import sys

h = open(sys.argv[1]).read()
v = {k: re.search(rf"#define\s+GZ_{k}\s+(\S+)", h).group(1)
     for k in ("MAJOR", "MINOR", "PATCH", "RELEASE")}
nums = f"{v['MAJOR']}.{v['MINOR']}.{v['PATCH']}"
rel = v["RELEASE"].strip('"')
m = re.search(r"^## (\d+\.\d+\.\d+)", open(sys.argv[2]).read(), re.M)
log = m.group(1) if m else None
bad = []
if nums != rel:
    bad.append(f"GZ_MAJOR.MINOR.PATCH is {nums}, GZ_RELEASE is {rel}")
if log != rel:
    bad.append(f"CHANGELOG.md's newest release is {log}, GZ_RELEASE is {rel}")
for b in bad:
    print("versioncheck:", b)
if bad:
    sys.exit(1)
print(f"versioncheck: {rel} ok")
