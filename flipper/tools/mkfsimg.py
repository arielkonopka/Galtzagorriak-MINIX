#!/usr/bin/env python3
"""Build a MINIX V2 file system image (14-char names, magic 0x2468) from a
MINIX mkfs(1) prototype file, without root privileges or kernel support.

Usage:
  mkfsimg.py PROTO OUT.img [--blocks N] [--inodes N] [--mbr DISK.img]
             [--root DIR]

PROTO uses the format of MINIX mkfs(1):

    boot                       (ignored)
    <blocks> <inodes>          (0 = take from --blocks / --inodes)
    d--755 0 0                 root directory: mode uid gid
       bin d--755 0 0
          sh ---755 0 0 path/to/sh      regular file: host path
       $                                end of directory
       dev d--755 0 0
          console c--622 0 1 4 0        device: major minor
       $
    $

Mode strings: type (- d c b), setuid (- u), setgid (- g), octal perms.
Host paths are relative to --root (default: the PROTO's directory).

--mbr also writes a whole-disk image with an MBR and the file system as
partition 1 (type 0x81) at sector 2048, laid out like the SD card.
"""
import argparse
import os
import struct
import time

BLOCK = 1024
INODE_SIZE = 64
INODES_PER_BLOCK = BLOCK // INODE_SIZE
DIRENT = 16
NAME_MAX = 14
NR_DZONES = 7
NINDIR = BLOCK // 4
SUPER_V2 = 0x2468

S_IFREG, S_IFDIR, S_IFCHR, S_IFBLK = 0o100000, 0o040000, 0o020000, 0o060000
TYPES = {"-": S_IFREG, "d": S_IFDIR, "c": S_IFCHR, "b": S_IFBLK}


class Inode:
    def __init__(self, mode, uid, gid):
        self.mode, self.uid, self.gid = mode, uid, gid
        self.nlinks = 0
        self.size = 0
        self.zones = [0] * 10
        self.data = b""          # file contents (directories: built later)
        self.entries = []        # directories: (name, inode number)
        self.rdev = None


