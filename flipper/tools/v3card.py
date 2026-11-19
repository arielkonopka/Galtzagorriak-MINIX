#!/usr/bin/env python3
"""v3card - a MINIX V3 /home in QEMU (spec O.4, ACO.1 and ACO.2 in part).

Usage: v3card.py

  1. A fresh copy of build/qemu/minix/disk.img (make minix disk, BOARD
     qemu) boots once: the first boot makes /usr, swap and /home.
  2. QEMU is stopped.  On the host, util-linux 'mkfs.minix -3' makes a V3
     file system of /home's size with 200 000 inodes (past the old
     16-bit ino_t, O.4d) and it replaces /home (/dev/hd4).
  3. Booted again (/etc/rc's fsck -p checks the new V3 /home), as root
     (the test mounts it again by hand):
       /home mounts; a 57- and a 60-character name work with cp, ls, mv,
       mkdir/rmdir; 61 characters give "File name too long"; on V2 /usr
       a 15-character name is still cut at 14 (existing cards rely on it)
       ls (readdir) shows the long names
       a file written to /home reads back the same after a remount
       a file with data past 100 MB and one past 200 MB (triple indirect
       zone, ACO.5 in part) read back; the second is removed
  4. QEMU is stopped; on the host 'fsck.minix -3 -f' finds /home clean
     (the triple indirect tree of the first, nothing leaked by the second)
     and 'fsck.minix -l' lists the long names.
  5. fsck for V3 (spec O.6): on the host, /home is damaged: a wrong link
     count, a lost file (its entry cleared) and an out-of-range zone in a
     third file, and CLEAN (offset 14) cleared.  Booted again, /etc/rc's
     'fsck -p' repairs all three (its log, /usr/adm/fsck.log) and sets
     CLEAN; the host 'fsck.minix -f' is clean afterwards; at the next boot
     (CLEAN cleared again) fsck finds nothing.
Exit status: the number of failures.
"""
import importlib.util
import os
import re
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
	"powercut", os.path.join(HERE, "powercut.py"))
powercut = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(powercut)

N57 = "galtzagorriak_etxeko_atea_irekitzeko_urruneko_agintea.sub"
N60 = "x" * 56 + ".sub"
N61 = "y" * 57 + ".sub"
fails = 0
T0 = time.time()


def check(ok, what):
	global fails
	print(("  ok   " if ok else "  FAIL ") + what + "  [%.0f s]"
		% (time.time() - T0), flush=True)
	if not ok:
		fails += 1


def part4(img):
	with open(img, "rb") as f:
		mbr = f.read(512)
	e = 0x1BE + 16 * 3
	start, size = struct.unpack_from("<II", mbr, e + 8)
	return (start, size) if mbr[e + 4] == 0x81 and size else (None, None)


def copy_part(img, start, size, part, to_img):
	with open(img, "r+b") as f, open(part, "r+b") as p:
		left = size * 512
		f.seek(start * 512)
		p.seek(0)
		while left:
			n = min(left, 1 << 20)
			if to_img:
				f.write(p.read(n))
			else:
				p.write(f.read(n))
			left -= n


def host(cmd):
	p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
	return p.returncode, p.stdout.decode(errors="replace")


class V3:
	"""Just enough of a MINIX V3 image on the host to damage it."""

	def __init__(self, path):
		self.f = open(path, "r+b")
		sb = self.blk(1)
		(self.ninodes, _, self.imap, self.zmap, self.first, _, _,
			_, self.zones) = struct.unpack_from("<IHHHHHHII", sb, 0)
		self.ilist = 2 + self.imap + self.zmap

	def blk(self, n):
		self.f.seek(n * 1024)
		return self.f.read(1024)

	def inode(self, ino):
		off = self.ilist * 1024 + (ino - 1) * 64
		self.f.seek(off)
		return off, bytearray(self.f.read(64))

	def put(self, off, data):
		self.f.seek(off)
		self.f.write(data)

	def lookup(self, name):
		"""(byte offset of the entry, inode) of name in the root."""
		_, ip = self.inode(1)
		for z in struct.unpack_from("<7I", ip, 24):
			if z == 0:
				continue
			b = self.blk(z)
			for e in range(0, 1024, 64):
				ino = struct.unpack_from("<I", b, e)[0]
				n = b[e + 4:e + 64].split(b"\0")[0]
				if ino and n == name.encode():
					return z * 1024 + e, ino
		return None, None

	def flags(self):
		return struct.unpack_from("<H", self.blk(1), 14)[0]

	def close(self):
		self.f.close()


