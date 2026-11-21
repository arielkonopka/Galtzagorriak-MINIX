#!/usr/bin/env python3
"""blktest - swappable memory blocks for apps (spec 12h) in QEMU.

Usage: blktest.py [-g]

Boots a fresh copy of build/qemu/minix/disk.img (make minix disk, BOARD
qemu) like tools/swaptest.py, through the first boot (swap partition
/dev/hd3), logs in as root and runs /usr/tests/blktest (tests/blktest.c):

  AC12.22  blocks off (swap on without blk): ENOSYS
  AC12.18  EBADF, zeros, ERANGE, ENOSPC, BLK_ANY, runs that do not overlap,
           first fit after a run ends, PUT/GET of 4 KB and of 1 KB, a
           byte flipped on the card: EIO for that sector only
  AC12.18  not root: at most half of the block space
  AC12.19  zeros after the writer exits and after exec of another
           program; a forked child has no reservation
  AC12.20  sectors written: a whole-block PUT 9, BLK_IFCHANGED 0, exit 0
  AC12.22  a Secret: yes image and a set-uid copy: EPERM
  R12.38   <blk.h> (libc) and libblk: 256 KB through 1 KB frames
  R12.38   blk=on in /etc/rc.conf: /etc/rc switches blocks on at boot
  R12.38   blocks only (swapon /dev/hd3 blk, swap off) work too
  R12.40   MMSWAPON discards the whole space at idle (QEMU zero-fills)
  AC12.21  a background caller waits while keys arrive (typed here), a
           foreground app is answered at once; background PUTs stop at
           the write budget; the card pulled (DIOCFAKECARD): a
           foreground GET is ENXIO, a background one waits for the card

With -g, cards >= 32 GB (R12.34) instead: after the first boot the swap
partition hd3 becomes a 0x81 one with the subpartitions hd3a, hd3b and
hd3d of type 82 (hd3c is not swap), and a 60 GiB hole of the QEMU HAL in
front of it (a sparse 64 GB card, as tools/bigcard.py).  Booted again,
/etc/rc (swap=plain, blk=on, /etc/.swap4) swaps on hd3a, and blktest -g: 64 extents per subpartition,
runs on hd3a, hd3b and hd3d, a block on hd3d read back.
Exit status: the number of failures.
"""
import importlib.util
import os
import re
import signal
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
	"powercut", os.path.join(HERE, "powercut.py"))
powercut = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(powercut)

fails = 0
T0 = time.time()


def check(ok, what):
	sys.stdout.flush()
	global fails
	print(("  ok   " if ok else "  FAIL ") + what + "  [%.0f s]"
		% (time.time() - T0), flush=True)
	if not ok:
		fails += 1


def tests(r, cmd, what, timeout=120):
	print("  ...  " + cmd, flush=True)
	out = r.output(cmd + "; echo st=$?", timeout) or b""
	for line in out.splitlines():
		if line.startswith(b"PUT") or line.startswith(b"GET"):
			print("       " + line.decode("latin-1"), flush=True)
		if line.startswith(b"PASS") or line.startswith(b"FAIL"):
			print("       " + line.decode("latin-1"), flush=True)
	check(b"st=0" in out and b"FAIL" not in out, what)

def keys(r):
	"""AC12.21: blktest -k, keys typed for 3 s after it says KEYS."""
	print("  ...  /usr/tests/blktest -k (keys for 3 s)", flush=True)
	start = len(r.out)
	r.send("/usr/tests/blktest -k; echo st=$?")
	ok = r.wait(rb"KEYS\r?\n", 60) is not None
	end = time.time() + 3
	while ok and time.time() < end:
		r.p.stdin.write(b"k")
		r.p.stdin.flush()
		time.sleep(0.1)
	r.send("")
	ok = ok and r.wait(rb"st=\d+", 120) is not None
	out = r.out[start:]
	for line in out.splitlines():
		if re.match(rb"(PASS|FAIL|keys ended|foreground)", line):
			print("       " + line.decode("latin-1"), flush=True)
	check(ok and b"st=0" in out and b"FAIL" not in out,
		"AC12.21: background caller while keys arrive")


