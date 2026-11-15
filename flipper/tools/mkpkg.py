#!/usr/bin/env python3
"""Build a Galtzagorriak package (a tar archive read by /bin/pkg).

Usage:
  mkpkg.py -n NAME -v VERSION [-d DEP,DEP] [-D "description"]
           [--postinst FILE] [--prerm FILE] -o OUTDIR  dest=source ...

Each dest=source puts the host file `source` at path `dest` on the Flipper,
e.g. /usr/bin/hello=build/flipper/minix/bin/hello (mode 755 if the path is
in a bin directory, else 644; ",MODE" after the source sets it, e.g. ,4755
for a set-uid program, which is then owned by root).  Writes OUTDIR/NAME_VERSION.tar in the old
POSIX ustar format that MINIX 2.0 tar understands (names < 100 chars).
"""
import argparse
import sys
import struct
import io
import os
import tarfile
import time


def add(tar, name, data, mode):
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = mode
    if mode & 0o6000:                    # set-uid/gid: owned by root
        info.uid, info.gid = 0, 0
        info.uname, info.gname = "root", "operator"
    else:
        info.uid, info.gid = 2, 0        # bin, operator: like /bin
        info.uname, info.gname = "bin", "operator"
    info.mtime = int(os.environ.get("SOURCE_DATE_EPOCH", time.time()))
    if len(name) >= 100:
        raise SystemExit(f"path too long for MINIX tar: {name}")
    tar.addfile(info, io.BytesIO(data))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", "--name", required=True)
    ap.add_argument("-v", "--version", required=True)
    ap.add_argument("-d", "--depends", default="")
    ap.add_argument("-D", "--description", default="")
    ap.add_argument("--postinst")
    ap.add_argument("--prerm")
    ap.add_argument("-o", "--outdir", default=".")
    ap.add_argument("--os", help="Os: line, the release (base package)")
    ap.add_argument("files", nargs="+", metavar="dest=source")
    a = ap.parse_args()

    # Abi: the ABI of its programs (MINIX a.out, a_version; 0 = 1), or
    # "any" for a package without programs (spec J.3).
    abis = set()
    for spec in a.files:
        with open(spec.split("=", 1)[1].rsplit(",", 1)[0], "rb") as f:
            h = f.read(8)
        if len(h) == 8 and h[0:2] == b"\x01\x03":
            abis.add(struct.unpack_from("<H", h, 6)[0] or 1)
    if len(abis) > 1:
        sys.exit(f"mkpkg: programs for different ABIs: {sorted(abis)}")
    control = (f"Package: {a.name}\nVersion: {a.version}\n"
               + (f"Depends: {a.depends}\n" if a.depends else "")
               + f"Abi: {abis.pop() if abis else 'any'}\n"
               + (f"Os: {a.os}\n" if a.os else "")
               + (f"Description: {a.description}\n" if a.description else ""))
    out = os.path.join(a.outdir, f"{a.name}_{a.version}.tar")
    os.makedirs(a.outdir, exist_ok=True)
    with tarfile.open(out, "w", format=tarfile.USTAR_FORMAT) as tar:
        add(tar, "CONTROL/control", control.encode(), 0o644)
        for script, name in ((a.postinst, "postinst"), (a.prerm, "prerm")):
            if script:
                with open(script, "rb") as f:
                    add(tar, f"CONTROL/{name}", f.read(), 0o755)
        for spec in a.files:
            dest, src = spec.split("=", 1)
            dest = dest.lstrip("/")
            mode = 0o755 if "/bin/" in "/" + dest else 0o644
            if "," in src:
                src, m = src.rsplit(",", 1)
                mode = int(m, 8)
            with open(src, "rb") as f:
                add(tar, dest, f.read(), mode)
    print(f"  {out}: {os.path.getsize(out)} bytes")


if __name__ == "__main__":
    main()
