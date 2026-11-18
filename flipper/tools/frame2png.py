#!/usr/bin/env python3
"""frame2png - turn the FRAME lines of a QEMU log (m2test's launcher test,
or any 1024-byte /dev/lcd frame printed as hex) into PNG pictures.

Usage: frame2png.py LOG OUTDIR [SCALE]

Each line "FRAME name <2048 hex digits>" becomes OUTDIR/name.png, the
128x64 LCD (ST7567 page layout: byte = 8 vertical pixels) enlarged
SCALE times (default 4), dark pixels on the Flipper's orange.
"""
import os
import sys

from PIL import Image


def main():
    log, out = sys.argv[1], sys.argv[2]
    k = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    os.makedirs(out, exist_ok=True)
    for line in open(log, errors="replace"):
        p = line.split()
        if len(p) != 3 or p[0] != "FRAME" or len(p[2]) != 2048:
            continue
        fb = bytes.fromhex(p[2])
        img = Image.new("RGB", (128, 64), (255, 140, 30))
        for y in range(64):
            for x in range(128):
                if fb[(y >> 3) * 128 + x] >> (y & 7) & 1:
                    img.putpixel((x, y), (30, 30, 30))
        img.resize((128 * k, 64 * k), Image.NEAREST).save(
            os.path.join(out, p[1] + ".png"))
        print(os.path.join(out, p[1] + ".png"))


if __name__ == "__main__":
    main()
