#!/usr/bin/env python3
"""Pack the kernel and boot server ELF images into one flash binary.

Usage: mkflash.py OBJCOPY NM out.bin kernel.elf mm.elf fs.elf init.elf

Each ELF is converted with objcopy -O binary (flash contents only: code,
read-only data and the load image of .data) and placed at its own flash
address (_stext; for the kernel that is the start of flash)."""
import os
import subprocess
import sys
import tempfile

objcopy, nm, out, kernel, *servers = sys.argv[1:]


def sym(elf, name):
    result = subprocess.run([nm, elf], capture_output=True, text=True,
                            check=True)
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    raise SystemExit(f"{elf}: symbol {name} not found")


def binary(elf):
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "x.bin")
        subprocess.run([objcopy, "-O", "binary", "-R", ".bss", elf, path],
                       check=True)
        with open(path, "rb") as f:
            return f.read()


base = sym(kernel, "_stext")
image = bytearray()
for elf in [kernel] + servers:
    start = sym(elf, "_stext") - base
    if start < len(image):
        raise SystemExit(f"{elf}: overlaps the previous image")
    image += b"\xff" * (start - len(image))
    image += binary(elf)
    print(f"  {os.path.basename(elf):10s} at {base + start:#010x}, "
          f"{len(image) - start} bytes")
# The STM32WB flash is programmed in double words.  ST's DFU loader writes a
# trailing half as a lone first word, and the controller then waits for the
# second one (FLASH_SR.CFGBSY stays set) until the next reset: no erase or
# program works, the flash text cache stays off.  Whole double words only.
image += b"\xff" * (-len(image) % 8)
with open(out, "wb") as f:
    f.write(image)
print(f"  {out}: {len(image)} bytes")
