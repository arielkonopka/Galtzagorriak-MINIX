#!/usr/bin/env python3
"""Convert an ARM ELF executable (linked at 0 by ld/user.ld, with -Wl,-q)
into a MINIX a.out with relocation information.

Usage: elf2mnx.py IN.elf OUT [--stack BYTES]

Output layout (see <a.out.h> and mm/exec.c):
  32-byte header: magic 01 03, flags A_EXEC, cpu A_ARMCM, a_text, a_data,
                  a_bss, a_entry, a_total (text+data+bss+stack/heap), a_syms=0
  text, data
  relocations in the GEMDOS format read by relocate() in mm/exec.c:
      long  offset of the first 32-bit word to relocate (0 = none)
      bytes  distance to the next one (even, 2..254); 1 = advance 254;
             0 = end
Every word marked is an absolute address in the image; exec adds the load
address.  PC-relative relocations need no fixing because the whole image
moves as one.

Position-independent programs (ld/user-pic.ld: .got/.data linked at
A_DATA_LINK = 0x10000000) become separate I&D (A_SEP) images: the text has
no relocations at all, so it can run from anywhere (the flash text cache);
the relocation offsets count from the start of data (the first one stored
plus 1, as the GOT starts at offset 0) and cover the GOT and
initialized pointers; exec adds the text or the data base depending on
whether the word holds an address below or above A_DATA_LINK.  a_total
then excludes the text and a_syms holds the CRC-32 of the text.

With --libc LIBC.json (tools/mkshlibc.py; ld/user-shlibc.ld) the program
uses the shared libc in flash (spec R2b.9): the first bytes of its data are
the libc area, filled here with the library's initial GOT and data (and its
relocations); addresses inside the library's flash are absolute and stay as
they are.  The header is 48 bytes, with flag A_LIBC and the library's build
id in a_tbase: exec refuses the program with any other library.
"""
import argparse
import json
import os
import re
import struct
import sys
import zlib

# a_version = the ABI the program is built for (spec J.3; MM's exec
# refuses another one, 0 = from before the stamp).
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "../../include/minix/gzversion.h")) as _f:
    GZ_ABI = int(re.search(r"#define\s+GZ_ABI\s+(\d+)", _f.read()).group(1))

