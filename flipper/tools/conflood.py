#!/usr/bin/env python3
"""conflood - a console input flood must not freeze the system (security
review 2026-11-16, K1), in QEMU.

Usage: conflood.py [SECONDS]

Boots build/qemu/minix (as tools/powercut.py does, with no power cut),
logs in, starts a background loop that prints MARK every second, and then
sends the console a flood of bytes for SECONDS (default 10) as fast as the
UART takes them, the way a USB host or a device on the header pins can.
With echo off (only the user processes' output is measured), then with
echo on.  Passes when, during each flood, the background loop still
prints at least half of its marks (the TTY task leaves the CPU to the
processes) and the shell answers after it.  Before the fix the TTY task
read the flood for as long as it lasted: no process ran, and on the
Flipper the watchdog reset the board after 2 s.

Needs BOARD=qemu built (make minix disk).  Exit status 0: pass.
"""
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from powercut import Run	# noqa: E402


def flood(r, secs):
	"""Send junk for secs seconds; returns the number of marks seen."""
	stop = time.time() + secs
	chunk = b"x" * 256

	def writer():
		try:
			while time.time() < stop:
				r.p.stdin.write(chunk)
				r.p.stdin.flush()
		except (BrokenPipeError, OSError):
			pass
	t = threading.Thread(target=writer, daemon=True)
	before = r.out.count(b"MARK")
	t0 = time.time()
	t.start()
	while time.time() < stop:
		r.wait(rb"\0never\0", 0.5)		# just collect output
	marks = r.out.count(b"MARK") - before
	t.join(5)
	print("  %.0f s of flood: %d mark(s)" % (time.time() - t0, marks),
		flush=True)
	return marks


def main():
	secs = int(sys.argv[1]) if len(sys.argv) > 1 else 10
	r = Run(0, 0)
	ok = True
	try:
		if not r.login():
			print("FAIL no login")
			return 1
		r.send("(while :; do echo MARK; sleep 1; done) & echo @@$?@@")
		if r.wait(rb"@@0@@", 30) is None:
			print("FAIL background loop")
			return 1
		for echo in ("-echo", "echo"):
			if r.sh("stty " + echo, 30) != 0:
				print("FAIL stty " + echo)
				return 1
			print("flood with stty %s:" % echo, flush=True)
			marks = flood(r, secs)
			# the line discipline holds a line of x: drop it
			r.send("\x15")
			time.sleep(1)
			t0 = time.time()
			alive = r.sh("echo alive", 60) == 0
			print("  shell answered: %s (%.1f s)" %
				("yes" if alive else "no", time.time() - t0))
			if marks < secs // 2 or not alive:
				ok = False
		r.sh("kill %1 2>/dev/null; kill 0 2>/dev/null; true", 10)
	finally:
		r.close()
	print("PASS" if ok else "FAIL", "console flood")
	return 0 if ok else 1


if __name__ == "__main__":
	sys.exit(main())
