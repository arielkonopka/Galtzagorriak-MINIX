#!/usr/bin/env python3
"""swaptest - plain swap to the SD card (spec Phase 12, R12.28) in QEMU.

Usage: swaptest.py [-n CYCLES] [--hot SECONDS]

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
  AC12.1   a shell is swapped on demand while big programs start, and
           keeps its variables (a copy of sh with its text in flash,
           /tmp/swsh: QEMU's own programs run their text from RAM, which
           is never swapped, R12.3; on the Flipper they are in flash).
           The launcher part needs parking (M4): today it waits in FS,
           not in MM, so it is not swapped.
  AC12.10  placement: tests/swapper -c 50 (two parked apps, a foreground
           app placed low)
  R12.5 b  the idle reserve swaps out a process idle for 10 s
  AC12.14, AC12.13 (12g)
           /usr/tests/swapwear (see tests/swapwear.c): a swap-out of an
           unchanged image writes 0 sectors, one changed heap byte writes
           the header and one chunk; a byte flipped in a skipped chunk
           kills on swap-in
  AC12.15  /usr/tests/swapwear -r SECONDS (--hot, default 600 as the
           spec): a parked app with a 30 s timer is not swapped out by the
           reserve again once its timer brought it back; on demand it is
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
	ap.add_argument("--hot", type=int, default=600,
		help="AC12.15: seconds to watch the woken app")
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
		# (short lines: the console's line buffer takes 256 bytes)
		check(r.sh("pkg budget 0 && cp /usr/tests/swapper /tmp/swplain "
			"&& cp /usr/tests/swapper /tmp/swsuid && chmod 4755 "
			"/tmp/swsuid && cp /bin/sh /tmp/swsh && sync", 120) == 0
			and r.sh("pkg flash /usr/tests/swapper && "
			"pkg flash /usr/tests/swapsecret && "
			"pkg flash /usr/tests/swapsmall", 300) == 0
			and r.sh("pkg flash /tmp/swplain && pkg flash /tmp/swsuid "
			"&& pkg flash /tmp/swsh", 300) == 0
			and r.sh("pkg flash /usr/tests/swapwear", 300) == 0,
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
		out = r.output("/tmp/swsh -c 'X=kaixo; /usr/tests/swapper -f; "
			"echo X=$X'", 600)
		sys.stdout.write((out or b"").decode(errors="replace"))
		check(out is not None and b"X=kaixo" in out
			and b"FAIL" not in out,
			"AC12.1: the shell keeps its variables")
		check(re.search(rb"swap: out swsh ", r.out[mark:]) is not None
			and re.search(rb"swap: in  swsh ", r.out[mark:])
			is not None, "AC12.1: the shell was swapped out and in")

		# AC12.10: placement
		out = r.output("/usr/tests/swapper -c 50; echo st=$?", 1200)
		sys.stdout.write((out or b"").decode(errors="replace"))
		check(out is not None and b"st=0" in out,
			"AC12.10: placement, 50 cycles")

		# R12.5 b: the reserve.  All of RAM as the reserve (192 KB):
		# an idle process with its text in flash goes after 10 s.
		mark = len(r.out)
		r.sh("/usr/tests/swapper -p & rp=$!; swapon -r 192", 30)
		r.wait(rb"\0never\0", 30)
		out = r.output("swapon; free -v | grep '^swap'", 30)
		sys.stdout.write((out or b"").decode(errors="replace"))
		check(re.search(rb"swap: out swapper ", r.out[mark:]) is not None,
			"R12.5 b: the idle reserve swapped out an idle process")
		check(out is not None and b"  swapped: pid" in out,
			"R12.8: free -v lists the swapped processes")
		r.sh("swapon -r 0", 30)
		out = r.output("kill -10 $rp; wait $rp; echo st=$?", 60)  # USR1
		check(out is not None and b"st=0" in out,
			"R12.5 b: it comes back intact on a signal")

		# 12g: AC12.14 (and AC12.13 for a skipped chunk), AC12.15
		mark = len(r.out)
		st = r.sh("/usr/tests/swapwear", 600)
		sys.stdout.write(r.out[mark:].decode(errors="replace")
			.replace("\r", ""))
		check(st == 0, "tests/swapwear: only changed chunks written "
			"(AC12.14, AC12.13)")
		mark = len(r.out)
		st = r.sh("/usr/tests/swapwear -r %d" % a.hot, a.hot + 600)
		sys.stdout.write(r.out[mark:].decode(errors="replace")
			.replace("\r", ""))
		check(st == 0, "tests/swapwear -r %d: the reserve leaves a "
			"woken app alone (AC12.15)" % a.hot)

		# AC12.12: two programs swapped, the card "pulled"
		mark = len(r.out)
		r.sh("/usr/tests/swapper -p & p1=$!; sleep 2; swapon -o $p1; "
			"/usr/tests/swapper -p & p2=$!; sleep 2; swapon -o $p2",
			60)
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
		# "damaged" is expected: the CRC test's console message and
		# its PASS/FAIL line
		bad = powercut.BAD.search(re.sub(
			rb"lost: swap image damaged|(PASS|FAIL) damaged slot"
			rb"|(PASS|FAIL) a byte flipped", b"",
			r.out))
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
