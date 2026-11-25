#!/usr/bin/env python3
"""timedtest - timers and alerts (spec F.2, R7b.3, R7b.4) in QEMU.

Usage: timedtest.py

Boots a fresh copy of build/qemu/minix/disk.img (make minix disk, BOARD
qemu, the real INIT) like tools/powercut.py, logs in as root, installs
the timed package and checks:

  R7b.3    INIT gets SIGRTC (kill -7 1) and runs /usr/lib/timed, which
           fires a due notify timer: /usr/lib/alert logs it on the console
  R7b.4    a once timer is removed after it fired; /var/timers/.last is
           written; QEMU has no RTC alarm, so timed logs the time it would
           arm; /var/timers is root:timers 1770
  AC7b.1.3 do-not-disturb around now holds a reminder back, not an alarm
  AC7b.1.4 a once timer two hours in the past: Missed: skip does not
           fire, fire-once does (timed -n, the test time override)
  F.2      Action: run starts the program as the file's owner with
           --timer ID, or says it cannot start it (#253); a file that is
           not mode 600 is ignored
  F.2      a timer added by the shell is listed by the format timed reads

QEMU's clock is the PC's (semihosting), in UTC: the times below come from
this script's clock.  Exit status: the number of failures.
"""
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


def check(ok, what):
	global fails
	print(("  ok   " if ok else "  FAIL ") + what, flush=True)
	if not ok:
		fails += 1


def timer(r, tid, lines, mode="600"):
	"""Write /var/timers/tid from lines (short shell lines: the console
	takes 256 bytes a line)."""
	r.sh("rm -f /var/timers/%s" % tid, 20)
	for i, l in enumerate(lines):
		r.sh("echo '%s' %s /var/timers/%s" % (l, ">" if i == 0 else ">>",
			tid), 20)
	return r.sh("chmod %s /var/timers/%s" % (mode, tid), 20) == 0


def stop(r, out, tid):
	"""End the alert timed -v started for tid ("tid: fired, pid N"): with
	no key it would hold its RAM for its ring time (an alarm 10 min), and
	the next step's alert or program might find no memory hole."""
	m = re.search(rb"timed: %s: fired, pid (\d+)" % tid.encode(),
		out or b"")
	if m: r.sh("kill %s" % m.group(1).decode(), 20)


def hm(t):
	return time.strftime("%H:%M", time.gmtime(t))


