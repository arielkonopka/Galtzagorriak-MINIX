#!/usr/bin/env python3
"""mkbadpkg - crafted packages that pkg must refuse (spec R3.6, AC3.4).

Usage: mkbadpkg.py OUTDIR

Writes OUTDIR/{dep,long,dotdot,name,abs,link}.tar; tests/pkgbad.c installs
each one and checks that pkg says why and installs nothing.  Each holds a
harmless marker file usr/tests/bad/got_NAME, which must not appear.

  dep     Depends: a 255-character name
  long    a member path of 250 characters (ustar prefix + name: MINIX tar
          ignores the prefix and would unpack it somewhere else)
  dotdot  a member ../pkgbad_dotdot (out of the scratch directory)
  name    Package: ../x (the database file would be outside the database)
  abs     a member with an absolute path, /tmp/pkgbad_abs
  link    a hard link member to /etc/shadow
"""
import io
import os
import sys
import tarfile


def member(tar, name, data=b"", kind=tarfile.REGTYPE, link=""):
    info = tarfile.TarInfo(name)
    info.type = kind
    info.size = len(data) if kind == tarfile.REGTYPE else 0
    info.linkname = link
    info.mode = 0o644
    info.uid, info.gid = 2, 0
    info.uname, info.gname = "bin", "operator"
    info.mtime = int(os.environ.get("SOURCE_DATE_EPOCH", 0))
    tar.addfile(info, io.BytesIO(data) if info.size else None)


def package(path, tag, name="bad", depends="", extra=()):
    control = (f"Package: {name}\nVersion: 1.0\nAbi: any\n"
               + (f"Depends: {depends}\n" if depends else "")
               + "Description: crafted for tests/pkgbad (AC3.4)\n")
    with tarfile.open(path, "w", format=tarfile.USTAR_FORMAT) as tar:
        member(tar, "CONTROL/control", control.encode())
        member(tar, f"usr/tests/bad/got_{tag}", b"installed\n")
        for args in extra:
            member(tar, *args)
        end = tar.offset
    with open(path, "r+b") as f:        # two zero blocks end it, no padding
        f.truncate(end + 1024)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    longname = "usr/" + "x" * 150 + "/" + "y" * 95
    assert len(longname) == 250
    package(os.path.join(out, "dep.tar"), "dep", "baddep",
            depends="a" * 255)
    package(os.path.join(out, "long.tar"), "long", "badlong",
            extra=[(longname, b"long\n")])
    package(os.path.join(out, "dotdot.tar"), "dotdot", "baddotdot",
            extra=[("../pkgbad_dotdot", b"out\n")])
    package(os.path.join(out, "name.tar"), "name", "../x")
    package(os.path.join(out, "abs.tar"), "abs", "badabs",
            extra=[("/tmp/pkgbad_abs", b"abs\n")])
    package(os.path.join(out, "link.tar"), "link", "badlink",
            extra=[("usr/tests/bad/shadow", b"", tarfile.LNKTYPE,
                    "/etc/shadow")])
    for t in ("dep", "long", "dotdot", "name", "abs", "link"):
        with tarfile.open(os.path.join(out, t + ".tar")) as tar:
            names = tar.getnames()
        assert names, t


if __name__ == "__main__":
    main()
