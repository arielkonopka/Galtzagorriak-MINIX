#!/usr/bin/env python3
"""Check a Galtzagorriak update file the way the Flipper does (spec RS.8
step 1-2, RS.13, RS.17).  It reads the format on its own (it does not share
code with mkupdate.py), so a mistake in one shows up in the other.

Usage:
  gzupdate-check.py --key PUB [--key PUB2] [--board flipper-f7]
                    [--epoch N] [--release X.Y.Z] [--downgrade] [-v] FILE

--key       a trusted public key: a file with 64 hex digits (as
            flipper/keys/release.pub and ~/.config/galtzagorriak/update.pub),
            or the hex itself; at most 2, like atea's two slots (RS.6)
--epoch     the running system's epoch: the file's must be higher, unless
            --downgrade (the press in atea, RS.13)
--release   the running release: the file's from_min must not be newer

Exit 0 and a summary if the file is good; otherwise exit 1 and one line
"gzupdate-check: <why>" (the same reasons `gzupdate check` gives)."""
import argparse
import hashlib
import os
import re
import struct
import sys

try:
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives.asymmetric.ed25519 import \
        Ed25519PublicKey
except ImportError:
    sys.exit("gzupdate-check: needs the Python package 'cryptography'")

SECTOR = 512
HDR_SIGNED = 448
NSLOTS = 16
DATA_START = 4 * SECTOR
CHUNK = 4096
NOTES_MAX = 4096
TYPES = {1: "flash", 2: "root", 3: "pkg", 4: "notes"}   # 5 radio: not in v1
HDR = struct.Struct("<8sHHI16s4s4sIIIIQ8sII64s32s")


class Bad(Exception):
    pass


def vers(b):
    return f"{b[0]}.{b[1]}.{b[2]}"


def cstr(b, what):
    s = b.split(b"\0", 1)
    if len(s) == 2 and s[1].strip(b"\0"):
        raise Bad(f"bad header: {what} not NUL-padded")
    if not re.fullmatch(rb"[\x21-\x7e]*", s[0]):
        raise Bad(f"bad header: {what} not printable")
    return s[0].decode()


def load_pub(arg):
    t = open(arg).read().strip() if os.path.exists(arg) else arg.strip()
    if not re.fullmatch(r"[0-9a-fA-F]{64}", t):
        raise SystemExit(f"gzupdate-check: {arg}: not a public key "
                         "(64 hex digits)")
    return bytes.fromhex(t)


