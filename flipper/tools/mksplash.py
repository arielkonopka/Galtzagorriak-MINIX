#!/usr/bin/env python3
"""Turn a picture into the 128x64 boot screen of Galtzagorriak MINIX.

Usage: mksplash.py [IMAGE] OUT.c [--title TEXT] [--preview OUT.png]

The picture is cropped to 2:1, scaled to 128x64, contrast-stretched and
dithered (Floyd-Steinberg) to black and white; a framed title banner is
drawn along the bottom edge, like the pixel-art originals.  Without an
IMAGE only the banner is drawn (a placeholder).  The output is a C array in
the ST7567 page layout that hal_lcd_flush() takes (bit n of byte
page*128+x = pixel row page*8+n), plus an optional enlarged PNG preview.
"""
import argparse
import gzip
import io

from PIL import Image, ImageOps, PcfFontFile

W, H = 128, 64
FONT = "/usr/share/fonts/X11/misc/6x10-ISO8859-1.pcf.gz"


def load_font():
    data = gzip.open(FONT).read()
    return PcfFontFile.PcfFontFile(io.BytesIO(data))


def font4x6():
    """The LCD's 4x6 glyphs (hal/common/font4x6.c): 6 row bytes per char."""
    import os
    import re
    src = open(os.path.join(os.path.dirname(__file__),
                            "../hal/common/font4x6.c")).read()
    return [[int(v, 16) for v in m.split(",")]
            for m in re.findall(r"\{((?:0x[0-9a-f]{2},?){6})\}", src)]


