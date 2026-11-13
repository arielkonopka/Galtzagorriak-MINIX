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
"""
import argparse
import struct
import sys

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
A_EXEC, A_ARMCM = 0x10, 0x18


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("out")
    ap.add_argument("--stack", type=int, default=16384,
                    help="bytes for stack and heap (like chmem)")
    a = ap.parse_args()
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
    hdr = struct.pack("<BBBBBBHIIIIII", 0x01, 0x03, A_EXEC, A_ARMCM, 32, 0, 0,
                      a_text, a_data, a_bss, e_entry, a_total, 0)
    with open(a.out, "wb") as f:
        f.write(hdr + image + stream)
    print(f"  {a.out}: text {a_text} data {a_data} bss {a_bss} "
          f"total {a_total}, {len(offs)} relocations")


if __name__ == "__main__":
    main()
