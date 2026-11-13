#!/usr/bin/env python3
"""Build a Galtzagorriak package (a tar archive read by /bin/pkg).

Usage:
  mkpkg.py -n NAME -v VERSION [-d DEP,DEP] [-D "description"]
           [--postinst FILE] [--prerm FILE] -o OUTDIR  dest=source ...

Each dest=source puts the host file `source` at path `dest` on the Flipper,
e.g. /usr/bin/hello=build/flipper/minix/bin/hello (mode 755 if the path is
in a bin directory, else 644).  Writes OUTDIR/NAME_VERSION.tar in the old
POSIX ustar format that MINIX 2.0 tar understands (names < 100 chars).
"""
import argparse
import io
import os
import tarfile
import time


def add(tar, name, data, mode):
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = mode
    info.uid, info.gid = 2, 0            # bin, operator: like /bin
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
    ap.add_argument("files", nargs="+", metavar="dest=source")
    a = ap.parse_args()

    control = (f"Package: {a.name}\nVersion: {a.version}\n"
               + (f"Depends: {a.depends}\n" if a.depends else "")
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
            with open(src, "rb") as f:
                add(tar, dest, f.read(), mode)
    print(f"  {out}: {os.path.getsize(out)} bytes")


if __name__ == "__main__":
    main()
