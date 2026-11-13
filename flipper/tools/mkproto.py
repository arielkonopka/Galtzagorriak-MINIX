#!/usr/bin/env python3
"""Expand @BINFILES@ in the root file system prototype.

Usage: mkproto.py TEMPLATE OUT BINDIR "prog prog ..." "alias=prog ..."
Each program becomes "name ---755 2 0 BINDIR/name"; aliases (other names
of multi-call programs such as cp/mv/rm) are installed as copies."""
import sys

template, out, bindir, progs, aliases = sys.argv[1:]
lines = [f"      {p:<14} ---755 2 0 {bindir}/{p}" for p in progs.split()]
for a in aliases.split():
    name, prog = a.split("=")
    lines.append(f"      {name:<14} ---755 2 0 {bindir}/{prog}")
with open(template) as f:
    text = f.read()
if "@BINFILES@" not in text:
    sys.exit(f"{template}: no @BINFILES@ line")
with open(out, "w") as f:
    f.write(text.replace("      @BINFILES@", "\n".join(lines)))
