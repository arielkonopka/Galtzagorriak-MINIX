#!/usr/bin/env python3
"""Build a Galtzagorriak package (a tar archive read by /bin/pkg).

Usage:
  mkpkg.py -n NAME -v VERSION [-d DEP,DEP] [-D "description"]
           [--postinst FILE] [--prerm FILE] [--timers]
           [--notify alarm|reminder|info] [--deskmode] [--flash] [-i ICON]
           [--window] [--background event|hibernate|park|stream|exit]
           -o OUTDIR
           dest=source ...

--timers, --notify, --deskmode: the app manifest fields of spec F.2
(Timers: yes = may add timers, group timers; Notify: the highest
category it may post; DeskMode: yes = may keep the backlight on while
charging).
-i ICON: Icon: line, the app's launcher icon (spec R5.6a.1): @name of a
built-in icon (e.g. @bell), # and 12 hex digits (8x6 pixels, one byte a
row, top first, MSB left), or 1-2 characters.  Until the launcher reads
manifests, the package's postinst writes it into its /etc/apps line.
--icon10 ICON10: Icon10: line, a 10x10 icon of the app's own for the
launcher's Stock style (spec R5.6b.3): a 10x10 PNG (as a .fam's fap_icon,
copied without downscaling; needs PIL) or "##" and 40 hex digits (2 bytes a
row, top first, MSB left).  The /etc/apps icon field takes the same "##"
form, optionally followed by the 8x6 icon for the Pine style.  fam2pkg
(R5.5) passes the .fam's fap_icon here.
--window: Type: window, a window app (spec K.8): the launcher starts it
with its event and record pipes on fd 3 and 4 and draws its windows; its
postinst writes "=" before the program in its /etc/apps line.
--background: Background: line, what the app does when it is not in
front (spec 12d tiers; "exit": a window app that ends on Back, spec K.8).
--flash: Flash: yes, the programs go into the installed region (internal
flash) as with pkg install --flash: their text is not copied into RAM.

Each dest=source puts the host file `source` at path `dest` on the Flipper,
e.g. /usr/bin/hello=build/flipper/minix/bin/hello (mode 755 if the path is
in a bin directory, else 644; ",MODE" after the source sets it, e.g. ,4755
for a set-uid program, which is then owned by root; ,2755:GID for a set-gid
program owned by bin and group GID).  Writes OUTDIR/NAME_VERSION.tar in the old
POSIX ustar format that MINIX 2.0 tar understands (names < 100 chars).
"""
import argparse
import sys
import struct
import io
import os
import tarfile
import time


def add(tar, name, data, mode, gid=None):
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = mode
    if gid is not None:                  # set-gid to a group: owner bin
        info.uid, info.gid = 2, gid
        info.uname, info.gname = "bin", ""
    elif mode & 0o6000:                  # set-uid/gid: owned by root
        info.uid, info.gid = 0, 0
        info.uname, info.gname = "root", "operator"
    else:
        info.uid, info.gid = 2, 0        # bin, operator: like /bin
        info.uname, info.gname = "bin", "operator"
    info.mtime = int(os.environ.get("SOURCE_DATE_EPOCH", time.time()))
    if len(name) >= 100:
        raise SystemExit(f"path too long for MINIX tar: {name}")
    tar.addfile(info, io.BytesIO(data))


