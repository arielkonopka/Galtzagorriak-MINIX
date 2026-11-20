#!/usr/bin/env python3
"""ACS.1 on the host (spec S.10), Python side: build a small update file
with a throw-away key, check that gzupdate-check.py accepts it, and that it
rejects every bad file: one flipped bit at every byte of the file, a wrong
key, an unsigned file, truncated files, a table/section hash mismatch, a
wrong board, a lower or equal epoch, a from_min above the running release.

The good and bad files and cases.txt (file, running epoch, running release,
expected first word of the answer) stay in DIR for the C verifier's test
(T33a part 2).  Nothing here is a secret: the key is made for the run and
thrown away (only its public half is written to DIR).

Usage: updatetest.py [-o DIR]"""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import struct
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
SECTOR = 512


def tool(name):
    return os.path.join(TOOLS, name)


def run(*args, env=None):
    r = subprocess.run([sys.executable, "-I", *args], capture_output=True,
                       text=True, env=env)
    if r.returncode:
        sys.exit(f"updatetest: {' '.join(args)} failed:\n{r.stdout}{r.stderr}")
    return r.stdout


def blob(seed, n):
    out, i = bytearray(), 0
    while len(out) < n:
        out += hashlib.sha512(f"{seed}{i}".encode()).digest()
        i += 1
    return bytes(out[:n])


spec = importlib.util.spec_from_file_location("gzcheck",
                                              tool("gzupdate-check.py"))
gzcheck = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gzcheck)


def verdict(path, keys, epoch=None, release=None, board="flipper-f7"):
    """'good' or the reason the checker gives."""
    try:
        r = gzcheck.check(path, keys, board, epoch, release, False)
        r["f"].close()
        return "good"
    except gzcheck.Bad as e:
        return str(e)


