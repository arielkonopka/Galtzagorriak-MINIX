#!/usr/bin/env python3
"""lfrawdump - show an LF RFID RAW file (.ask.raw / .psk.raw) as Unleashed and
`rfid raw` (spec R9.0.8) write it: the header, then (high, period) pairs in
microseconds.

Usage: lfrawdump.py FILE [-n PAIRS]     (default: the first 40 pairs)

Format (Unleashed lib/lfrfid/lfrfid_raw_file.c): magic 0x4C464952, version 1,
frequency and duty cycle as little-endian floats, the largest block; then
blocks: a 32-bit length and that many bytes of LEB128 varint pairs.
"""
import struct
import sys


def varints(data):
    v = shift = 0
    for b in data:
        v |= (b & 0x7F) << shift
        if b & 0x80:
            shift += 7
        else:
            yield v
            v = shift = 0


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip())
        return 2
    show = 40
    if len(sys.argv) == 4 and sys.argv[2] == "-n":
        show = int(sys.argv[3])
    with open(sys.argv[1], "rb") as f:
        raw = f.read()
    magic, version, freq, duty, maxblk = struct.unpack_from("<IIffI", raw, 0)
    if magic != 0x4C464952 or version != 1:
        print("not an LF RFID RAW file (magic %08x, version %d)" % (magic, version))
        return 1
    print("carrier %.0f Hz, duty %.2f, blocks up to %d bytes" % (freq, duty, maxblk))
    pos, pairs, blocks, bad = 20, [], 0, 0
    while pos + 4 <= len(raw):
        (n,) = struct.unpack_from("<I", raw, pos)
        pos += 4
        if n > maxblk or pos + n > len(raw):
            print("bad block length %d at %d" % (n, pos - 4))
            return 1
        vals = list(varints(raw[pos:pos + n]))
        pos += n
        blocks += 1
        if len(vals) % 2:
            bad += 1
        pairs += list(zip(vals[0::2], vals[1::2]))
    print("%d blocks, %d pairs%s" % (blocks, len(pairs),
          ", %d block(s) with an odd count" % bad if bad else ""))
    if pairs:
        hi = [p for p, _ in pairs]
        per = [q for _, q in pairs]
        print("high %d-%d us, period %d-%d us" % (min(hi), max(hi), min(per), max(per)))
        print("bad pairs (high > period): %d" % sum(1 for p, q in pairs if p > q))
    for p, q in pairs[:show]:
        print("%6d %6d" % (p, q))
    return 0


if __name__ == "__main__":
    sys.exit(main())
