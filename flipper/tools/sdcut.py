#!/usr/bin/env python3
"""sdcut - the SD card cut off without a sync (spec N.1-N.3, T17a), in QEMU.

Usage: sdcut.py [ACN.1|ACN.2|ACN.4|PWR|APPS ...]

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
  APPS   (#282) killed during pkg remove usbrem (3 times spread over
         it), and during its prerm run with sh -x, at CUTS of its
         commands (the cut when the command's trace line comes), with a
         "while sync" loop beside it, so the card holds what the cache
         had, as after an unlucky update: after the reboot /etc/apps is
         the old list or the new one, never empty or cut short.  A
         label with a "\" in it survives the prerm.

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


CUTS = 10
TEK = b"Teklatua:usbremote:@usb:=/usr/bin/usbremote\n"
SLASH = b"Back\\slash:nobody:@app:/bin/echo\n"
PRERM = "/usr/var/pkg/info/usbrem.prerm"
NOMEM = b"no memory to start it"


def install(r):
	"""pkg install usbrem; again after a pause if its postinst found no
	memory (#271: right after a boot, rc's programs may still run)."""
	for _ in range(3):
		if r.sh("pkg install /lib/pkg/usbrem_1.0.tar >/dev/null && sync",
			120) == 0:
			return True
		time.sleep(5)
	print("  APPS: pkg install usbrem fails")
	return False


def apps_cut(r, n):
	"""Power cut n of APPS; returns the cut's description, or None when
	the run could not be set up."""
	if not install(r):
		return None
	mark = len(r.out)
	if n[0] == "time":		# pkg remove itself, cut after n[1] s
		r.send("echo @@G''O@@; pkg remove usbrem")
		if r.wait(rb"@@GO@@", 30) is None:
			print("  APPS: pkg remove usbrem did not start")
			return None
		time.sleep(n[1])
		where = "%.2f s into pkg remove usbrem" % n[1]
	else:				# the prerm, cut at its n[1]th command
		r.send("while sync; do :; done & echo @@G''O@@; sh -x %s; "
			"echo @@DO''NE@@" % PRERM)
		if r.wait(rb"@@GO@@", 30) is None:
			print("  APPS: the prerm did not start")
			return None
		for _ in range(n[1]):
			if r.wait(rb"\n\+", 60) is None:
				break
		where = "at the prerm's command %d" % n[1]
		if NOMEM in r.out[mark:]:	# #271: once more
			return "again"
	r.restart()			# the power cut
	if not r.login():
		print("  APPS: no login after the cut %s" % where)
		return None
	return where


def apps(r):
	"""#282: /etc/apps is the old list or the new one after a power cut
	in pkg remove usbrem."""
	if r.sh("echo '%s' >>/etc/apps && sync" % SLASH.decode().strip(),
		60) != 0:
		print("  APPS: could not add the label with a backslash")
		return False
	if not install(r):
		return False
	old = r.output("cat /etc/apps")
	new = (old or b"").replace(TEK, b"")
	if TEK not in old or SLASH not in old:
		print("  APPS: /etc/apps after the install: %r" % old)
		return False
	# No cut: the prerm's commands with sh -x, and pkg remove's time.
	mark = len(r.out)
	if r.sh("sh -x %s 2>&1 | grep -c '^[+]'" % PRERM, 120) != 0:
		print("  APPS: the prerm fails")
		return False
	m = re.search(rb"\n(\d+)\r?\n", r.out[mark:])
	cmds = int(m.group(1)) if m else 0
	if r.output("cat /etc/apps") != new:
		print("  APPS: /etc/apps after the prerm: %r" %
			r.output("cat /etc/apps"))
		return False
	if not install(r):
		return False
	t0 = time.time()
	if r.sh("pkg remove usbrem", 120) != 0 or \
	   r.output("cat /etc/apps") != new:
		print("  APPS: pkg remove usbrem: /etc/apps %r" %
			r.output("cat /etc/apps"))
		return False
	t = time.time() - t0
	print("  APPS: the prerm runs %d commands, pkg remove usbrem %.1f s"
		% (cmds, t))
	if cmds < 10:
		return False
	cuts = [("time", t * k / 4) for k in range(1, 4)]
	cuts += [("cmd", k) for k in range(2, cmds, max(1, cmds // CUTS))]
	cuts.append(("cmd", cmds))		# its last: mv done
	ok = True
	for n in cuts:
		for _ in range(3):
			where = apps_cut(r, n)
			if where != "again":
				break
		if where == "again":
			print("  APPS: no memory to start the prerm, 3 times")
		if where in (None, "again"):
			return False
		got = r.output("cat /etc/apps")
		which = "old" if got == old else "new" if got == new else None
		inside = r.sh("test -f /etc/apps.new", 30) == 0
		print("  APPS: cut %s: /etc/apps is %s%s" % (where,
			which or repr(got),
			" (/etc/apps.new left)" if inside else ""))
		if which is None:
			ok = False
	return ok


TESTS = {"ACN.1": acn1, "ACN.2": acn2, "ACN.4": acn4, "PWR": pwr,
	 "APPS": apps}


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
