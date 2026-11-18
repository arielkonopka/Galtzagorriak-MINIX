#!/usr/bin/env python3
"""sdcut - the SD card cut off without a sync (spec N.1-N.3, T17a), in QEMU.

Usage: sdcut.py [ACN.1|ACN.2|ACN.4|PWR ...]

Boots a fresh copy of build/qemu/minix/disk.img (the first boot makes
/usr and /home, spec /etc/setup) and kills QEMU, which for the card is a
power cut or the card pulled out: nothing is synced.  Then boots the
same card again and looks at what /etc/rc's fsck -p -v prints.

  ACN.1  killed while cp writes /home: the next boot checks /home
         ("fsck /dev/hd4: unclean"), repairs it, a file synced before
         the kill is intact; after a halt the next boot checks nothing.
  ACN.2  killed 90 s after the last write: the next boot checks nothing
         (the file systems were marked clean when idle, N.2).
  ACN.4  the root damaged (an inode's link count) and not clean: fsck
         exits 2, rc restarts at once, the second boot is clean.
  PWR    (ACN.7 without a battery) kill -16 1, INIT's SIGPWR: an orderly
         halt (N.5); the next boot checks nothing.

Needs BOARD=qemu built (make minix disk).  Exit status 0: pass.
"""
import os
import re
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from powercut import Run, QEMU, MB, CFG	# noqa: E402

BAD = re.compile(rb"panic|Unrecoverable|Lost block")
CHECKED = re.compile(rb"fsck (/dev/hd\d): (\w+)")
DONE = re.compile(rb"fixed (\d+), left (\d+)")


class Card(Run):
	"""A Run whose card (disk.img in its own directory) survives the
	power cut: restart() boots it again."""

	def restart(self):
		self.p.kill()
		self.p.wait()
		cmd = QEMU + [
		    "-device", "loader,file=%s,addr=0" %
				os.path.abspath(os.path.join(MB, "minix.bin")),
		    "-device", "loader,addr=0x%x,data=0,data-len=4" % CFG,
		    "-device", "loader,addr=0x%x,data=0,data-len=4" % (CFG + 4)]
		self.p = subprocess.Popen(cmd, cwd=self.dir,
			stdin=subprocess.PIPE, stdout=subprocess.PIPE,
			stderr=subprocess.STDOUT)
		self.out = b""
		self.seen = 0

	def image(self):
		return os.path.join(self.dir, "build/qemu/minix/disk.img")

	def boot(self):
		"""Log in; what /etc/rc's fsck printed: [(dev, state)]."""
		ok = self.login()
		return ok, CHECKED.findall(self.out)


def fresh():
	"""A card past its first boot (setup), cleanly halted."""
	r = Card(0, 0)
	if not r.login():
		r.close()
		return None
	r.sh("sync", 60)
	return r


def acn1(r):
	ok = True
	if r.sh("cp /bin/sh /home/kept && sync && cksum /home/kept "
		">/home/kept.sum && sync", 120) != 0:
		print("  ACN.1: could not write /home/kept")
		return False
	want = r.output("cksum /home/kept")
	# a big copy to /home, killed in the middle
	r.send("for i in 1 2 3 4 5 6 7 8; do cp /bin/fsck /home/c$i; "
		"cp /bin/sh /home/d$i; done")
	time.sleep(4)
	r.restart()				# the power cut
	ok1, seen = r.boot()
	print("  ACN.1 boot after the cut: %s" % seen)
	if not ok1:
		print("  ACN.1: no login after the cut")
		return False
	if (b"/dev/hd4", b"unclean") not in seen:
		print("  ACN.1: /home was not checked")
		ok = False
	if not DONE.search(r.out):
		print("  ACN.1: no fsck summary")
		ok = False
	got = r.output("cksum /home/kept")
	if want is None or got != want:
		print("  ACN.1: /home/kept changed: %r %r" % (want, got))
		ok = False
	halt(r)					# an orderly halt this time
	r.restart()
	ok2, seen = r.boot()
	print("  ACN.1 boot after a halt: %s" % seen)
	if not ok2:
		print("  ACN.1: no login at the second boot")
		ok = False
	if seen:
		print("  ACN.1: checked again after a clean halt")
		ok = False
	return ok


def halt(r, cmd="halt"):
	"""Power off the orderly way; QEMU stops or sits in ship mode."""
	r.send(cmd)
	r.wait(rb"\0never\0", 8)		# the halt's sync, the servers' last words