def check(path, keys, board, epoch, release, downgrade):
    size = os.path.getsize(path)
    f = open(path, "rb")
    if size < DATA_START:
        raise Bad("truncated file")
    sec0 = f.read(SECTOR)
    (magic, fmt, hsize, flags, bd, rel, from_min, ep, abi, libc_id, furi_id,
     btime, kid, nsect, fsize, tabhash, text) = HDR.unpack_from(sec0)
    if magic != b"GZUPDATE":
        raise Bad("not an update file")
    if fmt != 1:
        raise Bad(f"format {fmt} not supported (this checker knows 1)")
    if hsize != HDR_SIGNED or flags:
        raise Bad("bad header")
    if any(sec0[HDR.size:HDR_SIGNED]) or rel[3] or from_min[3]:
        raise Bad("bad header: reserved bytes not zero")
    bd = cstr(bd, "board")
    text = cstr(text, "release text")
    # The key id must name a trusted key; then the signature.
    pub = next((k for k in keys if hashlib.sha512(k).digest()[:8] == kid),
               None)
    if pub is None:
        raise Bad("not signed by a trusted key")
    try:
        Ed25519PublicKey.from_public_bytes(pub).verify(sec0[HDR_SIGNED:],
                                                       sec0[:HDR_SIGNED])
    except InvalidSignature:
        raise Bad("bad signature")
    # From here on the header is the signer's.
    if board and bd != board:
        raise Bad(f"for another board ({bd}, this is {board})")
    if fsize != size:
        raise Bad("truncated file" if size < fsize else
                  "file longer than its header says")
    if fsize % SECTOR:
        raise Bad("bad header: size not sector aligned")
    tab = f.read(3 * SECTOR)
    if hashlib.sha512(tab).digest() != tabhash:
        raise Bad("section table damaged (hash mismatch)")
    if not 1 <= nsect <= NSLOTS:
        raise Bad("bad section table: section count")
    secs, end, seen = [], DATA_START, {}
    for i in range(NSLOTS):
        ent = tab[i * 32:i * 32 + 32]
        full = tab[SECTOR + i * 64:SECTOR + i * 64 + 64]
        if i >= nsect:
            if any(ent) or any(full):
                raise Bad("bad section table: unused entry not zero")
            continue
        typ, sflags, idx, off, ln, load, pre = struct.unpack("<BBHIII16s",
                                                             ent)
        if typ not in TYPES or sflags or idx != i:
            raise Bad(f"bad section table: entry {i}")
        if off % SECTOR or off < end or ln == 0 or off + ln > fsize:
            raise Bad(f"bad section table: entry {i} offset/length")
        if pre != full[:16]:
            raise Bad(f"bad section table: entry {i} hash prefix")
        if typ != 1 and load:
            raise Bad(f"bad section table: entry {i} load address")
        if typ == 1 and load % 2048:
            raise Bad("bad section table: flash section not page aligned")
        if typ == 4 and ln > NOTES_MAX:
            raise Bad("notes over 4 KB")
        seen[typ] = seen.get(typ, 0) + 1
        secs.append((i, typ, off, ln, load, full))
        end = off + ln + (-ln % SECTOR)
    if end != fsize:
        raise Bad("bad section table: does not end at the file's end")
    if seen.get(1) != 1 or seen.get(2) != 1 or seen.get(4, 0) > 1:
        raise Bad("bad section table: needs one flash and one root section")
    # Every section's SHA-512, streamed; the padding must be zero (it is
    # covered by no hash, so a flipped bit there is caught here).
    pos = DATA_START
    for i, typ, off, ln, load, full in secs:
        f.seek(pos)
        if any(f.read(off - pos)):
            raise Bad("padding not zero")
        h, left = hashlib.sha512(), ln
        while left:
            b = f.read(min(CHUNK, left))
            if not b:
                raise Bad("truncated file")
            h.update(b)
            left -= len(b)
        if h.digest() != full:
            raise Bad(f"section {i} ({TYPES[typ]}) damaged (hash mismatch)")
        pos = off + ln
    f.seek(pos)
    if any(f.read()):
        raise Bad("padding not zero")
    # Release rules (RS.8 step 2, RS.13).
    if epoch is not None and ep <= epoch and not downgrade:
        raise Bad(f"older than the running system (epoch {ep}, running "
                  f"{epoch})" if ep < epoch else "already installed")
    if release is not None:
        r = tuple(int(x) for x in release.split("."))
        if tuple(from_min[:3]) > r:
            raise Bad(f"needs at least {vers(from_min)} (running {release})")
    return dict(board=bd, release=text, epoch=ep, abi=abi, from_min=from_min,
                libc=libc_id, furi=furi_id, time=btime, kid=kid, size=fsize,
                secs=secs, f=f)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--key", action="append", default=[])
    ap.add_argument("--board", default="flipper-f7")
    ap.add_argument("--epoch", type=int)
    ap.add_argument("--release")
    ap.add_argument("--downgrade", action="store_true")
    ap.add_argument("-v", action="store_true", help="list the sections")
    ap.add_argument("file")
    a = ap.parse_args()
    if not a.key:
        sys.exit("gzupdate-check: no trusted key (--key)")
    if len(a.key) > 2:
        sys.exit("gzupdate-check: at most 2 keys (atea has 2 slots)")
    if a.release and not re.fullmatch(r"\d+\.\d+\.\d+", a.release):
        sys.exit("gzupdate-check: --release is X.Y.Z")
    keys = [load_pub(k) for k in a.key]
    try:
        r = check(a.file, keys, a.board, a.epoch, a.release, a.downgrade)
    except Bad as e:
        print(f"gzupdate-check: {e}")
        sys.exit(1)
    except OSError as e:
        print(f"gzupdate-check: {e.strerror}: {a.file}")
        sys.exit(1)
    print(f"gzupdate-check: {a.file}: good, {r['release']} for {r['board']},"
          f" epoch {r['epoch']}, ABI {r['abi']}, from {vers(r['from_min'])},"
          f" key id {r['kid'].hex()}, {len(r['secs'])} sections")
    if a.v:
        print(f"  libc id {r['libc']:#010x}  libfuri id {r['furi']:#010x}  "
              f"build time {r['time']}")
        for i, typ, off, ln, load, full in r["secs"]:
            at = f" at {load:#010x}" if typ == 1 else ""
            print(f"  {i:2} {TYPES[typ]:5} {ln:8} B @ {off:#09x}{at}  "
                  f"sha512 {full[:8].hex()}...")
        notes = [s for s in r["secs"] if s[1] == 4]
        if notes:
            r["f"].seek(notes[0][2])
            txt = r["f"].read(notes[0][3]).decode("utf-8", "replace")
            print("  notes: " + txt.split("\n", 1)[0])


if __name__ == "__main__":
    main()