BIG_HOLE = 60 * 2 * 1024 * 1024		# 60 GiB in sectors
SUB = 32760				# sectors per subpartition


def make_big(img):
	"""hd3 (swap) -> 0x81 with hd3a, hd3b, hd3d of type 82, past 60 GiB."""
	with open(img, "r+b") as f:
		mbr = bytearray(f.read(512))
		e = 0x1BE + 16 * 2
		start, size = struct.unpack_from("<II", mbr, e + 8)
		if mbr[e + 4] != 0x82 or size < 8 + 4 * SUB:
			return None
		mbr[0x1A8:0x1B8] = b"GZ-HOLE!" + struct.pack("<II", start,
								BIG_HOLE)
		for i in range(4):
			o = 0x1BE + 16 * i
			s0, n = struct.unpack_from("<II", mbr, o + 8)
			if n and s0 >= start:
				struct.pack_into("<I", mbr, o + 8, s0 + BIG_HOLE)
		mbr[e + 4] = 0x81
		f.seek(0)
		f.write(mbr)
		sub = bytearray(512)
		base = start + BIG_HOLE
		for i in range(4):
			o = 0x1BE + 16 * i
			sub[o + 4] = 0x82 if i != 2 else 0x83
			struct.pack_into("<II", sub, o + 8, base + 8 + i * SUB, SUB)
		sub[510:512] = b"\x55\xaa"
		f.seek(start * 512)
		f.write(sub)
	return start + BIG_HOLE


def main_big():
	r = powercut.Run(0, 0)
	keep = r.dir
	try:
		if not r.login():
			check(False, "first boot and login")
			return 1
		check(r.sh("pkg install /lib/pkg/tests_1.0.tar && echo swap=plain"
			" >>/etc/rc.conf && echo blk=on >>/etc/rc.conf && "
			">/etc/.swap4 && sync", 120) == 0,
			"install the tests package; swap=plain, blk=on, "
			"/etc/.swap4 (as /etc/setup on a card >= 32 GB)")
		r.p.kill()
		r.p.wait()
		start = make_big(os.path.join(keep, "build/qemu/minix/disk.img"))
		check(start is not None, "hd3 (type 81, hd3a-hd3d) at sector %s"
			% start)
		if start is None:
			return fails
		r = powercut.Run(0, 0, reuse=keep)
		if not r.login():
			check(False, "login on the 64 GB card")
			return fails + 1
		out = r.output("mkpart -l /dev/hd0", 30) or b""
		check(re.search(rb"^3 +type 81 +start +%d " % start, out, re.M)
			is not None, "mkpart -l: %s" % out.strip().decode(
				errors="replace").replace("\r\n", " | "))
		out = r.output("ls -l /dev/hd3a /dev/hd3b /dev/hd3c /dev/hd3d; "
			"echo st=$?", 30) or b""
		check(b"st=0" in out and b"3, 139" in out,
			"/dev/hd3a-hd3d in the root file system")
		out = r.output("swapon", 30) or b""
		print(out.decode(errors="replace"))
		check(b"device 3/136" in out and b"swap: plain" in out,
			"R12.34: /etc/rc swaps on /dev/hd3a (/etc/.swap4)")
		r.sh("sleep 3", 30)	# the DISCARD at MMSWAPON first
		tests(r, "/usr/tests/blktest -g",
			"R12.34: blocks over hd3a, hd3b, hd3d", 300)
		out = r.output("swapon off; swapon /dev/hd3b plain; echo st=$?",
			30) or b""
		check(b"st=0" not in out, "swapon /dev/hd3b is refused")
	finally:
		log = os.environ.get("BLKTEST_LOG")
		if log:
			with open(log, "wb") as f:
				f.write(r.out)
		r.close()
	print("blktest -g: %d failures" % fails)
	return fails



