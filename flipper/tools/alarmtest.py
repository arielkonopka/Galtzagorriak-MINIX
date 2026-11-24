#!/usr/bin/env python3
"""alarmtest - alarm() under load in QEMU (issue #308, MM panic "alarm er").

Usage: alarmtest.py [-r ROUNDS] [-s SECS]

Boots a fresh copy of build/qemu/minix/disk.img (make minix disk, BOARD
qemu) like tools/powercut.py, through the first boot, logs in as root,
installs the tests package and runs /usr/tests/alarmrace (tests/alarmrace.c)
ROUNDS times (default 3).  Each round copies it to a new file and runs it
three times: the third exec queues the copy to the flash cache, which
waits for an idle system, so MM's synchronous alarm comes every tick
while alarmrace's children keep the CPU busy: alarm() in a loop (MM ->
CLOCK SET_ALARM), poll() with a 1 ms timeout (FS's synchronous alarm)
and stime() 10 times a second (SIGRTC to INIT).  The third run lasts
SECS seconds (default 30).

Before the fix two synchronous alarms in one clock pass made CLOCK
block-send to SYN_AL, which was blocked sending to MM; MM's next
sendrec(CLOCK) failed (ELOCKED) and MM panicked.  PASS: every round
prints PASS, the console shows no panic, and the shell still answers.
Exit status: the number of failures.
"""
import argparse
import importlib.util
import os
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
	global fails
	print(("  ok   " if ok else "  FAIL ") + what + "  [%.0f s]"
		% (time.time() - T0), flush=True)
	if not ok:
		fails += 1


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("-r", type=int, default=3, help="rounds")
	ap.add_argument("-s", type=int, default=30,
		help="seconds of the long run in each round")
	a = ap.parse_args()
	r = powercut.Run(0, 0)
	try:
		if not r.login():
			check(False, "login")
			return 1
		check(r.sh("pkg install /lib/pkg/tests_1.0.tar", 120) == 0,
			"install the tests package")
		for k in range(a.r):
			f = "/tmp/ar%d" % k
			check(r.sh("cp /usr/tests/alarmrace %s && sync" % f, 60)
				== 0, "round %d: a new copy, %s" % (k + 1, f))
			mark = len(r.out)
			for secs in (2, 2, a.s):
				st = r.sh("%s -s %d" % (f, secs), secs + 120)
				if st != 0:
					break
			sys.stdout.write(r.out[mark:].decode(errors="replace")
				.replace("\r", ""))
			check(st == 0 and b"panic" not in r.out,
				"round %d: alarmrace 2 s, 2 s, %d s" % (k + 1, a.s))
			if st != 0:
				break
		check(b"panic" not in r.out, "console: no panic")
		check(r.sh("echo still here", 30) == 0, "the shell answers")
	except (OSError, ValueError) as e:
		check(False, "the system stopped (%s)" % e)
	finally:
		r.close()
	if fails:
		sys.stdout.write(r.out[-3000:].decode(errors="replace"))
	print("PASS" if fails == 0 else "FAIL", "alarm() under load "
		"(%d failures)" % fails)
	return fails


if __name__ == "__main__":
	sys.exit(main())