def main():
	r = powercut.Run(0, 0)
	try:
		if not r.login():
			check(False, "login")
			return 1
		check(r.sh("pkg install /lib/pkg/timed_1.3.tar", 120) == 0,
			"install the timed package")
		out = r.output("ls -ld /var/timers", 20)
		check(out is not None and re.search(rb"drwxrwx--[^-x ] .*root +timers",
			out) is not None, "/var/timers is root:timers 1770")
		if fails: print(out)

		# Action: run, as the owner (bin); a file of mode 644 ignored.
		# First, before any alert: an alert (the steps below fire three)
		# stays in RAM for its ring time with no key to end it (an alarm
		# 10 min, a reminder 1), and with them up /bin/id finds no
		# memory hole (exec: EAGAIN), which timed only said as "fired"
		# (#253).  Now it says "cannot start it", as for a program
		# that is not there (norun), and the steps below end their
		# alerts once checked (stop()).
		now = int(time.time())
		timer(r, "runme", ["Kind: once", "At: %d" % now, "Action: run",
			"Program: /bin/id"])
		r.sh("chown bin /var/timers/runme", 20)
		timer(r, "norun", ["Kind: once", "At: %d" % now, "Action: run",
			"Program: /bin/nosuch"])
		r.sh("chown bin /var/timers/norun", 20)
		timer(r, "loose", ["Kind: once", "At: %d" % now], "644")
		r.sh("echo %d > /var/timers/.last" % (now - 10), 20)
		out = r.output("/usr/lib/timed -v -n %d; sleep 2" % (now + 1), 30)
		check(out is not None and re.search(rb"uid=2\(bin\)", out)
			is not None, "Action: run: the program runs as the owner")
		check(out is not None and b"runme: fired, pid" in out
			and b"norun: cannot start it: " in out
			and b"norun: fired" not in out,
			"Action: run: a program that cannot start is said so")
		check(out is not None and b"loose: not a plain file of mode 600"
			in out, "a timer file not of mode 600 is ignored")
		if out is not None and fails: print(out.decode(errors="replace"))
		# #272: a timer that cannot start is kept, tried again after
		# 60 s, up to 10 times, then dropped; a later start works.
		check(out is not None and re.search(
			rb"norun: cannot start it: [^\r\n]*\(try 1/10\)", out)
			is not None, "#272: the first failure says try 1/10")
		t0 = now + 1
		check(r.sh("grep '^Try: 1$' /var/timers/norun >/dev/null", 20) == 0
			and r.sh("grep '^Retry: %d$' /var/timers/norun >/dev/null"
				% (t0 + 60), 20) == 0,
			"#272: the file keeps the timer with Try: 1 and Retry: +60 s")
		out = r.output("ls -l /var/timers/norun", 20)
		check(out is not None and re.search(rb"-rw-------.* bin ", out)
			is not None, "#272: still mode 600 and owned by bin")
		out = r.output("/usr/lib/timed -v -n %d" % (t0 + 30), 30)
		check(out is not None and b"cannot start it" not in out,
			"#272: no try before 60 s")
		for k in range(2, 11):
			out = r.output("/usr/lib/timed -v -n %d" % (t0 + 60 * (k - 1)),
									30)
			check(out is not None and (b"(try %d/10)" % k) in out,
				"#272: try %d/10 one minute later" % k)
		check(out is not None and b"norun: gave up after 10 tries" in out
			and r.sh("test -f /var/timers/norun", 20) == 1,
			"#272: after 10 tries it is dropped")
		t1 = t0 + 600
		timer(r, "later", ["Kind: once", "At: %d" % (t1 - 1),
			"Action: run", "Program: /bin/nolater"])
		r.sh("chown bin /var/timers/later", 20)
		out = r.output("/usr/lib/timed -v -n %d" % t1, 30)
		check(out is not None and b"later: cannot start it" in out
			and r.sh("test -f /var/timers/later", 20) == 0,
			"#272: a missing program keeps a once timer")
		r.sh("cp /bin/id /bin/nolater", 30)
		out = r.output("/usr/lib/timed -v -n %d; sleep 2" % (t1 + 60), 30)
		check(out is not None and b"later: fired, pid" in out
			and r.sh("test -f /var/timers/later", 20) == 1,
			"#272: the retry starts it and removes the timer")
		r.sh("rm -f /bin/nolater /var/timers/norun /var/timers/later", 20)
		# R7b.3 below is timed's first run (no .last) again.
		r.sh("rm -f /var/timers/loose /var/timers/.last", 20)

		# R7b.3: SIGRTC to INIT -> timed -> alert.
		now = int(time.time())
		timer(r, "kaixo", ["Kind: once", "At: %d" % (now - 2),
			"Category: reminder", "Missed: skip", "Title: Kaixo",
			"Text: R7b.3 test"])
		mark = len(r.out)
		r.sh("kill -7 1", 20)
		# timed's alert may come before the shell's @@0@@ of kill: then
		# r.sh() has already read past it, so look from mark first
		pat = rb"alert: \d\d:\d\d reminder: Kaixo: R7b.3 test"
		m = re.search(pat, r.out[mark:]) or r.wait(pat, 30)
		check(m is not None and r.out.find(b"alert:", mark) >= 0,
			"kill -7 1: INIT ran timed, which fired the due timer")
		time.sleep(1)
		r.wait(rb"@@nothing@@", 2)
		check(r.out.find(b"timed: no RTC alarm", mark) >= 0,
			"no RTC alarm in QEMU: timed says so")
		time.sleep(1)
		check(r.sh("test -f /var/timers/kaixo", 20) == 1,
			"the once timer is gone after it fired")
		check(r.sh("test -f /var/timers/.last", 20) == 0,
			"/var/timers/.last written")

		# AC7b.1.3: do-not-disturb from an hour ago to an hour ahead.
		now = int(time.time())
		r.sh("cp /etc/notify.conf /tmp/nc", 20)
		r.sh("echo 'Dnd: %s-%s' >> /etc/notify.conf" % (hm(now - 3600),
			hm(now + 3600)), 20)
		timer(r, "rem", ["Kind: once", "At: %d" % now,
			"Category: reminder", "Title: Rem", "Text: quiet"])
		timer(r, "alm", ["Kind: once", "At: %d" % now,
			"Category: alarm", "Title: Alm", "Text: loud"])
		r.sh("echo %d > /var/timers/.last" % (now - 10), 20)
		out = r.output("/usr/lib/timed -v -n %d; sleep 1" % (now + 1), 30)
		check(out is not None and b"rem: held (do not disturb)" in out,
			"AC7b.1.3: a reminder is held back in do-not-disturb")
		check(re.search(rb"alert: \d\d:\d\d alarm: Alm: loud", r.out)
			is not None, "AC7b.1.3: an alarm rings in do-not-disturb")
		stop(r, out, "alm")
		r.sh("cp /tmp/nc /etc/notify.conf", 20)

		# AC7b.1.4: two hours late (the clock moved, or it was off).
		now = int(time.time())
		for tid, missed in (("skip", "skip"), ("once", "fire-once")):
			timer(r, tid, ["Kind: once", "At: %d" % (now - 7200),
				"Category: reminder", "Missed: %s" % missed,
				"Title: M-%s" % tid, "Text: late"])
		r.sh("echo %d > /var/timers/.last" % (now - 7300), 20)
		mark = len(r.out)
		out = r.output("/usr/lib/timed -v -n %d; sleep 1" % now, 30)
		check(out is not None and b"skip: missed, skipped" in out,
			"AC7b.1.4: Missed: skip does not fire")
		check(re.search(rb"alert: \d\d:\d\d reminder: M-once: late",
			r.out[mark:]) is not None,
			"AC7b.1.4: Missed: fire-once fires once")
		stop(r, out, "once")

		# The next time: an interval timer every minute, all day.
		timer(r, "tick", ["Kind: interval", "Start: 00:00", "End: 24:00",
			"Every: 1", "Category: info", "Title: Tick", "Text: t"])
		now = int(time.time())
		out = r.output("/usr/lib/timed -v -n %d" % now, 30)
		want = (now // 60 + 1) * 60
		check(out is not None and ("next at %d" % want).encode() in out,
			"interval: the next time is the next minute")
		# RAM (chmem): MM reports what timed and alert never touched.
		mark = len(r.out)
		r.sh("free -t on", 20)
		r.output("/usr/lib/timed -n %d; /usr/lib/alert -f 0 T x & "
			"sleep 2; kill $!; sleep 1; free -t off" % now, 60)
		for m in re.finditer(rb"memuse: (timed|alert) .*", r.out[mark:]):
			print("  " + m.group(0).decode().strip())
		print(r.out.decode(errors="replace").replace("\r", "")[-3000:]
			if fails else "", end="")
	finally:
		r.close()
	print("%d failed" % fails)
	return fails


if __name__ == "__main__":
	sys.exit(main())
