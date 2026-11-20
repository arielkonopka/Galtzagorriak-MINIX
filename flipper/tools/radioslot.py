#!/usr/bin/env python3
"""radioslot - R2b.1a in QEMU (task T30a): `pkg flash` keeps out of the
radio slot.  Needs `make minix disk` (the QEMU image, normal INIT).

QEMU's flash layout (hal/qemu/board.c) has its installed region at 0x48000;
its first free page is 0x52000 (two directory pages and /dev/rom before
it).  With /etc/radio.slot = 0x08052000 nothing fits below the slot: the
install must fail and name the slot; a slot higher up leaves room; without
the file the install works as before.  `radio` prints the layout with the
slot, `radio list` the (empty) card.
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
	"powercut", os.path.join(HERE, "powercut.py"))
powercut = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(powercut)


def main():
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
