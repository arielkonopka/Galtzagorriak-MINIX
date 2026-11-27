#!/usr/bin/env python3
"""pkgload - pkg install under load, many boots (#271), in QEMU.

Usage: pkgload.py [-n BOOTS] [-j JOBS] [--no-load]

Each boot is a fresh copy of build/qemu/minix/disk.img (make minix disk,
BOARD qemu, the real INIT) like tools/powercut.py.  It logs in as root,
starts a background shell that runs small programs without a pause (they
take and give back memory holes all the time), and installs the timed,
remind and alarm packages from /lib/pkg, as tools/nattest.py does.  The
runs go JOBS at a time (default: twice the CPUs), so the host is loaded
as well.

#271: pkg install returned 0 while its postinst never ran (sh's exec
failed with EAGAIN, no memory hole, in pkg's vfork child, unseen).  A
boot fails here when an install returns 0 but what its postinst makes is
missing (timed: /var/timers 1770; remind, alarm: their users and launcher
lines).  An install that returns non-zero must say why ("pkg: ..."); it
counts as "refused", not as a failure.  Exit status 1 on any failure.
"""
import argparse
import importlib.util
import os
import re
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
	"powercut", os.path.join(HERE, "powercut.py"))
powercut = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(powercut)

LOAD = ("(while test ! -f /tmp/stop; do cat /etc/passwd >/dev/null; "
	"ls / >/dev/null; od /etc/rc >/dev/null; done) &")
PKGS = ("timed_1.7", "remind_1.1", "alarm_1.6")
# What each postinst makes: a command that prints 1 if it is there.
# timed's makes nothing a fresh card lacks: the line it prints.
SAYS = {"timed_1.7": rb"timed: timers in /var/timers"}
MADE = {
	"remind_1.1": "grep -c '^remind:.:21:11:' /etc/passwd",
	"alarm_1.6": "grep -c '^Ordua:alarm:@alarm:=/usr/bin/alarm$' /etc/apps",
}


def boot(i, load):
	"""One boot: (i, failures, refused, note)."""
	r = powercut.Run(0, 0)
	try:
		if not r.login():
			return i, 1, 0, "login"
		if load:
			r.send(LOAD)
		fails, refused, notes = 0, 0, []
		for p in PKGS:
			mark = len(r.out)
			st = r.sh("pkg install /lib/pkg/%s.tar 2>&1" % p, 180)
			said = r.out[mark:]
			if st is None:
				fails += 1
				notes.append("%s: no answer" % p)
				continue
			if st != 0:
				m = re.search(rb"pkg: [^\r\n]*", said)
				if m is None:
					fails += 1
					notes.append("%s: exit %d without a "
						     "reason (said %r)" % (p, st,
						     said[-200:]))
				else:
					refused += 1
					notes.append("%s: exit %d: %s" % (p, st,
						m.group(0).decode(errors="replace")))
				continue
			if p in SAYS:
				ok = re.search(SAYS[p], said) is not None
			else:
				out = r.output(MADE[p], 60)
				ok = out is not None and out.split()[-1:] == [b"1"]
			if not ok:
				fails += 1
				notes.append("%s: exit 0, but its postinst did not "
					     "run (#271)" % p)
		r.send("touch /tmp/stop")
		return i, fails, refused, "; ".join(notes)
	finally:
		r.close()


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("-n", type=int, default=32)
	ap.add_argument("-j", type=int, default=2 * (os.cpu_count() or 2))
	ap.add_argument("--no-load", action="store_true")
	a = ap.parse_args()
	bad = refused = 0
	with ThreadPoolExecutor(a.j) as ex:
		for i, f, rf, note in ex.map(lambda i: boot(i, not a.no_load),
					     range(a.n)):
			bad += f > 0
			refused += rf
			print("%s boot %d%s" % ("FAIL" if f else "ok  ", i,
				(": " + note) if note else ""))
			sys.stdout.flush()
	print("%d of %d boots failed; %d installs refused with a reason"
	      % (bad, a.n, refused))
	sys.exit(1 if bad else 0)


if __name__ == "__main__":
	main()
