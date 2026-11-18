#!/usr/bin/env python3
"""iconpng - the launcher's built-in icons (flipper/gui/appline.c, spec
R5.6a.2), enlarged into one PNG for review.

Usage: iconpng.py OUT.png [SCALE]

Each 8x6 icon is drawn SCALE times larger (default 8), four to a row with
its name under it, dark pixels on the Flipper's orange.  Needs Pillow.
"""
import os
import re
import sys

from PIL import Image, ImageDraw

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "gui", "appline.c")


def main():
    out = sys.argv[1]
    k = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    icons = [(m.group(1), [int(x, 16) for x in m.group(2).split(",")])
             for m in re.finditer(r'\{ "(\w+)", \{ ([^}]*) \} \}',
                                  open(SRC).read())]
    cw, ch, per = 8 * k + 4 * k, 6 * k + 3 * k, 4
    rows = (len(icons) + per - 1) // per
    img = Image.new("RGB", (cw * per, ch * rows), (255, 140, 30))
    d = ImageDraw.Draw(img)
    for i, (name, bits) in enumerate(icons):
        x0, y0 = (i % per) * cw + 2 * k, (i // per) * ch + k
        for y, b in enumerate(bits):
            for x in range(8):
                if b & (0x80 >> x):
                    d.rectangle([x0 + x * k, y0 + y * k,
                                 x0 + x * k + k - 1, y0 + y * k + k - 1],
                                fill=(30, 30, 30))
        d.text((x0, y0 + 6 * k + 2), name, fill=(30, 30, 30))
    img.save(out)
    print(f"{out}: {len(icons)} icons")


if __name__ == "__main__":
    main()