def damage(part):
	"""A wrong link count, a lost file, an out-of-range zone."""
	v = V3(part)
	try:
		_, ino = v.lookup("links")
		off, ip = v.inode(ino)
		struct.pack_into("<H", ip, 2, 4)		# nlinks 1 -> 4
		v.put(off, ip)
		eoff, ino = v.lookup(N57)
		v.put(eoff, b"\0\0\0\0")			# the entry: lost
		_, ino = v.lookup("badz")
		off, ip = v.inode(ino)
		struct.pack_into("<I", ip, 24, v.zones + 100)	# zone 0
		v.put(off, ip)
	finally:
		v.close()


def cksum(r, path):
	out = r.output("cksum < %s" % path, 60)
	return out.split()[0] if out and out.split() else None


def main():
	r = powercut.Run(0, 0)
	keep = r.dir
	img = os.path.join(keep, "build/qemu/minix/disk.img")
	part = tempfile.mktemp(prefix="v3home")
	try:
		if not r.login():
			check(False, "first boot and login")
			return 1
		check(r.sh("sync", 60) == 0, "sync")
		r.p.kill()
		r.p.wait()
		start, size = part4(img)
		check(start is not None, "/home is hd4 (type 81), %s sectors"
			% size)
		if start is None:
			return fails
		with open(part, "wb") as p:
			p.truncate(size * 512)
		st, out = host(["mkfs.minix", "-3", "-i", "200000", part])
		check(st == 0, "host mkfs.minix -3: " + out.strip().replace(
			"\n", " | "))
		copy_part(img, start, size, part, True)

		r = powercut.Run(0, 0, reuse=keep)
		if not r.login():
			check(False, "login with a V3 /home")
			return fails + 1
		r.sh("cd /; umount /dev/hd4", 60)
		check(r.sh("mount /dev/hd4 /home", 60) == 0,
			"mount the V3 /dev/hd4 on /home read-write")
		check(r.sh("cp /bin/sh /home/%s && cp /etc/rc /home/%s && sync"
			% (N57, N60), 120) == 0, "cp to a 57- and a 60-character name")
		out = r.output("ls /home", 30) or b""
		check(N57.encode() in out and N60.encode() in out,
			"ls shows both names")
		a, b = cksum(r, "/bin/sh"), cksum(r, "/home/" + N57)
		check(a is not None and a == b, "same file read back (%s, %s)"
			% (a, b))
		out = r.output("cp /etc/rc /home/%s; echo st=$?" % N61, 30) or b""
		check(b"too long" in out and b"st=0" not in out,
			"61 characters: %s" % out.strip().decode(errors="replace"))
		check(r.sh("cp /etc/rc /usr/tmp/abcdefghijklmno && "
			"test -f /usr/tmp/abcdefghijklmn && "
			"rm /usr/tmp/abcdefghijklmn", 30) == 0,
			"15 characters on V2 /usr: cut at 14, as before")
		# One command per line: the console's line is short.
		d = "/home/" + N57[:50]
		ok = r.sh("mkdir " + d, 30) == 0
		ok = ok and r.sh("mv /home/%s %s" % (N57, d), 30) == 0
		ok = ok and r.sh("test -f %s/%s" % (d, N57), 30) == 0
		ok = ok and r.sh("cd %s && mv %s /home" % (d, N57), 30) == 0
		ok = ok and r.sh("cd / && rmdir " + d, 30) == 0
		check(ok, "mkdir, mv into and out of, rmdir a 50-character "
			"directory")
		check(r.sh("cd / && umount /dev/hd4 && mount /dev/hd4 /home", 60)
			== 0, "remount /home")
		check(cksum(r, "/home/" + N57) == a, "the file again after mount")
		check(r.sh("rm /home/" + N60, 30) == 0, "rm the 60-character name")
		# ACO.5 in part: blocks past 100 MB go through the triple
		# indirect zone (sparse: the copy is QEMU-slow otherwise).
		a = cksum(r, "/etc/rc")
		check(r.sh("dd if=/etc/rc of=/home/big bs=1k seek=102400 "
			"2>/dev/null && dd if=/etc/rc of=/home/big2 bs=1k "
			"seek=204800 2>/dev/null && sync", 120) == 0,
			"write past 100 MB and past 200 MB (triple indirect)")
		out = (r.output("ls -l /home/big", 30) or b"") + b" " + \
			(r.output("ls -l /home/big2", 30) or b"")
		check(b" 104861" in out and b" 209719" in out, "sizes: %s" % out.decode(
			errors="replace").replace("\n", " | "))
		b = r.output("dd if=/home/big bs=1k "
			"skip=102400 2>/dev/null | cksum", 60)
		check(b is not None and b.split()[0] == a,
			"the data past 100 MB reads back the same")
		out = r.output("dd if=/home/big bs=1k skip=70000 count=1 "
			"2>/dev/null | od -c | head -2", 60) or b""
		check(out.startswith(b"0000000  \\0  \\0"),
			"a hole in the triple range reads as zeros")
		check(r.sh("rm /home/big2", 60) == 0, "rm the 200 MB file")
		check(r.sh("cp /etc/rc /home/links && echo x >/home/badz",
			60) == 0, "two more files for the fsck test")
		check(r.sh("sync; cd / && umount /dev/hd4", 60) == 0,
			"sync, umount /home")
		r.p.kill()
		r.p.wait()

		copy_part(img, start, size, part, False)
		st, out = host(["fsck.minix", "-f", part])
		check(st == 0, "host fsck.minix -f: clean (%s)"
			% out.strip().replace("\n", " | "))
		st, out = host(["fsck.minix", "-f", "-l", part])
		check(N57 in out and N60 not in out,
			"host fsck.minix -l lists the 57-character name, not the "
			"removed one")

		# 5. fsck for V3 (spec O.6).
		damage(part)
		copy_part(img, start, size, part, True)
		st, out = host(["fsck.minix", "-f", part])
		check(st != 0, "host fsck.minix sees the damage (status %d)"
			% st)
		v = V3(part)
		v.put(1024 + 14, b"\0\0")	# not CLEAN: /etc/rc checks it
		v.close()
		copy_part(img, start, size, part, True)
		r = powercut.Run(0, 0, reuse=keep)
		if not r.login():
			check(False, "login with the damaged V3 /home")
			return fails + 1
		log = r.output("cat /usr/adm/fsck.log", 60) or b""
		print(log.decode(errors="replace"))
		check(b"hd4: unclean" in log, "/etc/rc's fsck -p checked /dev/hd4")
		check(b": adjust" in log, "the wrong link count was repaired")
		check(b": remove" in log, "the file with the bad zone was removed")
		check(b"new map" in log and b": clear" in log,
			"the lost file's inode and zones were freed")
		check(r.sh("test ! -f /etc/.fsck-errors", 30) == 0,
			"no errors left")
		check(r.sh("test -f /home/links && test ! -f /home/badz && "
			"test ! -f /home/" + N57, 60) == 0,
			"/home: links kept, badz and the lost file gone")
		check(r.sh("echo new >/home/after && sync; cd / && "
			"umount /dev/hd4", 60) == 0, "write to /home, umount")
		r.p.kill()
		r.p.wait()
		copy_part(img, start, size, part, False)
		v = V3(part)
		check(v.flags() & 3 == 1, "CLEAN at offset 14 of the V3 super "
			"block, CHECK clear (%#x)" % v.flags())
		v.put(1024 + 14, b"\0\0")	# checked again at the next boot
		v.close()
		st, out = host(["fsck.minix", "-f", part])
		check(st == 0, "host fsck.minix -f after our fsck: clean (%s)"
			% out.strip().replace("\n", " | "))
		copy_part(img, start, size, part, True)
		r = powercut.Run(0, 0, reuse=keep)
		if not r.login():
			check(False, "login after the repair")
			return fails + 1
		out = r.output("tail -1 /usr/adm/fsck.log", 60) or b""
		check(b"fixed 0, left 0" in out, "fsck again: nothing to repair "
			"(%s)" % out.strip().decode(errors="replace"))
		return fails
	finally:
		r.close()
		if os.path.exists(part):
			os.unlink(part)


if __name__ == "__main__":
	sys.exit(main())
