#!/usr/bin/env python3
"""blktest - swappable memory blocks for apps (spec 12h) in QEMU.

Usage: blktest.py

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
  AC12.22  a Secret: yes image and a set-uid copy: EPERM
  R12.38   blocks only (swapon /dev/hd3 blk, swap off) work too
Exit status: the number of failures.
"""
import importlib.util
import os
import re
import signal
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
		if line.startswith(b"PASS") or line.startswith(b"FAIL"):
			print("       " + line.decode("latin-1"), flush=True)
	check(b"st=0" in out and b"FAIL" not in out, what)


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
		tests(r, "su bin -c '/usr/tests/blktest -h'",
			"AC12.18: not root, half of the space")
		tests(r, "/usr/tests/blksecret -p",
			"AC12.22: Secret: yes image gets EPERM")
		tests(r, "su bin -c '/tmp/blksuid -p'",
			"AC12.22: set-uid image gets EPERM")
		check(r.sh("swapon off && swapon /dev/hd3 blk", 30) == 0,
			"R12.38: blocks only, swap off")
		out = r.output("swapon", 30) or b""
		check(b"swap: off" in out, "R12.38: ... swap stays off")
		tests(r, "/usr/tests/blktest", "AC12.18, AC12.19 blocks only",
			300)
	finally:
		log = os.environ.get("BLKTEST_LOG")
		if log:
			with open(log, "wb") as f:
				f.write(r.out)
		r.close()
	print("blktest: %d failures" % fails)
	return fails


if __name__ == "__main__":
	sys.exit(main())
