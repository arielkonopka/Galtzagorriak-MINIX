#!/usr/bin/env python3
"""fsdamage - a damaged file system on the card must not halt the system
(security review 2026-11-16, F1-F3), in QEMU.

Usage: fsdamage.py

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

Needs BOARD=qemu built (make minix disk).  Exit status 0: pass.
"""
import os
import re
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


if __name__ == "__main__":
	sys.exit(main())
