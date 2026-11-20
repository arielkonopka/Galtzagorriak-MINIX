#!/usr/bin/env python3
"""ateasim - atea's flash step (spec RS.11), simulated on the QEMU flash
image (T33a part 3; the real atea is T33b).

QEMU has no boot stage: tools/gzapplytest.py stops QEMU after
`gzupdate apply` and runs this on the run's card (disk.img) and its flash
image (the file QEMU loads at address 0), as atea would at the reset:

  1. find the swap partition (MBR type 0x82, /dev/hd3) and read the
     pending sector (the sector after the staged file: "GZPEND1", epoch,
     size, the file's SHA-512; cmds/gzupdate.c pend());
  2. nothing pending and the image's first page not erased: nothing to do
     (exit 0, "atea: nothing pending");
  3. check the staged file again as atea does (signature, every hash; the
     card could have been swapped since gzupdate checked it), with
     tools/gzupdate-check.py and the trusted keys given;
  4. write the flash section: erase the first page first and program it
     last; every other page is compared first and written only if it
     differs; read back and compare;
  5. clear the pending sector.

--cut N stops after the Nth page operation (erase or program of one page;
the first page's erase is operation 1), as a power cut would: the next run
must finish the job (ACS.3).  Without a pending sector an erased first
page still makes it look on the card (S.5, backup domain lost).

Usage: ateasim.py --disk DISK --flash IMAGE --key PUB [--key PUB2]
                  [--board qemu-mps2] [--cut N] [-v]
Exit: 0 nothing to do or flashed, 1 refused (the reason on one line),
      3 cut."""
import argparse
import hashlib
import importlib.util
import os
import struct
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
SECTOR = 512
PAGE = 4096			# STM32WB55 flash page
PEND = b"GZPEND1\0"

spec = importlib.util.spec_from_file_location(
    "gzcheck", os.path.join(TOOLS, "gzupdate-check.py"))
gzcheck = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gzcheck)


class Cut(Exception):
    pass


def swap_partition(disk):
    with open(disk, "rb") as f:
        mbr = f.read(SECTOR)
    if mbr[510:512] != b"\x55\xaa":
        sys.exit("ateasim: no MBR on " + disk)
    for i in range(4):
        e = mbr[446 + 16 * i:462 + 16 * i]
        if e[4] == 0x82:
            start, n = struct.unpack_from("<II", e, 8)
            return start * SECTOR, n * SECTOR
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disk", required=True)
    ap.add_argument("--flash", required=True)
    ap.add_argument("--key", action="append", required=True)
    ap.add_argument("--board", default="qemu-mps2")
    ap.add_argument("--cut", type=int, default=0)
    ap.add_argument("-v", action="store_true")
    a = ap.parse_args()
    keys = [gzcheck.load_pub(k) for k in a.key]

    img = bytearray(open(a.flash, "rb").read())
    erased = img[:PAGE] == b"\xff" * PAGE
    part = swap_partition(a.disk)
    pend = hdr = None
    if part:
        with open(a.disk, "rb") as d:
            d.seek(part[0])
            sec0 = d.read(SECTOR)
            # The pending sector follows the staged file, whose size is
            # in its header (checked with the signature below).
            hdr = gzcheck.HDR.unpack_from(sec0) if sec0[:8] == b"GZUPDATE" \
                else None
            if hdr:
                size = hdr[14]
                if size % SECTOR == 0 and size + SECTOR <= part[1]:
                    d.seek(part[0] + size)
                    p = d.read(SECTOR)
                    if p[:8] == PEND:
                        pend = (struct.unpack_from("<II", p, 8), p[16:80],
                                size)
    if pend is None and not erased:
        print("atea: nothing pending")
        return 0
    if pend is None and part is None:
        print("atea: Insert the SD card")
        return 1
    if pend is None:
        # Image incomplete, backup state lost: whatever is staged.
        if not hdr:
            print("atea: nothing staged; Recovery: connect to a PC (DFU)")
            return 1
        size = hdr[14]
    else:
        size = pend[2]
    with tempfile.NamedTemporaryFile() as t:
        with open(a.disk, "rb") as d:
            d.seek(part[0])
            data = d.read(size)
        if pend and hashlib.sha512(data).digest() != pend[1]:
            print("atea: the staged file changed since gzupdate checked it")
            return 1
        t.write(data)
        t.flush()
        try:
            info = gzcheck.check(t.name, keys, a.board, None, None, False)
        except gzcheck.Bad as e:
            print("atea: " + str(e))
            return 1
        fl = [s for s in info["secs"] if s[1] == 1][0]
        _, _, off, ln, load, _ = fl
        new = data[off:off + ln]
    if a.v:
        print(f"atea: installing {info['release']} (epoch {info['epoch']}),"
              f" {ln} bytes at 0x{load:x}")
    end = load + ln
    if len(img) < end:
        img += b"\xff" * (end - len(img))
    ops = [0]

    def op():
        ops[0] += 1
        if a.cut and ops[0] >= a.cut:
            raise Cut()

    def save():
        with open(a.flash + ".tmp", "wb") as f:
            f.write(img)
        os.replace(a.flash + ".tmp", a.flash)

    pages = list(range(load, end, PAGE))
    changed = 0
    try:
        first = pages[0]
        want0 = new[:PAGE].ljust(PAGE, b"\xff")
        # Any page to change: the first page is erased first (even if it
        # is the same), so a cut never leaves a complete-looking mix.
        if bytes(img[load:end]) != new.ljust(end - load, b"\xff") \
                and img[first:first + PAGE] != b"\xff" * PAGE:
            img[first:first + PAGE] = b"\xff" * PAGE
            save()
            op()
        for p in pages[1:]:
            want = new[p - load:p - load + PAGE].ljust(PAGE, b"\xff")
            if img[p:p + PAGE] == want:
                continue			# compare first
            img[p:p + PAGE] = b"\xff" * PAGE
            save()
            op()
            img[p:p + PAGE] = want
            save()
            changed += 1
            op()
        if img[first:first + PAGE] != want0:
            img[first:first + PAGE] = want0		# program last
            save()
            changed += 1
            op()
    except Cut:
        print(f"atea: power cut at page operation {ops[0]}")
        return 3
    if bytes(img[load:end]) != new.ljust(end - load, b"\xff")[:end - load]:
        print("atea: flash does not read back")
        return 1
    if part and pend:
        with open(a.disk, "r+b") as d:
            d.seek(part[0] + size)
            d.write(b"\0" * SECTOR)
    print(f"atea: installed {info['release']}, {changed} pages written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
