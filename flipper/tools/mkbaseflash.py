#!/usr/bin/env python3
"""Build programs into the installed region of a flash image (spec Phase 2b,
R2b.6, make base-flash).

Usage: mkbaseflash.py system.bin out.bin FLASH_BASE REGION_OFFSET prog.aout ...

out.bin is system.bin padded to REGION_OFFSET (the installed region, which
starts right after the 192 KB system image on every layout), then the
region in the format of src/mm/finst.c:
    page 0   directory "GINB" (made on the PC), generation 1, one 64-byte
             record per program: SHA-256 of the text, address, size, the
             binding left erased (ones), seq, flags, dead marker erased;
    page 1   erased (ones);
    page 2.. the texts, each from a page boundary.
At boot /etc/rc runs "pkg bind /bin/*": MM gives each record to the file
whose text has the same SHA-256 and writes the binding, after moving the
directory to a page it erased itself (DFU has programmed the free double
words of page 0 with ones, which must not be programmed twice).

The image is only flashed with DFU, which needs physical access, so the
records are not signed.  Programs that do not fit the region on a given
Flipper (it ends 16 pages below CPU2's flash, at 0x080A7000 on most) are
ignored there: MM finds no binding for them and they run from the cache.
"""
import hashlib
import struct
import sys

PAGE = 4096
HDR = 8
REC = 64
NREC = (PAGE - HDR) // REC
SEED = 0x424E4947          # "GINB"
A_SEP = 0x20

system, out, flash, region, *progs = sys.argv[1:]
flash, region = int(flash, 0), int(region, 0)
image = bytearray(open(system, "rb").read())
if len(image) > region:
    sys.exit(f"mkbaseflash: {system} is {len(image)} bytes, more than "
             f"{region:#x}")
if len(progs) > NREC:
    sys.exit(f"mkbaseflash: at most {NREC} programs")
image += b"\xff" * (region - len(image))

directory = bytearray(b"\xff" * PAGE)
struct.pack_into("<II", directory, 0, SEED, 1)
texts = bytearray()
addr = flash + region + 2 * PAGE
for seq, path in enumerate(progs):
    data = open(path, "rb").read()
    if data[0:2] != b"\x01\x03" or not data[2] & A_SEP:
        sys.exit(f"mkbaseflash: {path}: not a separate I&D MINIX program")
    hdrlen = data[4]
    a_text = struct.unpack_from("<I", data, 8)[0]
    text = data[hdrlen:hdrlen + a_text]
    rec = hashlib.sha256(text).digest() + struct.pack(
        "<IIIIII", addr, a_text, 0xFFFFFFFF, 0xFFFFFFFF, seq, 0xFFFFFFFF)
    directory[HDR + seq * REC:HDR + seq * REC + len(rec)] = rec
    pages = (a_text + PAGE - 1) // PAGE
    texts += text + b"\xff" * (pages * PAGE - a_text)
    print(f"  {path.rsplit('/', 1)[-1]:10s} at {addr:#010x}, {a_text} bytes")
    addr += pages * PAGE

image += directory + b"\xff" * PAGE + texts
# whole double words only (see mkflash.py)
image += b"\xff" * (-len(image) % 8)
with open(out, "wb") as f:
    f.write(image)
print(f"  {out}: {len(image)} bytes, {len(texts) // 1024} KB of programs")
