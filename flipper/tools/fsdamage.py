#!/usr/bin/env python3
"""fsdamage - a damaged file system on the card must not halt the system
(security review 2026-11-16, F1-F3), in QEMU.

Usage: fsdamage.py [ACN.3]

Boots build/qemu/minix (as tools/powercut.py does, with no power cut) and,
as root, makes small file systems on the reserved swap partition /dev/hd3,
damages them with dd and uses them:
  F1  an indirect block full of junk zone numbers: cat and rm the file;
  F2  a zone bit map wiped while files use the zones: rm them;
  F3  a super block whose bit map is too small for its zones: mount must
      refuse it.
Passes when nothing panics, the expected messages ("run fsck",
"inconsistent super block") appear, and the shell still answers.  Before
the fix F1 and F2 stopped the whole system with an FS panic.

ACN.3 (spec N.4, T17a): fsck on the full-size 2 GB /usr (/dev/hd2) made by
the first boot's setup, with 5 000 files, damaged on the card while QEMU is
off: a zone marked free but used, a duplicate zone (in a later zone
window), a wrong link count, an orphan inode.  /etc/rc.conf sets
fsck_mem=8, so the computed zone map is one 8 MB window of 256.  The boot's
fsck -p repairs every error (left 0), a second check finds nothing, and
util-linux fsck.minix -f (no repairs), if the host has it, finds the card clean.

Needs BOARD=qemu built (make minix disk).  Exit status 0: pass.
"""
import os
import re
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from powercut import Run	# noqa: E402

DEV = "/dev/hd3"
MNT = "/tmp/m"
Z32 = "z" * 31				# + newline: 32 bytes a line, 2 KB
# 2 KB of "z": one line, doubled 6 times (a long command line is cut)
JUNK = ["echo %s > /tmp/junk" % Z32,
	"for i in 1 2 3 4 5 6; do cat /tmp/junk /tmp/junk > /tmp/j2; "
	"mv /tmp/j2 /tmp/junk; done"]


def sb_word(r, i):
	"""The i-th 16-bit word of DEV's super block (block 1), via od -d."""
	mark = len(r.out)
	if r.sh("dd if=%s bs=16 skip=64 count=1 2>/dev/null | od -d"
			% DEV, 30) != 0:
		return None
	m = re.compile(rb"0000000((?: +\d+){8})").search(r.out, mark)
	return int(m.group(1).split()[i]) if m else None


