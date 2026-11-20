#!/usr/bin/env python3
"""Expand @BINFILES@ in the root file system prototype.

Usage: mkproto.py TEMPLATE OUT BINDIR "prog prog ..." "alias=prog ..." ["pkg.tar ..."]
                   ["suidprog ..."] [GZUDIR]
Each program becomes "name ---755 2 0 BINDIR/name"; aliases (other names
of multi-call programs such as cp/mv/rm) are installed as copies.  Set-uid
programs are owned by root, mode 4755; "prog:GID" in that list makes prog
set-gid GID instead (owner bin, mode 2755).  @BINDIR@ in the template becomes
BINDIR (for programs placed elsewhere, such as /lib/pwdauth).  GZUDIR (QEMU, ACS.2):
its files go to /lib/gzu (@GZUFILES@; tools/gzucases.py makes them)."""
import sys

template, out, bindir, progs, aliases = sys.argv[1:6]
pkgs = sys.argv[6].split() if len(sys.argv) > 6 else []
suid = set(sys.argv[7].split()) if len(sys.argv) > 7 else set()
# "prog:GID" in the set-uid list: set-gid to group GID instead (owned by
# bin, as the other programs), e.g. infrared:13 (group ir, AC7.3).
sgid = {e.split(":")[0]: e.split(":")[1] for e in suid if ":" in e}
suid -= {e for e in suid if ":" in e}
gzudir = sys.argv[8] if len(sys.argv) > 8 else ""
lines = [f"      {p:<14} -u-755 0 0 {bindir}/{p}" if p in suid
         else f"      {p:<14} --g755 2 {sgid[p]} {bindir}/{p}" if p in sgid
         else f"      {p:<14} ---755 2 0 {bindir}/{p}" for p in progs.split()]
for a in aliases.split():
    name, prog = a.split("=")
    lines.append(f"      {name:<14} ---755 2 0 {bindir}/{prog}")
with open(template) as f:
    text = f.read()
if "@BINFILES@" not in text:
    sys.exit(f"{template}: no @BINFILES@ line")
pkglines = [f"      {p.split('/')[-1]:<14} ---644 0 0 {p}" for p in pkgs]
text = text.replace("      @PKGFILES@", "\n".join(pkglines))
gzu = []
if gzudir:
    import os
    gzu = ["      gzu d--755 0 0"] + \
        [f"         {n:<14} ---644 0 0 {os.path.join(gzudir, n)}"
         for n in sorted(os.listdir(gzudir))] + ["      $"]
text = text.replace("      @GZUFILES@\n", "".join(g + "\n" for g in gzu))
text = text.replace("@BINDIR@", bindir)
with open(out, "w") as f:
    f.write(text.replace("      @BINFILES@", "\n".join(lines)))