def draw_text(img, font, text, x0, y0, ink):
    """Draw text with an X11 PCF bitmap font; returns the width used."""
    ascent = max(-g[1][1] for g in font.glyph if g)
    x = x0
    for ch in text:
        g = font.glyph[ord(ch)]
        if not g:
            continue
        (dx, _), (bx0, by0, _, _), _, im = g
        for yy in range(im.size[1]):
            for xx in range(im.size[0]):
                if im.getpixel((xx, yy)):
                    px, py = x + bx0 + xx, y0 + ascent + by0 + yy
                    if 0 <= px < W and 0 <= py < H:
                        img.putpixel((px, py), ink)
        x += dx
    return x - x0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("args", nargs="+", help="[IMAGE] OUT.c")
    ap.add_argument("--title", default="GALTZAGORRIAK MINIX")
    ap.add_argument("--preview")
    ap.add_argument("--crop", help="x0,y0,x1,y1 of the picture to use first")
    ap.add_argument("--threshold", type=int,
                    help="plain threshold (0-255) instead of dithering; "
                    "better for line art")
    ap.add_argument("--ready", action="store_true",
                    help="IMAGE is already 128x64: no crop, scale or banner")
    ap.add_argument("--bubble", metavar="TEXT",
                    help="with --ready: TEXT in the speech bubble (4x6 font "
                    "of the LCD, light on dark), centred in x 9..118, y 4..9")
    ap.add_argument("--bye", metavar="TEXT",
                    help="with --ready: a second picture, splash_bye_fb, "
                    "with TEXT in the bubble (shown at power off)")
    ap.add_argument("--label", action="append", default=[],
                    metavar="TEXT@X,Y,INK",
                    help="with --ready: more 4x6 text, top left at X,Y; "
                    "INK 0 dark, 1 light (repeatable)")
    ap.add_argument("--gamma", type=float, default=1.0,
                    help="<1 lightens the picture before dithering")
    a = ap.parse_args()
    src, out = (a.args + [None])[:2] if len(a.args) == 2 else (None, a.args[0])

    def ready(bubble):
        img = Image.open(src).convert("L")
        if img.size != (W, H):
            raise SystemExit("%s: need %dx%d with --ready" % (src, W, H))
        img = img.point(lambda v: 0 if v < 128 else 255)
        rows = font4x6()
        labels = list(a.label)
        if bubble:
            labels.append("%s@%d,4,1" % (bubble,
                                         9 + (110 - 4 * len(bubble)) // 2))
        for lab in labels:
            text, at = lab.rsplit("@", 1)
            x, y, ink = (int(v) for v in at.split(","))
            for ch in text:
                for gy in range(6):
                    for gx in range(4):
                        if rows[ord(ch) - 32][gy] & (0x80 >> gx):
                            img.putpixel((x + gx, y + gy), 255 if ink else 0)
                x += 4
        return img

    bye = None
    if src and a.ready:
        img = ready(a.bubble)
        if a.bye:
            bye = ready(a.bye)
    elif src:
        pic = Image.open(src).convert("L")
        if a.crop:
            pic = pic.crop(tuple(int(v) for v in a.crop.split(",")))
        w, h = pic.size
        if w / h > 2:                      # crop to 2:1 around the centre
            nw = 2 * h
            pic = pic.crop(((w - nw) // 2, 0, (w - nw) // 2 + nw, h))
        else:
            nh = w // 2
            pic = pic.crop((0, (h - nh) // 2, w, (h - nh) // 2 + nh))
        pic = pic.resize((W, H), Image.LANCZOS)
        pic = ImageOps.autocontrast(pic, cutoff=2)
        if a.gamma != 1.0:
            pic = pic.point(lambda v: int(255 * (v / 255) ** a.gamma))
        if a.threshold is not None:
            img = pic.point(lambda v: 0 if v < a.threshold else 255)
        else:
            img = pic.convert("1").convert("L")    # Floyd-Steinberg
    else:
        img = Image.new("L", (W, H), 255)

    # Title banner: white box with a black frame along the bottom edge.
    font = None if a.ready else load_font()
    tw = 0 if a.ready else sum(font.glyph[ord(c)][0][0] for c in a.title if font.glyph[ord(c)])
    bw, bh = min(W, tw + 6), 13
    bx, by = (W - bw) // 2, H - bh
    if not a.ready:
        for y in range(by, H):
            for x in range(bx, bx + bw):
                edge = y in (by, H - 1) or x in (bx, bx + bw - 1)
                img.putpixel((x, y), 0 if edge else 255)
        draw_text(img, font, a.title, bx + 3, by + 2, 0)

    # LCD: a set bit is a dark pixel.
    def pack(img):
        fb = bytearray(W * H // 8)
        for y in range(H):
            for x in range(W):
                if img.getpixel((x, y)) < 128:
                    fb[(y >> 3) * W + x] |= 1 << (y & 7)
        return fb

    with open(out, "w") as f:
        f.write("/* Generated by flipper/tools/mksplash.py - do not edit. */\n")
        for name, im in (("splash_fb", img), ("splash_bye_fb", bye)):
            if im is None:
                continue
            fb = pack(im)
            f.write("const unsigned char %s[%d] = {\n" % (name, len(fb)))
            for i in range(0, len(fb), 16):
                f.write("  " + ",".join(f"0x{b:02x}" for b in fb[i:i + 16])
                        + ",\n")
            f.write("};\n")
    if a.preview:
        prev = Image.new("RGB", (W, H))
        for y in range(H):
            for x in range(W):
                dark = img.getpixel((x, y)) < 128
                prev.putpixel((x, y), (40, 48, 30) if dark else (170, 190, 120))
        prev.resize((W * 4, H * 4), Image.NEAREST).save(a.preview)
        if bye:
            for y in range(H):
                for x in range(W):
                    dark = bye.getpixel((x, y)) < 128
                    prev.putpixel((x, y),
                                  (40, 48, 30) if dark else (170, 190, 120))
            prev.resize((W * 4, H * 4), Image.NEAREST).save(
                a.preview.replace(".png", "-bye.png"))
    print(f"  {out}: 128x64 boot screen" + (f" from {src}" if src else
                                             " (title only)"))


if __name__ == "__main__":
    main()