class FS:
    def __init__(self, nblocks, ninodes):
        if ninodes > 65535:
            raise SystemExit("MINIX 2.0 supports at most 65535 inodes")
        self.nblocks, self.ninodes = nblocks, ninodes
        self.imap_blocks = (ninodes + 1 + BLOCK * 8 - 1) // (BLOCK * 8)
        itable = (ninodes + INODES_PER_BLOCK - 1) // INODES_PER_BLOCK
        # The zone map covers zones firstdatazone-1 .. nblocks-1.
        zmap = 1
        while True:
            first = 2 + self.imap_blocks + zmap + itable
            need = (nblocks - first + 1 + BLOCK * 8 - 1) // (BLOCK * 8)
            if need <= zmap:
                break
            zmap = need
        self.zmap_blocks = zmap
        self.first = first
        if first >= nblocks:
            raise SystemExit("file system too small")
        # Metadata (boot, super, maps, inode table) is kept as one array;
        # data zones sparsely, so file systems of several GB need no RAM.
        self.img = bytearray(first * BLOCK)
        self.data = {}
        self.inodes = {}
        self.next_zone = first
        self.now = int(os.environ.get("SOURCE_DATE_EPOCH", time.time()))

    def new_inode(self, mode, uid, gid):
        n = len(self.inodes) + 1
        if n > self.ninodes:
            raise SystemExit("out of inodes")
        self.inodes[n] = Inode(mode, uid, gid)
        return n

    def alloc(self):
        z = self.next_zone
        if z >= self.nblocks:
            raise SystemExit("file system full")
        self.next_zone += 1
        return z

    def put_block(self, z, data):
        if z < self.first:
            self.img[z * BLOCK:z * BLOCK + len(data)] = data
        else:
            self.data[z] = bytes(data).ljust(BLOCK, b"\0")

    def image(self):
        """Metadata and the used data zones: everything up to the last
        allocated zone.  The rest of the file system is free space."""
        return bytes(self.img) + b"".join(
            self.data.get(z, bytes(BLOCK))
            for z in range(self.first, self.next_zone))

    def write_data(self, ino):
        """Allocate zones for ino.data (direct, single and double indirect)."""
        data = ino.data
        ino.size = len(data)
        nblk = (len(data) + BLOCK - 1) // BLOCK
        zones = []
        for i in range(nblk):
            z = self.alloc()
            self.put_block(z, data[i * BLOCK:(i + 1) * BLOCK])
            zones.append(z)
        ino.zones[:min(nblk, NR_DZONES)] = zones[:NR_DZONES]
        rest = zones[NR_DZONES:]
        if rest:
            ind = self.alloc()
            ino.zones[7] = ind
            self.put_block(ind, struct.pack(f"<{len(rest[:NINDIR])}I",
                                            *rest[:NINDIR]))
            rest = rest[NINDIR:]
        if rest:
            dind = self.alloc()
            ino.zones[8] = dind
            ptrs = []
            while rest:
                chunk, rest = rest[:NINDIR], rest[NINDIR:]
                ind = self.alloc()
                self.put_block(ind, struct.pack(f"<{len(chunk)}I", *chunk))
                ptrs.append(ind)
            if len(ptrs) > NINDIR:
                raise SystemExit("file too large")
            self.put_block(dind, struct.pack(f"<{len(ptrs)}I", *ptrs))

    def finish(self):
        for n, ino in self.inodes.items():
            if ino.mode & 0o170000 == S_IFDIR:
                ino.data = b"".join(struct.pack("<H14s", num,
                                                name.encode())
                                    for name, num in ino.entries)
            if ino.rdev is not None:
                ino.zones[0] = ino.rdev
            else:
                self.write_data(ino)
        # inode table
        itab = (2 + self.imap_blocks + self.zmap_blocks) * BLOCK
        for n, ino in self.inodes.items():
            raw = struct.pack("<HHHHIIII10I", ino.mode, ino.nlinks,
                              ino.uid, ino.gid, ino.size, self.now,
                              self.now, self.now, *ino.zones)
            off = itab + (n - 1) * INODE_SIZE
            self.img[off:off + INODE_SIZE] = raw
        # inode map: bit 0 and bits beyond ninodes are set
        imap = 2 * BLOCK
        for bit in list(range(0, len(self.inodes) + 1)) + \
                list(range(self.ninodes + 1, self.imap_blocks * BLOCK * 8)):
            self.img[imap + bit // 8] |= 1 << (bit % 8)
        # zone map: bit z <-> zone first-1+z; bit 0 reserved
        zmap = (2 + self.imap_blocks) * BLOCK
        used = self.next_zone - self.first
        nbits = self.nblocks - self.first + 1
        for bit in list(range(0, used + 1)) + \
                list(range(nbits, self.zmap_blocks * BLOCK * 8)):
            self.img[zmap + bit // 8] |= 1 << (bit % 8)
        # super block
        sb = struct.pack("<HHhhHhIhhI", self.ninodes,
                         min(self.nblocks, 0xFFFF), self.imap_blocks,
                         self.zmap_blocks, self.first, 0, 0x7FFFFFFF,
                         SUPER_V2, 0, self.nblocks)
        self.put_block(1, sb)
        return used


def parse_mode(s):
    if len(s) != 6 or s[0] not in TYPES:
        raise SystemExit(f"bad mode {s!r}")
    mode = TYPES[s[0]] | int(s[3:], 8)
    if s[1] == "u":
        mode |= 0o4000
    if s[2] == "g":
        mode |= 0o2000
    return mode


def build(fs, tokens, root):
    def directory(dnum, parent):
        d = fs.inodes[dnum]
        d.entries = [(".", dnum), ("..", parent)]
        d.nlinks += 1                       # "."
        fs.inodes[parent].nlinks += 1       # ".."
        while True:
            name = next(tokens)
            if name == "$":
                return
            if len(name) > NAME_MAX:
                raise SystemExit(f"name too long: {name}")
            mode = parse_mode(next(tokens))
            uid, gid = int(next(tokens)), int(next(tokens))
            num = fs.new_inode(mode, uid, gid)
            ino = fs.inodes[num]
            ino.nlinks += 1
            d.entries.append((name, num))
            kind = mode & 0o170000
            if kind == S_IFDIR:
                directory(num, dnum)
            elif kind in (S_IFCHR, S_IFBLK):
                major, minor = int(next(tokens)), int(next(tokens))
                ino.rdev = (major << 8) | minor
            else:
                with open(os.path.join(root, next(tokens)), "rb") as f:
                    ino.data = f.read()

    rmode = parse_mode(next(tokens))
    ruid, rgid = int(next(tokens)), int(next(tokens))
    rnum = fs.new_inode(rmode, ruid, rgid)
    assert rnum == 1
    fs.inodes[1].nlinks += 1            # root's own ".." entry
    directory(1, 1)
    fs.inodes[1].nlinks -= 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("proto")
    ap.add_argument("out")
    ap.add_argument("--blocks", type=int, default=0)
    ap.add_argument("--inodes", type=int, default=0)
    ap.add_argument("--mbr")
    ap.add_argument("--root")
    ap.add_argument("--trim", action="store_true",
                    help="write only the used part (metadata + files); the "
                    "file system still has --blocks blocks.  For dd onto "
                    "a partition of at least that size.")
    a = ap.parse_args()

    with open(a.proto) as f:
        tokens = iter([t for line in f for t in line.split("#", 1)[0].split()])
    next(tokens)                                    # boot
    blocks, inodes = int(next(tokens)), int(next(tokens))
    blocks = a.blocks or blocks
    inodes = a.inodes or inodes or min(65535, max(64, blocks // 4))
    fs = FS(blocks, inodes)
    build(fs, tokens, a.root or os.path.dirname(os.path.abspath(a.proto)))
    used = fs.finish()
    img = fs.image()
    with open(a.out, "wb") as f:
        f.write(img)
        if not a.trim:
            f.truncate(blocks * BLOCK)        # sparse
    print(f"  {a.out}: {blocks} blocks ({blocks // 1024} MB), {inodes} "
          f"inodes, {len(fs.inodes)} files, {used} data zones used"
          + (f"; trimmed to {len(img) // 1024} KB" if a.trim else ""))

    if a.mbr:
        start = 2048
        nsec = blocks * 2
        mbr = bytearray(512)
        mbr[446:462] = struct.pack("<B3sB3sII", 0, b"\x00\x02\x00", 0x81,
                                   b"\xfe\xff\xff", start, nsec)
        mbr[510:512] = b"\x55\xaa"
        with open(a.mbr, "wb") as f:
            f.write(mbr)
            f.write(b"\0" * ((start - 1) * 512))
            f.write(img)
            f.truncate(start * 512 + blocks * BLOCK)
        print(f"  {a.mbr}: MBR + partition 1 (type 0x81) at sector {start}")


if __name__ == "__main__":
    main()