def pwr(r):
	"""N.5 (ACN.7 in QEMU): SIGPWR to INIT (low battery) halts in order:
	the next boot checks nothing, a file written just before is there."""
	ok = True
	if r.sh("cp /bin/sh /home/pwr && cp /bin/sh /usr/pwr", 120) != 0:
		print("  PWR: could not write")
		return False
	want = r.output("cksum /home/pwr")
	halt(r, "kill -16 1")
	if b"battery low: powering off" not in r.out:
		print("  PWR: INIT did not say it powers off")
		ok = False
	r.restart()
	ok1, seen = r.boot()
	print("  PWR boot after SIGPWR: %s" % seen)
	if not ok1 or seen:
		print("  PWR: no login, or a file system was checked")
		ok = False
	if r.output("cksum /home/pwr") != want:
		print("  PWR: /home/pwr is not what was written")
		ok = False
	return ok


def acn2(r):
	ok = True
	if r.sh("cp /bin/sh /home/idle && cp /bin/sh /tmp/idle", 120) != 0:
		print("  ACN.2: could not write")
		return False
	time.sleep(95)				# update syncs every 30 s
	r.restart()
	ok1, seen = r.boot()
	print("  ACN.2 boot 90 s after the last write: %s" % seen)
	if not ok1:
		print("  ACN.2: no login")
		ok = False
	if seen:
		print("  ACN.2: a file system was checked")
		ok = False
	return ok


def acn4(r):
	"""Damage the root on the card while QEMU is off: the link count of
	/etc/motd's inode wrong, CLEAN off."""
	ok = True
	if r.sh("sync", 60) != 0:
		return False
	m = r.output("ls -id /etc/motd")
	ino = int(m.split()[0]) if m and m.split() and m.split()[0].isdigit() \
		else None
	r.p.kill()
	r.p.wait()
	if ino is None:
		print("  ACN.4: no inode number of /etc/motd")
		return False
	img = r.image()
	with open(img, "r+b") as f:
		f.seek(446 + 8)			# partition 1's first sector
		base = struct.unpack("<I", f.read(4))[0] * 512
		f.seek(base + 1024)
		sb = f.read(24)
		imap, zmap = struct.unpack_from("<hh", sb, 4)
		ioff = base + (2 + imap + zmap) * 1024 + (ino - 1) * 64
		f.seek(ioff + 2)		# d2_nlinks
		f.write(struct.pack("<H", 5))
		f.seek(base + 1024 + 18)	# s_flags: unclean
		f.write(struct.pack("<H", 0))
	r.restart()
	ok1, seen = r.boot()
	print("  ACN.4 boot with the root damaged: %s %s" % (seen,
		DONE.findall(r.out)))
	if (b"/dev/hd1", b"unclean") not in seen:
		print("  ACN.4: the root was not checked")
		ok = False
	k = r.out.find(b"root file system was repaired: restarting")
	if k < 0:
		print("  ACN.4: rc did not restart after the root repair")
		return False
	if not ok1:
		print("  ACN.4: no login after the restart")
		ok = False
	again = CHECKED.findall(r.out, k)
	print("  ACN.4 boot after rc's restart: %s" % again)
	if again:
		print("  ACN.4: checked again after the restart")
		ok = False
	if r.output("ls -l /etc/motd", 60) is None:
		ok = False
	return ok


TESTS = {"ACN.1": acn1, "ACN.2": acn2, "ACN.4": acn4, "PWR": pwr}


def main():
	names = sys.argv[1:] or list(TESTS)
	ok = True
	for n in names:
		print(n, flush=True)
		r = fresh()
		if r is None:
			print("FAIL %s: no first login" % n)
			ok = False
			continue
		try:
			good = TESTS[n](r)
		except (OSError, ValueError) as e:
			print("  %s: QEMU stopped (%s)" % (n, e))
			good = False
		if BAD.search(r.out):
			print("  %s: %s" % (n, BAD.search(r.out).group(0).decode()))
			good = False
		if not good:
			sys.stdout.write(r.out[-2500:].decode(errors="replace"))
		print("%s %s" % ("PASS" if good else "FAIL", n), flush=True)
		ok = ok and good
		r.close()
	return 0 if ok else 1


if __name__ == "__main__":
	sys.exit(main())
