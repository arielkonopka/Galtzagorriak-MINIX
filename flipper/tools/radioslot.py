#!/usr/bin/env python3
"""radioslot - R2b.1a in QEMU (task T30a): `pkg flash` keeps out of the
radio slot.  Needs `make minix disk` (the QEMU image, normal INIT).

QEMU's flash layout (hal/qemu/board.c) has its installed region at 0x60000;
its first free page is 0x6A000 (two directory pages and /dev/rom before
it).  With /etc/radio.slot = 0x0806A000 nothing fits below the slot: the
install must fail and name the slot; a slot higher up leaves room; without
the file the install works as before.  `radio` prints the layout with the
slot, `radio list` the (empty) card.

Part 2, `radioslot.py sfsa`: QEMU with a fake SFSA (hal/qemu/board.c
FAKE_SFSA); the installed region goes from one QEMU run to the next
through the monitor (pmemsave, then a loader file), as a CPU2 stack swap
would leave it.  Run 1 today's layout, a program installed at 0x6A000;
run 2 SFSA 0x9B000 (programs end at 0x6B000): the record is past the
end, MM marks it dead at boot, sed runs from RAM; run 3 today's
layout again: the record stays dead.  Run 4 SFSA 0x70000 (no room below
the guard): neither region, `pkg flash` refuses, the system runs.
"""
import importlib.util
import os
import re
import socket
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
	"powercut", os.path.join(HERE, "powercut.py"))
powercut = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(powercut)


FAKE_SFSA = 0x000CFFE0
REGION = (0x60000, 0x87F00)		# up to the config words (FAKE_SFSA, CUT_CFG)


def qemu_extra(sfsa, region, mon):
	a = ["-monitor", "unix:%s,server=on,wait=off" % mon]
	if sfsa:
		a += ["-device", "loader,addr=0x%x,data=%d,data-len=4"
			% (FAKE_SFSA, sfsa)]
	if region:
		a += ["-device", "loader,file=%s,addr=0x%x" % (region, REGION[0])]
	return a


def installed(r):
	"""The number of installed programs (free -v) and the CPU2 line."""
	out = r.output("free -v | grep installed")
	m = re.search(rb"installed programs: (\d+).*CPU2 flash from (\w+)",
		out or b"")
	return (int(m.group(1)), m.group(2).decode()) if m else (None, out)


def save_region(r, mon, path):
	"""sync, then the installed region to path (QEMU monitor)."""
	r.sh("sync")
	s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
	s.connect(mon)
	s.settimeout(10)
	got = b""
	try:
		while b"(qemu)" not in got:
			got += s.recv(4096)
		s.sendall(b"pmemsave 0x%x 0x%x \"%s\"\n" % (REGION[0],
						REGION[1], path.encode()))
		got = b""
		while b"(qemu)" not in got:
			got += s.recv(4096)
	except socket.timeout:
		print("radioslot: monitor: %r" % got)
	s.close()
	return os.path.exists(path) and os.path.getsize(path) == REGION[1]


def od_ok(r):
	"""/tmp/p is sed (10 KB of text: 3 pages from 0x6A000)."""
	out = r.output("echo hi | /tmp/p s/h/j/")
	return out is not None and b"ji" in out


def boot(sfsa, region, reuse, tag):
	mon = os.path.join(tempfile.mkdtemp(prefix="rsmon"), "m")
	r = powercut.Run(0, 0, reuse=reuse,
			 extra=qemu_extra(sfsa, region, mon))
	return r, mon


def stop(r):
	"""QEMU off, the card kept for the next run."""
	r.p.kill()
	r.p.wait()


def sfsa_end(bad):
	for b in bad:
		print("radioslot sfsa: FAIL " + b)
	if not bad:
		print("radioslot sfsa: pass")
	return 1 if bad else 0


def sfsa_main():
	bad = []
	keep = tempfile.mkdtemp(prefix="rsregion")
	reg1, reg2 = os.path.join(keep, "r1"), os.path.join(keep, "r2")
	r, mon = boot(0, None, None, "1")
	try:
		if not r.login():
			return sfsa_end(["run 1: no login"])
		n0, c = installed(r)
		if n0 is None:
			return sfsa_end(["free: %r" % c])
		r.sh("cp /bin/sed /tmp/p; sync")
		for i in range(3):	# /usr may not be mounted yet
			out = r.output("pkg flash /tmp/p 2>&1; echo rc=$?", 120)
			if out is not None and b"is missing" not in out:
				break
			time.sleep(5)
		n1, c = installed(r)
		if out is None or b"rc=0" not in out or n1 != n0 + 1:
			bad.append("run 1: install: %r, %r -> %r" % (out, n0, n1))
		if not save_region(r, mon, reg1):
			bad.append("run 1: no region dump")
		card = r.dir
	finally:
		stop(r)
	if bad:
		return sfsa_end(bad)
	r, mon = boot(0x9B000, reg1, card, "2")
	try:
		if not r.login():
			return sfsa_end(["run 2: no login"])
		n2, c = installed(r)
		if c != "00083000" or n2 != n0:
			bad.append("run 2 (SFSA 0x9B000): %r programs, CPU2 %r"
				% (n2, c))
		if b"past the region (SFSA moved): dead" not in r.out:
			bad.append("run 2: no 'dead' line: %r" % re.findall(
				rb"flash:[^\n]*", r.out))
		if not od_ok(r):
			bad.append("run 2: od from RAM")
		if not save_region(r, mon, reg2):
			bad.append("run 2: no region dump")
	finally:
		stop(r)
	r, mon = boot(0, reg2, card, "3")
	try:
		if not r.login():
			return sfsa_end(bad + ["run 3: no login"])
		n3, c = installed(r)
		if c != "000d0000" or n3 != n0:
			bad.append("run 3 (today's layout): %r programs, CPU2 %r"
				% (n3, c))
		if not od_ok(r):
			bad.append("run 3: od")
		if powercut.BAD.search(r.out):
			bad.append("run 3: console: %r"
				% powercut.BAD.search(r.out).group(0))
	finally:
		stop(r)
	r, mon = boot(0x70000, None, card, "4")
	try:
		if not r.login():
			bad.append("run 4 (SFSA 0x70000): no login")
		else:
			out = r.output("pkg flash /tmp/p 2>&1; echo rc=$?", 120)
			if out is None or b"rc=0" in out:
				bad.append("run 4 (SFSA 0x70000): install: %r" % out)
			if not od_ok(r):
				bad.append("run 4: od")
	finally:
		r.close()
	return sfsa_end(bad)


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
		r.sh("cp /bin/sed /tmp/p; sync")
		r.sh("echo 0x0806A000 > /etc/radio.slot")
		out = r.output("radio")
		if out is None or b"radio slot: 0x0806A000" not in out \
		   or b"programs up to 0x0806A000" not in out:
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