def main():
	r = Run(0, 0)
	ok = True

	def step(cmd, secs=120):
		nonlocal ok
		s = r.sh(cmd, secs)
		if s is None:
			print("  no answer to: " + cmd)
			ok = False
		return s

	try:
		if not r.login():
			print("FAIL no login")
			return 1
		step("mkdir %s" % MNT)
		for c in JUNK:
			step(c)

		# F1: a 20 KB file, so that it has an indirect block (zones
		# are given out in order on a fresh file system).
		print("F1: junk in an indirect block", flush=True)
		step("mkfs %s 1024 && mount %s %s && dd if=/bin/sh of=%s/f "
			"bs=1024 count=20 && umount %s" % (DEV, DEV, MNT, MNT, DEV))
		# its inode (2: the first after the root) is at byte 64 of the
		# first inode block (block 4 with one block per bit map); the
		# single indirect zone is i_zone[7], at byte 52 of the inode
		mark = len(r.out)
		r.sh("dd if=%s bs=4 skip=%d count=1 2>/dev/null | od -d"
			% (DEV, (4 * 1024 + 64 + 52) // 4), 30)
		m = re.compile(rb"0000000 +(\d+) +(\d+)").search(r.out, mark)
		if m is None or int(m.group(1)) == 0:
			print("  could not find the indirect block")
			ok = False
		else:
			step("dd if=/tmp/junk of=%s bs=1024 seek=%s count=1"
				% (DEV, m.group(1).decode()))
			step("mount %s %s && cat %s/f > /dev/null; rm %s/f; "
				"umount %s" % (DEV, MNT, MNT, MNT, DEV))

		# F2: the zone bit map (block 3: one inode map block) wiped.
		print("F2: zone bit map wiped", flush=True)
		step("mkfs %s 1024 && mount %s %s && dd if=/bin/sh of=%s/g "
			"bs=1024 count=12 && umount %s" % (DEV, DEV, MNT, MNT, DEV))
		step("dd if=/dev/zero of=%s bs=1024 seek=3 count=1"
			% DEV)
		step("mount %s %s && rm %s/g; umount %s" % (DEV, MNT, MNT, DEV))

		# F3: the zone map block count (u16 at offset 6) set to 1 on a
		# file system that needs more: refused.
		print("F3: bit map too small for the zones", flush=True)
		step("mkfs %s 20000" % DEV, 300)
		zmap = sb_word(r, 3) or 0
		if zmap < 2:
			print("  test file system has %d zone map block(s)" % zmap)
			ok = False
		# s_zmap_blocks (u16 at offset 6 of block 1) := 1, copied from
		# s_imap_blocks (offset 4, which is 1): MINIX has no printf
		step("dd if=%s of=/tmp/one bs=1 skip=1028 count=2 && "
			"dd if=/tmp/one of=%s bs=1 seek=1030 count=2" % (DEV, DEV))
		s = r.sh("mount %s %s" % (DEV, MNT), 60)
		if s == 0:
			print("  the damaged file system was mounted")
			r.sh("umount %s" % DEV, 30)
			ok = False
		if re.search(rb"panic|Kernel panic|FS panic", r.out):
			print("  a panic was printed")
			ok = False
		if r.sh("sync; sleep 1; echo alive", 30) != 0:
			print("  the shell does not answer")
			ok = False
		r.wait(rb"\0never\0", 2)	# the servers' last messages
		for f, msg in (("F1", b"Illegal zone number"),
				("F2", b"freeing unused zone"),
				("F3", b"inconsistent super block")):
			if msg not in r.out:
				print("  %s: no '%s' message" % (f, msg.decode()))
				ok = False
	except (OSError, ValueError) as e:	# QEMU gone: halted
		print("  the system stopped (%s)" % e)
		ok = False
	finally:
		r.close()
	if re.search(rb"panic", r.out):
		ok = False
	print("PASS" if ok else "FAIL", "damaged file systems")
	if not ok:
		sys.stdout.write(r.out[-3000:].decode(errors="replace"))
	return 0 if ok else 1


class Part:
	"""MBR partition n (1-4) of a card image: its MINIX V2 file system."""

	def __init__(self, img, n):
		self.f = open(img, "r+b")
		self.f.seek(446 + 16 * (n - 1) + 8)
		self.base, self.size = struct.unpack("<II", self.f.read(8))
		self.base *= 512
		self.size *= 512
		sb = self.block(1)
		(self.ninodes, _, self.imap, self.zmap, self.first, self.log,
			_, _, self.flags, self.zones) = \
			struct.unpack_from("<HHhhHhIhHI", sb)

	def block(self, b):
		self.f.seek(self.base + b * 1024)
		return bytearray(self.f.read(1024))

	def put(self, b, data):
		self.f.seek(self.base + b * 1024)
		self.f.write(bytes(data))

	def inode(self, ino):
		b, o = divmod((ino - 1) * 64, 1024)
		return 2 + self.imap + self.zmap + b, o

	def get_inode(self, ino):
		b, o = self.inode(ino)
		blk = self.block(b)
		mode, nlinks = struct.unpack_from("<HH", blk, o)
		zones = list(struct.unpack_from("<10I", blk, o + 24))
		return mode, nlinks, zones

	def set_inode(self, ino, nlinks=None, zone=None):
		b, o = self.inode(ino)
		blk = self.block(b)
		if nlinks is not None:
			struct.pack_into("<H", blk, o + 2, nlinks)
		if zone is not None:
			struct.pack_into("<I", blk, o + 24 + 4 * zone[0], zone[1])
		self.put(b, blk)

	def zbit(self, z, on):
		bit = z - self.first + 1
		b = 2 + self.imap + bit // 8192
		blk = self.block(b)
		if on:
			blk[(bit % 8192) // 8] |= 1 << (bit % 8)
		else:
			blk[(bit % 8192) // 8] &= ~(1 << (bit % 8)) & 0xFF
		self.put(b, blk)

	def unlink(self, dino, name):
		"""Clear name's entry in directory inode dino (first zone)."""
		z = self.get_inode(dino)[2][0]
		blk = self.block(z << self.log)
		for o in range(0, 1024, 16):
			if blk[o + 2:o + 16].rstrip(b"\0") == name:
				blk[o:o + 2] = b"\0\0"
				self.put(z << self.log, blk)
				return True
		return False

	def set_flags(self, v):
		sb = self.block(1)
		struct.pack_into("<H", sb, 18, v)
		self.put(1, sb)

	def close(self):
		self.f.close()


def acn3():
	from sdcut import Card, halt, DONE
	r = Card(0, 0)
	ok = True
	try:
		if not r.login():
			print("FAIL ACN.3: no login")
			return 1
		print("ACN.3: 5 000 files and 10 MB on /usr", flush=True)
		# 50 directories of 100 empty files: sh makes them, no exec
		r.sh("mkdir /usr/f; cd /usr/f; for a in 0 1 2 3 4; do "
			"for b in 0 1 2 3 4 5 6 7 8 9; do mkdir d$a$b; "
			"for c in 0 1 2 3 4 5 6 7 8 9; do for d in 0 1 2 3 4 "
			"5 6 7 8 9; do >d$a$b/f$c$d; done; done; done; done; cd /",
			1800)
		r.sh("dd if=/dev/zero of=/usr/big bs=1024 count=10240 "
			"2>/dev/null; for i in 1 2 3 4; do cp /bin/sh /usr/s$i; "
			"done; cp /bin/sh /usr/dup; cp /bin/sh /usr/orph", 600)
		ino = {}
		for n in ("s1", "s2", "s3", "s4", "dup", "orph"):
			m = r.output("ls -id /usr/" + n, 30)
			ino[n] = int(m.split()[0]) if m and m.split() and \
				m.split()[0].isdigit() else None
		nfiles = r.output("ls /usr/f/d49 | wc -l", 60)
		print("  inodes %s, files in the last directory: %s" % (
			ino, nfiles.strip() if nfiles else None))
		r.sh("echo fsck_mem=8 >>/etc/rc.conf", 30)
		halt(r)
		r.p.kill()
		r.p.wait()
		if None in ino.values():
			print("  ACN.3: no inode numbers")
			return 1
		u = Part(r.image(), 2)
		print("  /usr: %d inodes, %d zones, %d zone map blocks" % (
			u.ninodes, u.zones, u.zmap))
		z1 = u.get_inode(ino["s1"])[2][0]
		u.zbit(z1, False)			# used zone marked free
		z2 = u.get_inode(ino["s2"])[2][0]
		u.set_inode(ino["dup"], zone=(1, z2))	# duplicate (window > 0)
		u.set_inode(ino["s3"], nlinks=3)	# wrong link count
		if not u.unlink(1, b"orph"):		# orphan inode
			print("  ACN.3: no /usr/orph entry")
			ok = False
		u.set_flags(0)
		print("  damaged: zone %d free, zone %d twice, links, orphan"
			% (z1, z2))
		u.close()

		r.restart()
		ok1 = r.login()
		got = DONE.findall(r.out)
		i = r.out.find(b"fsck /dev/hd2")
		print(r.out[i:i + 1200].decode(errors="replace")
			if i >= 0 else "  (no fsck of /dev/hd2)")
		fixed = [int(a) for a, b in got] if got else []
		left = [int(b) for a, b in got] if got else []
		if not ok1 or i < 0 or not fixed or max(fixed) < 4 \
							or max(left) > 0:
			print("  ACN.3: not every error repaired: %s" % got)
			ok = False
		halt(r)
		r.p.kill()
		r.p.wait()

		u = Part(r.image(), 2)
		u.set_flags(0)				# check it once more
		u.close()
		r.restart()
		r.login()
		again = DONE.findall(r.out)
		print("  second check: %s" % again)
		if again != [(b"0", b"0")]:
			print("  ACN.3: the second check is not clean")
			ok = False
		halt(r)
		r.p.kill()
		r.p.wait()

		if shutil.which("fsck.minix"):
			u = Part(r.image(), 2)
			part = os.path.join(r.dir, "usr.img")
			with open(r.image(), "rb") as f, open(part, "wb") as o:
				f.seek(u.base)
				left_b = u.size
				while left_b > 0:
					d = f.read(min(left_b, 1 << 20))
					if not d:
						break
					if d.count(0) == len(d):
						o.seek(len(d), 1)
					else:
						o.write(d)
					left_b -= len(d)
				o.truncate(u.size)
			u.close()
			p = subprocess.run(["fsck.minix", "-f", part],
				stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
			print("  fsck.minix -f: exit %d %s" % (p.returncode,
				p.stdout.decode(errors="replace")[-300:]))
			if p.returncode != 0:
				ok = False
		else:
			print("  (no fsck.minix on this host: not cross-checked)")
		if re.search(rb"panic", r.out):
			ok = False
	except (OSError, ValueError) as e:
		print("  ACN.3: QEMU stopped (%s)" % e)
		ok = False
	finally:
		r.close()
	print("PASS" if ok else "FAIL", "ACN.3")
	return 0 if ok else 1


if __name__ == "__main__":
	if sys.argv[1:] == ["ACN.3"]:
		sys.exit(acn3())
	sys.exit(main())
