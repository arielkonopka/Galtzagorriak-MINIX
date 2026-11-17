#!/usr/bin/env python3
"""swaptest - plain swap to the SD card (spec Phase 12, R12.28) in QEMU.

Usage: swaptest.py [-n CYCLES]

Boots a fresh copy of build/qemu/minix/disk.img (make minix disk, BOARD
qemu) like tools/powercut.py, through the first boot (which makes the swap
partition /dev/hd3, type 82), logs in as root and checks:

  AC12.11  swapon refuses what is not an SD card partition of type 82
           (/dev/hd1 the root file system, /dev/rom, /dev/ram) and crypt
           (12b is not there yet)
  AC12.5, AC12.6, AC12.13, R12.6, AC12.1
           /usr/tests/swapper (see tests/swapper.c): image and gap after
           swap-in, a wait waking a swapped parent, N kills without
           swap-in, a damaged slot killed by the CRC, Secret: yes and
           set-uid images never swapped, programs started past the free
           RAM
  AC12.1   the login shell is swapped while a big program runs and keeps
           its variables
  R12.5 b  the idle reserve swaps out processes idle for 10 s
  R12.8    free -v shows the mode, the slots and the swapped processes
  AC12.12  "card pulled" (swapon -X, the test hook) with two programs
           swapped: both reported lost, the shell goes on
  rule 0.2 0 internal flash pages erased by all of the above (flash erase
           counters before and after; the run-time text cache is off, the
           test programs are installed (pkg flash) before the count)
Exit status: the number of failures.
"""
import argparse
import importlib.util
import os
import re
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


def flash_counts(r):
	out = r.output("free -v | grep '^flash'", 30)
	m = out and re.search(rb"cache (\d+) pages erased .*installed (\d+)",
				out)
	return (int(m.group(1)), int(m.group(2))) if m else None


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("-n", type=int, default=100, help="kill cycles")
	a = ap.parse_args()
	r = powercut.Run(0, 0)
	try:
		if not r.login():
			check(False, "login")
			return 1
		check(r.sh("pkg install /lib/pkg/tests_1.0.tar", 120) == 0,
			"install the tests package")
		# Only text in flash is swapped (R12.3): install the test
		# programs (install-time flash writes, before the count).
		check(r.sh("pkg budget 0 && cp /usr/tests/swapper /tmp/swplain "
			"&& cp /usr/tests/swapper /tmp/swsuid && chmod 4755 "
			"/tmp/swsuid && sync && pkg flash /usr/tests/swapper && "
			"pkg flash /usr/tests/swapsecret && pkg flash /tmp/swplain "
			"&& pkg flash /tmp/swsuid", 600) == 0,
			"run-time cache off, test programs installed in flash")
		before = flash_counts(r)

		# AC12.11 / R12.1 amended
		for dev in ("/dev/hd1", "/dev/rom", "/dev/ram", "/dev/hd0"):
			check(r.sh("swapon %s" % dev, 30) not in (0, None),
				"AC12.11: swapon %s refused" % dev)
		out = r.output("swapon /dev/hd3 crypt; echo st=$?", 30)
		check(out is not None and b"st=1" in out
			and b"not there yet" in out,
			"R12.28: swap=crypt refused with a message")
		check(r.sh("su bin -c 'swapon /dev/hd3'", 30) not in (0, None),
			"R12.1: only root turns swap on")
		check(r.sh("swapon /dev/hd3 plain && swapon -r 0 && free -t on",
			30) == 0, "swapon /dev/hd3 plain (reserve 0 for now)")
		out = r.output("free -v | grep '^swap'", 30)
		check(out is not None and b"swap: plain, 0 of 16 slots" in out,
			"R12.8: free -v shows the mode and the slot use")

		# tests/swapper (AC12.5, R12.6, AC12.6, AC12.13, AC12.1)
		mark = len(r.out)
		st = r.sh("/usr/tests/swapper -n %d" % a.n, 3600)
		sys.stdout.write(r.out[mark:].decode(errors="replace")
			.replace("\r", ""))
		check(st == 0, "tests/swapper (AC12.5, AC12.6, AC12.13, AC12.1)")
		check(b"CRC mismatch" in r.out[mark:],
			"AC12.13: the damaged slot was reported (CRC mismatch)")

		# AC12.1 with the shell: it waits for a big program, is swapped
		# out to make room and keeps its variables.
		mark = len(r.out)
		out = r.output("X=kaixo; /usr/tests/swapper -f; echo X=$X", 600)
		check(out is not None and b"X=kaixo" in out,
			"AC12.1: the shell keeps its variables")
		check(re.search(rb"swap: out (sh|-sh) ", r.out[mark:]) is not None
			and re.search(rb"swap: in  (sh|-sh) ", r.out[mark:])
			is not None, "AC12.1: the shell was swapped out and in")

		# R12.5 b: the reserve.  All of RAM as the reserve (192 KB):
		# idle processes (update, getty, ...) go after 10 s.
		mark = len(r.out)
		r.sh("swapon -r 192", 30)
		r.wait(rb"\0never\0", 40)
		out = r.output("swapon; free -v | grep '^swap'", 30)
		sys.stdout.write((out or b"").decode(errors="replace"))
		check(re.search(rb"swap: out ", r.out[mark:]) is not None,
			"R12.5 b: the idle reserve swapped out idle processes")
		check(out is not None and b"  swapped: pid" in out,
			"R12.8: free -v lists the swapped processes")
		r.sh("swapon -r 0", 30)

		# AC12.12: two programs swapped, the card "pulled"
		mark = len(r.out)
		r.sh("/usr/tests/swapper -p & p1=$!; /usr/tests/swapper -p & "
			"p2=$!; sleep 2; swapon -o $p1; swapon -o $p2", 60)
		r.sh("swapon -X; sleep 2", 60)
		lost = re.findall(rb"swap: swapper +\(pid \d+\) lost: SD card",
					r.out[mark:])
		check(len(lost) == 2, "AC12.12: both swapped programs reported "
			"lost (%d)" % len(lost))
		check(r.sh("echo still here", 30) == 0,
			"AC12.12: nothing else crashed, the shell answers")
		out = r.output("swapon", 30)
		check(out is not None and b"lost" in out,
			"AC12.12: swapon shows the card as lost")

		after = flash_counts(r)
		print("  flash erase counts (cache, installed): before %s, "
			"after %s" % (before, after))
		check(before is not None and before == after,
			"rule 0.2: swap erased 0 internal flash pages")
		bad = powercut.BAD.search(r.out)
		check(bad is None and b"panic" not in r.out,
			"console: no panic")
	except (OSError, ValueError) as e:
		check(False, "the system stopped (%s)" % e)
	finally:
		r.close()
	if fails:
		sys.stdout.write(r.out[-4000:].decode(errors="replace"))
	print("PASS" if fails == 0 else "FAIL", "plain swap (%d failures)" % fails)
	return fails


if __name__ == "__main__":
	sys.exit(main())