R_ARM_NONE, R_ARM_ABS32, R_ARM_REL32, R_ARM_TARGET1 = 0, 2, 3, 38
PC_RELATIVE = {
    3,    # R_ARM_REL32
    10,   # R_ARM_THM_CALL
    25, 28, 29,   # R_ARM_BASE_PREL / R_ARM_CALL / R_ARM_JUMP24
    30,   # R_ARM_THM_JUMP24
    40,   # R_ARM_V4BX
    42,   # R_ARM_PREL31
    51,   # R_ARM_THM_JUMP19
    102, 103,  # R_ARM_THM_JUMP11, R_ARM_THM_JUMP8
    11, 12,    # R_ARM_THM_PC8, R_ARM_THM_PC12 (older numbering)
    129,  # R_ARM_THM_PC12
}
SHN_UNDEF, SHN_ABS = 0, 0xFFF1
A_EXEC, A_SEP, A_ARMCM, A_LIBC = 0x10, 0x20, 0x18, 0x08
A_NOSWAP = 0x80      # never swapped in plain mode (Secret: yes, R12.28)
A_DATA_LINK = 0x10000000
# Resolved at link time against the GOT origin (= start of data): no fixup.
GOT_RELATIVE = {24, 26}    # R_ARM_GOTOFF32, R_ARM_GOT_BREL


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("out")
    ap.add_argument("--stack", type=int, default=6144,
                    help="bytes for stack and heap (like chmem)")
    ap.add_argument("--libc", help="shared libc description (mkshlibc.py)")
    ap.add_argument("--secret", action="store_true",
                    help="A_NOSWAP: it holds secrets, never swap it in "
                    "plain mode (Secret: yes, spec R12.28)")
    a = ap.parse_args()
    a.lib = None
    if a.libc:
        with open(a.libc) as f:
            a.lib = json.load(f)
    with open(a.elf, "rb") as f:
        elf = f.read()

    if elf[:4] != b"\x7fELF" or elf[4] != 1 or elf[5] != 1:
        sys.exit("not a 32-bit little-endian ELF")
    (e_type, e_machine, _, e_entry, _, e_shoff, _, _, _, _, e_shentsize,
     e_shnum, e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", elf, 16)
    if e_machine != 40:
        sys.exit("not an ARM ELF")

    shdrs = [struct.unpack_from("<IIIIIIIIII", elf, e_shoff + i * e_shentsize)
             for i in range(e_shnum)]
    strtab = shdrs[e_shstrndx]

    def name(off):
        s = elf[strtab[4] + off:]
        return s[:s.index(b"\0")].decode()

    secs = {name(sh[0]): sh for sh in shdrs}
    if ".data" in secs and secs[".data"][3] >= A_DATA_LINK:
        return separate(a, elf, secs, name, shdrs, e_entry)
    if a.lib:
        sys.exit(f"{a.elf}: --libc needs a position-independent program")
    for need in (".text", ".data", ".bss"):
        if need not in secs:
            sys.exit(f"missing section {need}")
    text, data, bss = secs[".text"], secs[".data"], secs[".bss"]
    if text[3] != 0:
        sys.exit(".text must be linked at address 0")
    t_end = text[3] + text[5]
    d_start, d_end = data[3], data[3] + data[5]
    if d_start < t_end:
        sys.exit(".data overlaps .text")

    image = bytearray(d_end)
    image[0:t_end] = elf[text[4]:text[4] + text[5]]
    image[d_start:d_end] = elf[data[4]:data[4] + data[5]]
    a_text, a_data = t_end, d_end - t_end
    a_bss = bss[3] + bss[5] - d_end if bss[5] else 0

    # Symbol table, to recognise relocations against absolute/undefined
    # (weak) symbols, which must not be relocated.
    symtab = secs.get(".symtab")
    syms = []
    if symtab:
        for i in range(symtab[5] // 16):
            _, value, _, info, _, shndx = struct.unpack_from(
                "<IIIBBH", elf, symtab[4] + i * 16)
            syms.append((value, shndx))

    relocs = set()
    for sname, sh in secs.items():
        if sh[1] != 9:                        # SHT_REL
            continue
        target = name(shdrs[sh[7]][0])        # section being relocated
        if target not in (".text", ".data"):
            continue
        for i in range(sh[5] // 8):
            r_offset, r_info = struct.unpack_from("<II", elf, sh[4] + i * 8)
            rtype, rsym = r_info & 0xFF, r_info >> 8
            if rtype in PC_RELATIVE or rtype == R_ARM_NONE:
                continue
            if rtype not in (R_ARM_ABS32, R_ARM_TARGET1):
                sys.exit(f"{a.elf}: unsupported relocation type {rtype} at "
                         f"{r_offset:#x} in {target} (compile without "
                         "-mslow-flash-data/-mpure-code)")
            if rsym < len(syms) and syms[rsym][1] in (SHN_UNDEF, SHN_ABS):
                continue                      # absolute value, keep as is
            if r_offset % 2 or r_offset + 4 > len(image):
                sys.exit(f"bad relocation offset {r_offset:#x}")
            relocs.add(r_offset)

    stream = bytearray()
    offs = sorted(relocs)
    if not offs:
        stream += struct.pack("<I", 0)
    else:
        stream += struct.pack("<I", offs[0])
        for prev, cur in zip(offs, offs[1:]):
            d = cur - prev
            while d > 254:
                stream.append(1)
                d -= 254
            stream.append(d)
        stream.append(0)

    a_total = a_text + a_data + a_bss + a.stack
    hdr = struct.pack("<BBBBBBHIIIIII", 0x01, 0x03,
                      A_EXEC | (A_NOSWAP if a.secret else 0), A_ARMCM, 32, 0, GZ_ABI,
                      a_text, a_data, a_bss, e_entry, a_total, 0)
    with open(a.out, "wb") as f:
        f.write(hdr + image + stream)
    print(f"  {a.out}: text {a_text} data {a_data} bss {a_bss} "
          f"total {a_total}, {len(offs)} relocations")


def symbols(elf, secs):
    symtab = secs.get(".symtab")
    syms = []
    if symtab:
        for i in range(symtab[5] // 16):
            _, value, _, info, _, shndx = struct.unpack_from(
                "<IIIBBH", elf, symtab[4] + i * 16)
            syms.append((value, shndx))
    return syms


def reloc_stream(offs):
    """GEMDOS-style stream for A_SEP; the first long is the first offset
    plus 1, because the GOT starts at data offset 0 (0 = no relocations)."""
    stream = bytearray()
    if not offs:
        return bytearray(struct.pack("<I", 0))
    stream += struct.pack("<I", offs[0] + 1)
    for prev, cur in zip(offs, offs[1:]):
        d = cur - prev
        while d > 254:
            stream.append(1)
            d -= 254
        stream.append(d)
    stream.append(0)
    return stream


def separate(a, elf, secs, name, shdrs, e_entry):
    """Position-independent program -> A_SEP image (see the top)."""
    text = secs[".text"]
    if text[3] != 0:
        sys.exit(".text must be linked at address 0")
    t_bytes = elf[text[4]:text[4] + text[5]]
    parts = [secs[n] for n in (".got", ".data") if n in secs and secs[n][5]]
    d_start = A_DATA_LINK
    d_end = max(sh[3] + sh[5] for sh in parts) if parts else d_start
    data = bytearray(d_end - d_start)
    for sh in parts:
        data[sh[3] - d_start:sh[3] - d_start + sh[5]] = \
            elf[sh[4]:sh[4] + sh[5]]
    bss = secs.get(".bss")
    a_bss = bss[3] + bss[5] - d_end if bss and bss[5] else 0
    syms = symbols(elf, secs)
    lib = a.lib
    area = lib["area"] if lib else 0
    lib_lo = lib["base"] if lib else 0
    lib_hi = lib_lo + lib["size"] if lib else 0
    if lib and secs.get(".got", (0,) * 4)[3] != d_start:
        sys.exit(f"{a.elf}: --libc needs the GOT first in the data "
                 "(ld/user-shlibc.ld)")
    if lib and any(data[:area]):
        sys.exit(f"{a.elf}: the libc area is not empty")

    def in_area(v):
        return lib and d_start <= v < d_start + area

    relocs = set()
    for sname, sh in secs.items():
        if sh[1] != 9:                        # SHT_REL
            continue
        target = name(shdrs[sh[7]][0])
        for i in range(sh[5] // 8):
            r_offset, r_info = struct.unpack_from("<II", elf, sh[4] + i * 8)
            rtype, rsym = r_info & 0xFF, r_info >> 8
            if (rtype in PC_RELATIVE or rtype in GOT_RELATIVE
                    or rtype == R_ARM_NONE):
                continue
            if target == ".text":
                sys.exit(f"{a.elf}: absolute relocation (type {rtype}) in "
                         f"the text at {r_offset:#x}: an object not compiled "
                         "with -fpic -msingle-pic-base")
            if target not in (".data", ".got"):
                continue
            if rtype not in (R_ARM_ABS32, R_ARM_TARGET1):
                sys.exit(f"{a.elf}: unsupported relocation type {rtype} at "
                         f"{r_offset:#x} in {target}")
            if rsym < len(syms) and syms[rsym][1] in (SHN_UNDEF, SHN_ABS):
                # absolute, unless it is a libc variable in the area
                v = struct.unpack_from("<I", data, r_offset - d_start)[0]
                if not in_area(v):
                    continue
            relocs.add(r_offset - d_start)
    # The linker fills the GOT itself and -q emits no relocations for it:
    # every non-zero entry is an address (zero: undefined weak symbol).
    # (Only the program's part: the libc area is still zeros.)
    got = secs.get(".got")
    if got:
        for off in range(got[3] - d_start + area, got[3] - d_start + got[5], 4):
            v = struct.unpack_from("<I", data, off)[0]
            if v != 0 and not (lib and lib_lo <= v < lib_hi):
                relocs.add(off)
    for off in relocs:
        v = struct.unpack_from("<I", data, off)[0]
        if not (v < len(t_bytes) + 1 or d_start <= v <= d_end + a_bss):
            sys.exit(f"{a.elf}: data word at {off:#x} = {v:#x} is neither "
                     "a text nor a data address")
    if lib:                       # the area: the library's own GOT and data
        tmpl = bytes.fromhex(lib["template"])
        if len(tmpl) > area:
            sys.exit(f"{a.libc}: template longer than the area")
        data[0:len(tmpl)] = tmpl
        relocs |= set(lib["relocs"])
    offs = sorted(relocs)
    a_text, a_data = len(t_bytes), len(data)
    a_total = a_data + a_bss + a.stack
    crc = zlib.crc32(t_bytes) & 0xFFFFFFFF
    flags = A_EXEC | A_SEP | (A_LIBC if lib else 0) | (A_NOSWAP if a.secret else 0)
    hdr = struct.pack("<BBBBBBHIIIIII", 0x01, 0x03, flags, A_ARMCM,
                      48 if lib else 32, 0, GZ_ABI, a_text, a_data, a_bss,
                      e_entry, a_total, crc)
    if lib:                       # a_trsize, a_drsize, a_tbase = id, a_dbase
        hdr += struct.pack("<IIII", 0, 0, lib["id"], 0)
    with open(a.out, "wb") as f:
        f.write(hdr + t_bytes + data + reloc_stream(offs))
    print(f"  {a.out}: text {a_text} data {a_data} bss {a_bss} "
          f"total {a_total} (+text), {len(offs)} relocations, PIC"
          f"{', shared libc' if lib else ''}")


if __name__ == "__main__":
    main()
