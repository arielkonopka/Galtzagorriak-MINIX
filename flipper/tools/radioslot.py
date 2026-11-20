#!/usr/bin/env python3
"""radioslot - R2b.1a in QEMU (task T30a): `pkg flash` keeps out of the
radio slot.  Needs `make minix disk` (the QEMU image, normal INIT).

QEMU's flash layout (hal/qemu/board.c) has its installed region at 0x48000;
its first free page is 0x52000 (two directory pages and /dev/rom before
it).  With /etc/radio.slot = 0x08052000 nothing fits below the slot: the
install must fail and name the slot; a slot higher up leaves room; without
the file the install works as before.  `radio` prints the layout with the
slot, `radio list` the (empty) card.

Part 2, `radioslot.py sfsa`: QEMU with a fake SFSA per boot
(hal/qemu/board.c FAKE_CFG; a reset in the same QEMU keeps its flash,
as a CPU2 stack swap would): boot 1 today's layout, a program installed at
0x52000; boot 2 SFSA 0x83000 (programs end at 0x53000): the record is
past the end, MM marks it dead at boot, nothing is installed, the system
runs; boot 3 today's layout again: the record stays dead.  Then QEMU with
SFSA 0x70000 (no room below the guard): neither region, `pkg flash`
refuses, the system runs.
"""
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
	"powercut", os.path.join(HERE, "powercut.py"))
powercut = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(powercut)


FAKE_CFG = 0x000CFFD0


def fake(sfsas):
	"""QEMU loader words: one SFSA per boot.  Not the boot counter
	(FAKE_CFG[3], zero at QEMU's start): the loader writes its words
	again at every reset."""
	a = []
	for i, v in enumerate(sfsas):
		a += ["-device", "loader,addr=0x%x,data=%d,data-len=4"
			% (FAKE_CFG + 4 * i, v)]
	return a


def installed(r):
	"""The number of installed programs (free) and the CPU2 line."""
	out = r.output("free -v | grep installed")
	m = re.search(rb"installed programs: (\d+).*CPU2 flash from (\w+)",
		out or b"")
	return (int(m.group(1)), m.group(2).decode()) if m else (None, out)


def reboot(r):
	r.send("sync; reboot")
	return r.login()


def sfsa_main():
	bad = []
	r = powercut.Run(0, 0, extra=fake([0, 0x83000, 0]))
	try:
		if not r.login():
			print("radioslot sfsa: no login")
			return 1
		n0, c = installed(r)
		if n0 is None:
			bad.append("free: %r" % c)
			return sfsa_end(bad)
		r.sh("cp /bin/od /tmp/p; sync")
		out = r.output("pkg flash /tmp/p 2>&1; echo rc=$?", 120)
		n1, c = installed(r)
		if out is None or b"rc=0" not in out or n1 != n0 + 1:
			bad.append("boot 1: install: %r, %r -> %r" % (out, n0, n1))
		if not reboot(r):
			bad.append("boot 2: no login")
			return sfsa_end(bad)
		n2, c = installed(r)
		if c != "00083000" or n2 != n0:
			bad.append("boot 2 (SFSA 0x83000): %r programs, CPU2 %r"
				% (n2, c))
		if b"past the region (SFSA moved): dead" not in r.out:
			bad.append("boot 2: no 'dead' line")
		out = r.output("echo hi | /tmp/p -c")
		if out is None or b"h   i" not in out:
			bad.append("boot 2: od from RAM: %r" % out)
		if not reboot(r):
			bad.append("boot 3: no login")
			return sfsa_end(bad)
		n3, c = installed(r)
		if c != "000d0000" or n3 != n0:
			bad.append("boot 3 (today's layout): %r programs, CPU2 %r"
				% (n3, c))
		if powercut.BAD.search(r.out):
			bad.append("console: %r" % powercut.BAD.search(r.out).group(0))
	finally:
		r.close()
	r = powercut.Run(0, 0, extra=fake([0x70000]))
	try:
		if not r.login():
			bad.append("SFSA 0x70000: no login")
		else:
			r.sh("cp /bin/od /tmp/p; sync")
			out = r.output("pkg flash /tmp/p 2>&1; echo rc=$?", 120)
			if out is None or b"rc=0" in out:
				bad.append("SFSA 0x70000: install: %r" % out)
			out = r.output("echo hi | /tmp/p -c")
			if out is None or b"h   i" not in out:
				bad.append("SFSA 0x70000: od: %r" % out)
	finally:
		r.close()
	return sfsa_end(bad)


def sfsa_end(bad):
	for b in bad:
		print("radioslot sfsa: FAIL " + b)
	if not bad:
		print("radioslot sfsa: pass")
	return 1 if bad else 0


def main():
	if sys.argv[1:] == ["sfsa"]:
		return sfsa_main()
	r = powercut.Run(0, 0)
	bad = []
	try:
		if not r.login():
			print("radioslot: no login")
			return 1
		out = r.output("radio list")
		if out is None or b"no images" not in out:
			bad.append("radio list: %r" % out)
		r.sh("cp /bin/od /tmp/p; sync")
		r.sh("echo 0x08052000 > /etc/radio.slot")
		out = r.output("radio")
		if out is None or b"radio slot: 0x08052000" not in out \
		   or b"programs up to 0x08052000" not in out:
			bad.append("radio: %r" % out)
		out = r.output("pkg flash /tmp/p 2>&1; echo rc=$?", 120)
		if out is None or b"radio slot" not in out or b"rc=0" in out:
			bad.append("install into the slot: %r" % out)
		r.sh("echo 0x080A0000 > /etc/radio.slot")
		out = r.output("pkg flash /tmp/p 2>&1; echo rc=$?", 120)
		if out is None or b"rc=0" not in out:
			bad.append("install below a higher slot: %r" % out)
		r.sh("pkg unflash /tmp/p; rm /etc/radio.slot", 120)
		out = r.output("radio")
		if out is None or b"radio slot: none" not in out:
			bad.append("radio without a slot: %r" % out)
	finally:
		r.close()
	for b in bad:
		print("radioslot: FAIL " + b)
	if not bad:
		print("radioslot: pass")
	return 1 if bad else 0


if __name__ == "__main__":
	sys.exit(main())
