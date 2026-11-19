#!/usr/bin/env python3
"""v3card - a MINIX V3 /home in QEMU (spec O.4, ACO.1 and ACO.2 in part).

Usage: v3card.py

  1. A fresh copy of build/qemu/minix/disk.img (make minix disk, BOARD
     qemu) boots once: the first boot makes /usr, swap and /home.
  2. QEMU is stopped.  On the host, util-linux 'mkfs.minix -3' makes a V3
     file system of /home's size with 200 000 inodes (past the old
     16-bit ino_t, O.4d) and it replaces /home (/dev/hd4).
  3. Booted again, as root (until fsck learns V3, /etc/rc mounts it
     read-only; the test mounts it read-write by hand):
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
		return fails
	finally:
		r.close()
		if os.path.exists(part):
			os.unlink(part)


if __name__ == "__main__":
	sys.exit(main())
