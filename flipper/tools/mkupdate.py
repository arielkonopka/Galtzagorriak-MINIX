#!/usr/bin/env python3
"""Build an unsigned Galtzagorriak update file (spec section S, RS.1-RS.3,
RS.17).  tools/gzsign signs it, tools/gzupdate-check.py checks it the way
the Flipper does; `make BOARD=flipper update` runs all three.

Usage:
  mkupdate.py -o OUT --version-h gzversion.h --changelog CHANGELOG.md
              --libc libc.json --furi furi.json --board flipper-f7
              --flash minix-base.bin --load 0x08000000
              --root base_X.Y.tar [--pkg app_X.Y.tar ...]
              [--gzbuild gzbuild.h] [--from-min X.Y.Z]

Format version 1 (documented in flipper/PORTING.md, "The update file"),
all numbers little endian, everything sector aligned (512 B):

  sector 0    header: 448 signed bytes, then the Ed25519 signature (64 B;
              zero here, gzsign fills it in together with the key id)
  sector 1    section table: 16 entries of 32 B
  sector 2-3  full SHA-512 of each section, 16 x 64 B
  sector 4..  the sections, each starting on a sector, in table order

The header holds the SHA-512 of sectors 1-3; those hold the SHA-512 of
every section.  So one signature check covers the whole file.
The output is the same for the same inputs (no time stamps of its own)."""
import argparse
import hashlib
import json
import re
import struct
import sys

SECTOR = 512
MAGIC = b"GZUPDATE"
FORMAT = 1
HDR_SIGNED = 448
NSLOTS = 16
TABLE_SECTORS = 3              # table + 2 sectors of full hashes
DATA_START = (1 + TABLE_SECTORS) * SECTOR
NOTES_MAX = 4096

T_FLASH, T_ROOT, T_PKG, T_NOTES, T_RADIO = 1, 2, 3, 4, 5


def die(msg):
    print(f"mkupdate: {msg}", file=sys.stderr)
    sys.exit(1)


def version(s, what):
    m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", s)
    if not m or any(int(x) > 255 for x in m.groups()):
        die(f"{what} {s!r} is not X.Y.Z (each at most 255)")
    return bytes(int(x) for x in m.groups()) + b"\0"


def define(h, name, required=True):
    m = re.search(rf"#define\s+{name}\s+(\S+)", h)
    if not m:
        if required:
            die(f"no {name} in the version header")
        return None
    return m.group(1).strip('"').rstrip("UuLl")


def notes_of(changelog, release, build_id):
    """The CHANGELOG.md section of this release (Unreleased for a build
    after the tag), cut to 4 KB on a line end."""
    text = open(changelog, encoding="utf-8").read()
    parts = re.split(r"^## ", text, flags=re.M)[1:]
    def first(name):
        return next((p for p in parts if p.split()[0] == name), None)
    sec = first(release) if build_id == "" else None
    if sec is None:
        sec = first("Unreleased") or ""
    sec = "## " + sec.strip() + "\n" if sec else f"## {release}\n"
    raw = sec.encode("utf-8")
    if len(raw) > NOTES_MAX:
        tail = "\n[more in CHANGELOG.md]\n".encode()
        raw = raw[:NOTES_MAX - len(tail)]
        raw = raw[:raw.rfind(b"\n") + 1] + tail
    return raw


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--version-h", required=True)
    ap.add_argument("--changelog", required=True)
    ap.add_argument("--libc", required=True, help="shlibc libc.json")
    ap.add_argument("--furi", required=True, help="shfuri furi.json")
    ap.add_argument("--board", required=True)
    ap.add_argument("--flash", required=True, help="minix-base.bin")
    ap.add_argument("--load", required=True, type=lambda s: int(s, 0))
    ap.add_argument("--root", required=True, help="base package tar")
    ap.add_argument("--pkg", action="append", default=[])
    ap.add_argument("--from-min", default="0.0.0")
    ap.add_argument("--gzbuild", help="gzbuild.h: build id and time")
    a = ap.parse_args()
    a.build_id, a.build_time = "", 0
    if a.gzbuild:
        g = open(a.gzbuild).read()
        a.build_id = define(g, "GZ_BUILD").strip('"')
        a.build_time = int(define(g, "GZ_BUILD_EPOCH"), 0)

    h = open(a.version_h).read()
    release = define(h, "GZ_RELEASE")
    epoch = int(define(h, "GZ_EPOCH"), 0)
    abi = int(define(h, "GZ_ABI"), 0)
    libc_id = json.load(open(a.libc))["id"]
    furi_id = json.load(open(a.furi))["id"]
    board = a.board.encode()
    if not 0 < len(board) < 16:
        die("board name must be 1-15 characters")
    fulltext = (release + a.build_id).encode()
    if len(fulltext) > 31:
        die(f"release text {fulltext!r} is over 31 characters")
    rel_b = version(release, "release")
    from_b = version(a.from_min, "--from-min")
    if from_b[:3] > rel_b[:3]:
        die(f"--from-min {a.from_min} is newer than the release {release}")
    if not 0 < epoch < 1 << 32:
        die(f"GZ_EPOCH {epoch} out of range")

    secs = [(T_FLASH, a.load, open(a.flash, "rb").read()),
            (T_ROOT, 0, open(a.root, "rb").read()),
            (T_NOTES, 0, notes_of(a.changelog, release, a.build_id))]
    secs += [(T_PKG, 0, open(p, "rb").read()) for p in a.pkg]
    if len(secs) > NSLOTS:
        die(f"{len(secs)} sections, at most {NSLOTS}")
    if a.load % 2048:
        die("the flash section must start on a flash page (2 KB)")

    table = bytearray(NSLOTS * 32)
    hashes = bytearray(NSLOTS * 64)
    body = bytearray()
    off = DATA_START
    for i, (typ, load, data) in enumerate(secs):
        if not data:
            die(f"section {i} (type {typ}) is empty")
        d = hashlib.sha512(data).digest()
        struct.pack_into("<BBHIII16s", table, i * 32, typ, 0, i, off,
                         len(data), load, d[:16])
        hashes[i * 64:i * 64 + 64] = d
        pad = -len(data) % SECTOR
        body += data + bytes(pad)
        off += len(data) + pad
    if off >= 1 << 32:
        die("file over 4 GB")
    tab = bytes(table) + bytes(hashes)
    assert len(tab) == TABLE_SECTORS * SECTOR

    hdr = struct.pack("<8sHHI16s4s4sIIIIQ8sII64s32s",
                      MAGIC, FORMAT, HDR_SIGNED, 0, board, rel_b, from_b,
                      epoch, abi, libc_id, furi_id, a.build_time,
                      bytes(8), len(secs), off,
                      hashlib.sha512(tab).digest(), fulltext)
    hdr += bytes(HDR_SIGNED - len(hdr)) + bytes(64)
    assert len(hdr) == SECTOR
    out = hdr + tab + bytes(body)
    assert len(out) == off
    with open(a.out, "wb") as f:
        f.write(out)
    print(f"  {a.out}: {len(out)} bytes, {len(secs)} sections, "
          f"{release}{a.build_id} epoch {epoch} (unsigned)")


main()