def main():
	r = powercut.Run(0, 0)
	signal.signal(signal.SIGTERM, lambda *a: sys.exit(99))
	try:
		if not r.login():
			check(False, "login")
			return 1
		check(r.sh("pkg install /lib/pkg/tests_1.0.tar", 120) == 0,
			"install the tests package")
		check(r.sh("cp /usr/tests/blktest /tmp/blk2 && cp "
			"/usr/tests/blktest /tmp/blksuid && chmod 4755 "
			"/tmp/blksuid && sync", 60) == 0, "copies of blktest")
		check(r.sh("swapon /dev/hd3 plain && swapon -r 0", 30) == 0,
			"swapon /dev/hd3 plain (no blocks)")
		tests(r, "/usr/tests/blktest -n", "AC12.22: blk=off gives ENOSYS")
		check(r.sh("swapon off && swapon /dev/hd3 plain blk && "
			"swapon -r 0", 30) == 0, "swapon /dev/hd3 plain blk")
		tests(r, "/usr/tests/blktest", "AC12.18, AC12.19 as root", 300)
		tests(r, "/usr/tests/blktest -c",
			"AC12.20: sectors written (PUT 9, IFCHANGED 0, exit 0)")
		tests(r, "su bin -c '/usr/tests/blktest -h'",
			"AC12.18: not root, half of the space")
		tests(r, "/usr/tests/blksecret -p",
			"AC12.22: Secret: yes image gets EPERM")
		tests(r, "su bin -c '/tmp/blksuid -p'",
			"AC12.22: set-uid image gets EPERM")
		tests(r, "/usr/tests/blktest -l", "R12.38: <blk.h>, libblk", 300)
		check(r.sh("swapon off && /usr/tests/blktest -d && "
			"swapon /dev/hd3 blk", 30) == 0,
			"R12.38: blocks only, swap off (marks on the card)")
		r.sh("sleep 3", 30)
		tests(r, "/usr/tests/blktest -D",
			"R12.40: the DISCARD of the whole space at MMSWAPON")
		out = r.output("swapon", 30) or b""
		check(b"swap: off" in out, "R12.38: ... swap stays off")
		tests(r, "/usr/tests/blktest", "AC12.18, AC12.19 blocks only",
			300)
		# R12.38: blk=on in the card's /etc/rc.conf (swap=off there):
		# after a reboot /etc/rc has switched blocks on by itself.
		# (QEMU has no rc.conf; a card's own one is kept in
		# /etc/rc.conf.blk and put back at the end)
		check(r.sh("swapon off && { test ! -f /etc/rc.conf || cp "
			"/etc/rc.conf /etc/rc.conf.blk; } && echo swap=off "
			">>/etc/rc.conf && echo blk=on >>/etc/rc.conf && sync",
			30) == 0,
			"R12.38: blk=on in /etc/rc.conf")
		r.send("reboot")
		if not r.login():
			check(False, "login after the reboot")
			return 1
		out = r.output("swapon", 30) or b""
		check(b"swap: off" in out, "R12.38: blk=on, swap=off: swap off")
		tests(r, "/usr/tests/blktest -l",
			"R12.38: blk=on: /etc/rc switched blocks on", 300)
		# AC12.21 last: the write budget is spent for a minute after it
		keys(r)
		tests(r, "/usr/tests/blktest -o",
			"AC12.21: write budget; the card pulled", 120)
		check(r.sh("if test -f /etc/rc.conf.blk; then mv /etc/rc.conf.blk "
			"/etc/rc.conf; else rm /etc/rc.conf; fi && sync", 30) == 0,
			"the default rc.conf again")
	finally:
		log = os.environ.get("BLKTEST_LOG")
		if log:
			with open(log, "wb") as f:
				f.write(r.out)
		r.close()
	print("blktest: %d failures" % fails)
	return fails


if __name__ == "__main__":
	sys.exit(main_big() if sys.argv[1:] == ["-g"] else main())