def icon10_field(v):
    """The Icon10: value, "##" and 40 hex digits, from that form or a
    10x10 PNG (dark, opaque pixels are set; no downscaling)."""
    if v.startswith("##"):
        if len(v) != 42 or any(c not in "0123456789abcdefABCDEF" for c in v[2:]):
            sys.exit("mkpkg: --icon10: ## and 40 hex digits")
        return v.upper()
    from PIL import Image
    im = Image.open(v).convert("RGBA")
    if im.size != (10, 10):
        sys.exit(f"mkpkg: --icon10: {v} is {im.size[0]}x{im.size[1]}, not 10x10")
    out = []
    for y in range(10):
        bits = 0
        for x in range(10):
            r, g, b, al = im.getpixel((x, y))
            if al > 127 and r + g + b < 384:
                bits |= 0x8000 >> x
        out.append(f"{bits:04X}")
    return "##" + "".join(out)


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
    ap.add_argument("--timers", action="store_true", help="Timers: yes")
    ap.add_argument("--notify", choices=("alarm", "reminder", "info"))
    ap.add_argument("--deskmode", action="store_true", help="DeskMode: yes")
    ap.add_argument("--flash", action="store_true", help="Flash: yes")
    ap.add_argument("-i", "--icon", help="Icon: the launcher icon (R5.6a.1)")
    ap.add_argument("--icon10", help="Icon10: 10x10 PNG or ##+40 hex (R5.6b.3)")
    ap.add_argument("--window", action="store_true", help="Type: window (K.8)")
    ap.add_argument("--background", choices=("event", "hibernate", "park",
                                             "stream", "exit"))
    ap.add_argument("files", nargs="+", metavar="dest=source")
    a = ap.parse_args()

    # Abi: the ABI of its programs (MINIX a.out, a_version; 0 = 1), or
    # "any" for a package without programs (spec J.3).
    # Libc: the build id of the shared libc its programs were linked with
    # (a.out flag A_LIBC, a_tbase; spec R2b.9): pkg refuses another one.
    # Furi: the same for the shared libfuri + libapp (a_dbase, 0: unused).
    # Secret: yes when a program is marked A_NOSWAP (elf2mnx --secret): in
    # plain swap mode its RAM never goes to the card (spec R12.28).
    abis, libcs, furis, secret = set(), set(), set(), False
    for spec in a.files:
        with open(spec.split("=", 1)[1].rsplit(",", 1)[0], "rb") as f:
            h = f.read(48)
        if len(h) >= 8 and h[0:2] == b"\x01\x03":
            abis.add(struct.unpack_from("<H", h, 6)[0] or 1)
            secret = secret or bool(h[2] & 0x80)
            if h[2] & 0x08 and h[4] >= 48 and len(h) == 48:
                libcs.add(struct.unpack_from("<I", h, 40)[0])
                if struct.unpack_from("<I", h, 44)[0]:
                    furis.add(struct.unpack_from("<I", h, 44)[0])
    if len(abis) > 1:
        sys.exit(f"mkpkg: programs for different ABIs: {sorted(abis)}")
    if len(libcs) > 1:
        sys.exit("mkpkg: programs for different shared libc builds: "
                 + " ".join(f"{x:08x}" for x in sorted(libcs)))
    if len(furis) > 1:
        sys.exit("mkpkg: programs for different shared libfuri builds: "
                 + " ".join(f"{x:08x}" for x in sorted(furis)))
    icon10 = icon10_field(a.icon10) if a.icon10 else None
    control = (f"Package: {a.name}\nVersion: {a.version}\n"
               + (f"Depends: {a.depends}\n" if a.depends else "")
               + f"Abi: {abis.pop() if abis else 'any'}\n"
               + (f"Libc: {libcs.pop():08x}\n" if libcs else "")
               + (f"Furi: {furis.pop():08x}\n" if furis else "")
               + (f"Os: {a.os}\n" if a.os else "")
               + ("Secret: yes\n" if secret else "")
               + ("Timers: yes\n" if a.timers else "")
               + (f"Notify: {a.notify}\n" if a.notify else "")
               + ("DeskMode: yes\n" if a.deskmode else "")
               + ("Flash: yes\n" if a.flash else "")
               + (f"Icon: {a.icon}\n" if a.icon else "")
               + (f"Icon10: {icon10}\n" if icon10 else "")
               + ("Type: window\n" if a.window else "")
               + (f"Background: {a.background}\n" if a.background else "")
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
            gid = None
            if "," in src:
                src, m = src.rsplit(",", 1)
                if ":" in m:             # ,2755:13: set-gid group 13
                    m, g = m.split(":")
                    gid = int(g)
                mode = int(m, 8)
            with open(src, "rb") as f:
                add(tar, dest, f.read(), mode, gid)
    print(f"  {out}: {os.path.getsize(out)} bytes")


if __name__ == "__main__":
    main()