def sigvectors(out):
    """sigs.txt for the C verifier: Ed25519 signatures made by the
    `cryptography` package (another implementation) over messages of 0-300
    bytes, each followed by bad variants: a flipped bit in R, in S, in the
    message, in the key, and S + L (the same point, refused by RFC 8032)."""
    from cryptography.hazmat.primitives.asymmetric.ed25519 import \
        Ed25519PrivateKey
    from cryptography.hazmat.primitives import serialization as ser
    L = 2**252 + 27742317777372353535851937790883648493
    with open(os.path.join(out, "sigs.txt"), "w") as f:
        f.write("# good|bad public-key message signature (hex; '-' = empty"
                " message; written by tools/updatetest.py)\n")
        for i in range(64):
            k = Ed25519PrivateKey.from_private_bytes(blob(f"k{i}", 32))
            pk = k.public_key().public_bytes(ser.Encoding.Raw,
                                             ser.PublicFormat.Raw)
            m = blob(f"m{i}", (i * 37) % 301)
            sig = k.sign(m)

            def line(tag, pk, m, sig):
                f.write(f"{tag} {pk.hex()} {m.hex() or '-'} {sig.hex()}\n")

            def fl(b, j):
                b = bytearray(b)
                b[j % len(b)] ^= 1 << (j % 8)
                return bytes(b)
            line("good", pk, m, sig)
            line("bad", pk, m, fl(sig, i % 32))
            line("bad", pk, m, fl(sig, 32 + i % 31))
            if m:
                line("bad", pk, fl(m, i), sig)
            line("bad", fl(pk, i), m, sig)
            s = int.from_bytes(sig[32:], "little") + L
            if s < 2**256:
                line("bad", pk, m, sig[:32] + s.to_bytes(32, "little"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out")
    a = ap.parse_args()
    tmp = tempfile.TemporaryDirectory()
    out = a.out or os.path.join(tmp.name, "out")
    os.makedirs(out, exist_ok=True)
    w = tmp.name

    def put(name, data, mode="wb"):
        p = os.path.join(w, name)
        with open(p, mode) as f:
            f.write(data)
        return p

    def version_h(epoch):
        return put(f"gzversion{epoch}.h",
                   f'#define GZ_RELEASE\t"0.9.0"\n#define GZ_EPOCH\t{epoch}\n'
                   f'#define GZ_ABI\t\t3\n', "w")

    put("CHANGELOG.md", "# Changelog\n\n## Unreleased\n\n- next\n\n"
        "## 0.9.0 (2026-12-01)\n\nalarm snooze, \"Egun on!\" on the welcome"
        " screen\n\n## 0.8.2\n\nold\n", "w")
    put("libc.json", json.dumps({"id": 0x1549C46A}), "w")
    put("furi.json", json.dumps({"id": 0x5FFE7B3F}), "w")
    flash = put("flash.bin", blob("flash", 4 * 2048 + 100))
    root = put("base.tar", blob("root", 3000))
    pkgs = [put(f"p{i}.tar", blob(f"pkg{i}", n))
            for i, n in enumerate((700, 1300))]

    kdir = os.path.join(w, "key")
    run(tool("gzsign"), "--new-key", kdir)
    kdir2 = os.path.join(w, "key2")
    run(tool("gzsign"), "--new-key", kdir2)
    pub = bytes.fromhex(open(os.path.join(kdir, "update.pub")).read())
    pub2 = bytes.fromhex(open(os.path.join(kdir2, "update.pub")).read())
    with open(os.path.join(out, "trusted.pub"), "w") as f:
        f.write(pub.hex() + "\n")

    def build(name, epoch=7, board="flipper-f7", from_min="0.8.0",
              key=kdir, sign=True):
        p = os.path.join(out, name)
        run(tool("mkupdate.py"), "-o", p, "--version-h", version_h(epoch),
            "--changelog", os.path.join(w, "CHANGELOG.md"),
            "--libc", os.path.join(w, "libc.json"),
            "--furi", os.path.join(w, "furi.json"), "--board", board,
            "--flash", flash, "--load", "0x08004000", "--root", root,
            "--from-min", from_min, *sum((["--pkg", q] for q in pkgs), []))
        if sign:
            run(tool("gzsign"), "--key", os.path.join(key, "update.key"), p)
        return p

    cases = []          # (file, epoch, release, expected verdict prefix)
    fails = 0

    def expect(path, want, epoch=6, release="0.8.2"):
        nonlocal fails
        got = verdict(path, [pub], epoch, release)
        ok = got == want if want == "good" else got != "good" and \
            got.startswith(want)
        if not ok:
            fails += 1
            print(f"FAIL {os.path.basename(path)}: want {want!r}, got {got!r}")
        cases.append((os.path.basename(path), epoch, release,
                      "good" if want == "good" else "bad"))

    good = build("good.update")
    expect(good, "good")
    data = open(good, "rb").read()
    # The output is reproducible, and the notes are the release's section.
    again = build("again.update")
    if open(again, "rb").read() != data:
        fails += 1
        print("FAIL: two builds of the same input differ")
    os.unlink(again)
    if b"Egun on!" not in data or b"## 0.8.2" in data:
        fails += 1
        print("FAIL: notes are not the 0.9.0 section")

    # Signed with the second key: trusted only if that key is given.
    expect(build("wrongkey.update", key=kdir2), "not signed by a trusted key")
    if verdict(os.path.join(out, "wrongkey.update"), [pub, pub2], 6) != "good":
        fails += 1
        print("FAIL: second trusted key (atea slot 2) not accepted")
    expect(build("unsigned.update", sign=False),
           "not signed by a trusted key")
    expect(build("wrongboard.update", board="qemu-mps2"), "for another board")
    expect(good, "older than the running system", epoch=8)
    cases[-1] = ("good.update", 8, "0.8.2", "bad")
    expect(good, "already installed", epoch=7)
    cases[-1] = ("good.update", 7, "0.8.2", "bad")
    expect(build("frommin.update", from_min="0.8.5"), "needs at least 0.8.5")

    def variant(name, b):
        p = os.path.join(out, name)
        with open(p, "wb") as f:
            f.write(b)
        return p

    # Truncated: inside the header, the table, a section, the last sector.
    for n in (100, SECTOR, 3 * SECTOR, 4 * SECTOR + 7, len(data) - SECTOR):
        expect(variant(f"trunc{n}.update", data[:n]), "truncated file")
    expect(variant("long.update", data + bytes(SECTOR)),
           "file longer than its header says")
    # A table entry changed: the header's table hash no longer matches.
    t = bytearray(data)
    t[SECTOR + 8] ^= 1
    expect(variant("tablehash.update", bytes(t)),
           "section table damaged")
    # A section changed: the table's hash for it no longer matches.
    off = struct.unpack_from("<I", data, SECTOR + 32 + 4)[0]
    t = bytearray(data)
    t[off + 5] ^= 0x80
    expect(variant("sectionhash.update", bytes(t)), "section 1 (root)")
    # A flipped bit in the signature.
    t = bytearray(data)
    t[448 + 10] ^= 4
    expect(variant("badsig.update", bytes(t)), "bad signature")

    # One flipped bit at every byte of the good file (bit = offset % 8).
    flip = os.path.join(w, "flip.update")
    buf = bytearray(data)
    accepted = []
    for i in range(len(buf)):
        buf[i] ^= 1 << (i % 8)
        with open(flip, "wb") as f:
            f.write(buf)
        if verdict(flip, [pub], 6, "0.8.2") == "good":
            accepted.append(i)
        buf[i] ^= 1 << (i % 8)
    if accepted:
        fails += 1
        print(f"FAIL: {len(accepted)} one-bit flips accepted, first at "
              f"{accepted[0]:#x}")

    sigvectors(out)
    with open(os.path.join(out, "cases.txt"), "w") as f:
        f.write("# file running-epoch running-release expect "
                "(written by tools/updatetest.py)\n")
        for c in cases:
            f.write("%s %d %s %s\n" % c)
    total = len(cases) + len(data) + 3
    if fails:
        print(f"updatetest: {fails} of {total} checks FAILED")
        sys.exit(1)
    print(f"updatetest: {total} checks passed ({len(data)} one-bit flips, "
          f"{len(cases)} files in {out if a.out else 'a temporary dir'})")


main()
