#!/usr/bin/env python3
"""bigcard - partitions past 4 GiB (spec R12.39, O.3) in QEMU.

Usage: bigcard.py

The QEMU card is a semihosted image whose positions are 32-bit, so it
cannot pass 4 GiB itself.  The QEMU HAL can put a hole into the card
(hal/qemu/board.c: "GZ-HOLE!" in the MBR): the card grows by the hole and
the image's sectors after it follow the hole.

  1. A fresh copy of build/qemu/minix/disk.img (make minix disk, BOARD
     qemu) boots once: the first boot makes /usr, swap and /home.
  2. QEMU is stopped.  On the host, a 4 GiB hole goes in front of /home
     (/dev/hd4) and its MBR entry moves by 4 GiB: the same sectors of the
     image are now a partition starting past 4 GiB.
  3. Booted again, as root:
       mkpart -l shows /home's start past 4 GiB
       /home is mounted from /dev/hd4 (df)
       prwtest /dev/hd4 mounted: DIOCGETPS, DIOCGETP EOVERFLOW, bread()
         equals read(), bwrite() refused (EBADF, EBUSY)
       a file written to /home reads back the same (cksum)
       umount, prwtest /dev/hd4 (bwrite round trip at its last
         sector, past the end EINVAL), mount again
       after a reboot /etc/rc's fsck finds no error and the file is
       still the same (a full fsck after login has no room in QEMU's
       RAM next to the running system: "no memory to start it")
Exit status: the number of failures.
"""
import importlib.util
import os
import re
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
	"powercut", os.path.join(HERE, "powercut.py"))
powercut = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(powercut)

HOLE = 0x800000			# 4 GiB in sectors
fails = 0
T0 = time.time()


def check(ok, what):
	global fails
	print(("  ok   " if ok else "  FAIL ") + what + "  [%.0f s]"
		% (time.time() - T0), flush=True)
	if not ok:
		fails += 1


def add_hole(img):
	"""Hole in front of partition 4, which moves past 4 GiB."""
	with open(img, "r+b") as f:
		mbr = bytearray(f.read(512))
		e = 0x1BE + 16 * 3
		start, size = struct.unpack_from("<II", mbr, e + 8)
		if mbr[e + 4] != 0x81 or size == 0:
			return None
		mbr[0x1A8:0x1B8] = b"GZ-HOLE!" + struct.pack("<II", start, HOLE)
		struct.pack_into("<I", mbr, e + 8, start + HOLE)
		f.seek(0)
		f.write(mbr)
	return start + HOLE


def cksum(r, path):
	out = r.output("cksum < %s" % path, 60)
	return out.split()[0] if out and out.split() else None


def main():
	r = powercut.Run(0, 0)
	keep = r.dir
	try:
		if not r.login():
			check(False, "first boot and login")
			return 1
		check(r.sh("pkg install /lib/pkg/tests_1.0.tar", 120) == 0,
			"install the tests package")
		check(r.sh("sync", 60) == 0, "sync")
		r.p.kill()
		r.p.wait()
		start = add_hole(os.path.join(keep, "build/qemu/minix/disk.img"))
		check(start is not None, "/home (hd4, type 81) moved to sector %s"
			% start)
		if start is None:
			return fails

		r = powercut.Run(0, 0, reuse=keep)
		if not r.login():
			check(False, "login with /home past 4 GiB")
			return fails + 1
		out = r.output("mkpart -l /dev/hd0", 30) or b""
		check(re.search(rb"^4 +type 81 +start +%d " % start, out, re.M)
			is not None, "mkpart -l: %s" % out.strip().decode(
				errors="replace").replace("\r\n", " | "))
		out = r.output("df", 30) or b""
		check(re.search(rb"/dev/hd4\b.*/home", out) is not None,
			"/home mounted from /dev/hd4")
		out = r.output("/usr/tests/prwtest /dev/hd4 mounted; echo st=$?",
			60) or b""
		print(out.decode(errors="replace"))
		check(b"starts past 4 GiB" in out and b"st=0" in out,
			"prwtest /dev/hd4 mounted")
		check(r.sh("cp /bin/sh /home/big && cp /usr/tests/prwtest "
			"/home/p2 && sync", 120) == 0, "files written to /home")
		a, b = cksum(r, "/bin/sh"), cksum(r, "/home/big")
		check(a is not None and a == b, "same file read back (%s, %s)"
			% (a, b))
		check(r.sh("cd / && umount /dev/hd4", 60) == 0, "umount /home")
		out = r.output("/usr/tests/prwtest /dev/hd4; echo st=$?",
			60) or b""
		print(out.decode(errors="replace"))
		check(b"st=0" in out, "prwtest /dev/hd4 (not mounted)")
		check(r.sh("mount /dev/hd4 /home", 60) == 0, "mount /home again")
		check(cksum(r, "/home/big") == a, "the file again after mount")
		r.send("reboot")
		if not r.login():
			check(False, "login after the reboot")
			return fails
		check(b"/dev/hd4 has errors" not in r.out
			and b"cannot mount /dev/hd4" not in r.out,
			"the boot's fsck of /dev/hd4 found nothing, /home mounted")
		check(cksum(r, "/home/big") == a, "the file after a reboot")
		return fails
	finally:
		r.close()


if __name__ == "__main__":
	sys.exit(main())
