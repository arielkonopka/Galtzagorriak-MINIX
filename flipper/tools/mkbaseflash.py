#!/usr/bin/env python3
"""Build programs into the installed region of a flash image (spec Phase 2b,
R2b.6, make base-flash).

Usage: mkbaseflash.py system.bin out.bin FLASH_BASE REGION_OFFSET [--rom=rom.img] prog.aout ...

out.bin is system.bin padded to REGION_OFFSET (the installed region, which
starts after the 192 KB system image and the 96 KB of the shared libraries,
libc and libfuri + libapp, R2b.9, on every layout; system.bin = minix.bin,
which carries them), then the
region in the format of src/mm/finst.c:
    page 0   directory "GINB" (made on the PC), generation 1, one 64-byte
             record per program: SHA-256 of the text, address, size, the
             binding left erased (ones), seq, flags, dead marker erased;
    page 1   erased (ones);
    page 2.. the texts, each from a page boundary.
At boot /etc/rc runs "pkg bind /bin": MM gives each record to the file
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
import zlib
import sys

PAGE = 4096
HDR = 8
REC = 64
NREC = (PAGE - HDR) // REC
SEED = 0x424E4947          # "GINB"
A_SEP = 0x20

ROM_SIZE = 32 * 1024       # hal.h HAL_ROM_SIZE: after the directories

system, out, flash, region, *progs = sys.argv[1:]
rom = b""
if progs and progs[0].startswith("--rom="):   # the settings image (L.2)
    rom = open(progs.pop(0)[6:], "rb").read()
    if len(rom) > ROM_SIZE:
        sys.exit(f"mkbaseflash: the settings image is {len(rom)} bytes, "
                 f"more than {ROM_SIZE}")
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
addr = flash + region + 2 * PAGE + ROM_SIZE   # installs skip the ROM too
for seq, path in enumerate(progs):
    data = open(path, "rb").read()
    if data[0:2] != b"\x01\x03" or not data[2] & A_SEP:
        sys.exit(f"mkbaseflash: {path}: not a separate I&D MINIX program")
    hdrlen = data[4]
    a_text = struct.unpack_from("<I", data, 8)[0]
    text = data[hdrlen:hdrlen + a_text]
    # flags: CRC-32 of the text's first 256 bytes (exec checks it; #58)
    head = zlib.crc32(text[:256]) & 0xFFFFFFFF
    if head == 0xFFFFFFFF:
        head = 0xFFFFFFFE              # all ones means "no CRC"
    rec = hashlib.sha256(text).digest() + struct.pack(
        "<IIIIII", addr, a_text, 0xFFFFFFFF, 0xFFFFFFFF, seq, head)
    directory[HDR + seq * REC:HDR + seq * REC + len(rec)] = rec
    pages = (a_text + PAGE - 1) // PAGE
    texts += text + b"\xff" * (pages * PAGE - a_text)
    print(f"  {path.rsplit('/', 1)[-1]:10s} at {addr:#010x}, {a_text} bytes")
    addr += pages * PAGE

image += directory + b"\xff" * PAGE + rom + b"\xff" * (ROM_SIZE - len(rom))
image += texts
# whole double words only (see mkflash.py)
image += b"\xff" * (-len(image) % 8)
with open(out, "wb") as f:
    f.write(image)
print(f"  {out}: {len(image)} bytes, {len(texts) // 1024} KB of programs")
