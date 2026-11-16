#!/usr/bin/env python3
"""p3check - Phase 3 acceptance (AC3.1-AC3.5, spec R3.2-R3.7) in QEMU.

Usage: p3check.py

Boots a fresh copy of build/qemu/minix/disk.img (make minix disk, BOARD
qemu) like tools/powercut.py, through the first boot, and checks:

  AC3.2  the first boot asks for the root password before any login
         prompt; /etc/passwd holds no hashes (root is ##root), /etc/shadow
         is mode 600 with a $5$ hash; root with an empty password and with
         a wrong one is refused, the wrong one after >= 2 s (R3.3)
  AC3.1  two passwd runs in a row give different salts (R3.2)
  AC3.5  /usr/tests/sticky (as nobody: rm and mv of root's file in /tmp)
  AC3.4  /usr/tests/pkgbad (crafted packages refused with a message)
  AC3.3  /usr/tests/pkgplant plant (as nobody), then pkg get receiving a
         small package over the console (the flsend protocol), then
         /usr/tests/pkgplant check: the planted package was not installed
  R3.3   /usr/tests/pwdcheck: pwdauth answers a ##root check only for
         root, refused ones after 2 s
Exit status: the number of failures.
"""
import importlib.util
import io
import os
import re
import sys
import tarfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
	"powercut", os.path.join(HERE, "powercut.py"))
powercut = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(powercut)

PW = powercut.PASSWORD
fails = 0
T0 = time.time()


def check(ok, what):
	global fails
	print(("  ok   " if ok else "  FAIL ") + what + "  [%.0f s]"
		% (time.time() - T0), flush=True)
	if not ok:
		fails += 1


def dummy_package():
	"""A small package other than hello, p3d_1.0.tar."""
	buf = io.BytesIO()
	with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
		for name, data in (("CONTROL/control",
				b"Package: p3d\nVersion: 1.0\nAbi: any\n"),
				("usr/share/p3d", b"p3check\n")):
			info = tarfile.TarInfo(name)
			info.size, info.mode = len(data), 0o644
			t.addfile(info, io.BytesIO(data))
	return buf.getvalue()


def send_packages(r, files):
	"""Speak flsend's protocol to a running pkg get."""
	if r.wait(rb"GPKG READY\n", 30) is None:
		return False
	for name, data in files:
		r.p.stdin.write(b"FILE %s %d %08x\n" % (name.encode(), len(data),
			zlib.crc32(data) & 0xFFFFFFFF))
		r.p.stdin.flush()
		for i in range(0, len(data), 64):
			r.p.stdin.write(data[i:i + 64])
			r.p.stdin.flush()
			time.sleep(0.01)
		if r.wait(rb"OK\n|BAD[^\n]*\n", 60) is None:
			return False
	r.p.stdin.write(b"DONE\n")
	r.p.stdin.flush()
	return True


def salt(r):
	"""The salt of root's $5$ hash in /etc/shadow, or None."""
	out = r.output("grep '^root:' /etc/shadow")
	m = re.match(rb"root:\$5\$(?:rounds=\d+\$)?([^$]*)\$", out or b"")
	return m.group(1) if m else None


def passwd(r, pw):
	r.send("passwd root; echo @@$?@@")
	for prompt in (rb"New password:", rb"Retype password:"):
		if r.wait(prompt, 30) is None:
			return None
		r.send(pw)
	m = r.wait(rb"@@(\d+)@@", 300)	# SHA-crypt: slow in QEMU
	return None if m is None else int(m.group(1))


def main():
	r = powercut.Run(0xFFFFFFFF, 0)
	try:
		m = r.wait(rb"login: |Press Enter to set one", 180)
		check(m is not None and m.group(0) != b"login: ",
			"AC3.2: the first boot asks for the root password first")
		if m is None or not r.set_root_password(PW):
			check(False, "set the root password")
			return
		# root with an empty password, then a wrong one: refused
		r.wait(rb"login: ", 60)
		r.send("root")
		r.wait(rb"Password:", 30)
		r.send("")
		check(r.wait(rb"Login incorrect", 30) is not None,
			"AC3.2: root with an empty password is refused")
		r.wait(rb"login: ", 30)
		r.send("root")
		r.wait(rb"Password:", 30)
		t = time.time()
		r.send("wrong")
		ok = r.wait(rb"Login incorrect", 120) is not None
		check(ok and time.time() - t >= 2.0,
			"R3.3: a wrong password is refused after >= 2 s (%.1f s)"
			% (time.time() - t))
		r.wait(rb"login: ", 30)
		r.send("root")
		r.wait(rb"Password:", 30)
		r.send(PW)
		if r.wait(rb"# ", 120) is None:
			check(False, "login as root with the new password")
			return

		pw = r.output("cat /etc/passwd")
		fields = [l.split(b":")[1] for l in (pw or b"").splitlines()
			if l.count(b":") >= 6]
		check(pw is not None and fields and all(f == b"*"
			or f.startswith(b"##") for f in fields)
			and b"root:##root:" in pw,
			"AC3.2: /etc/passwd holds no hashes, root is ##root")
		ls = r.output("ls -l /etc/shadow")
		check(ls is not None and ls.startswith(b"-rw------- "),
			"R3.4: /etc/shadow is mode 600")
		s1 = salt(r)
		check(s1 is not None and len(s1) == 16,
			"R3.4: root's hash in /etc/shadow is $5$ with a 16-character "
			"salt")

		# AC3.1: two passwd runs in a row
		ok = passwd(r, PW) == 0
		s2 = salt(r)
		ok = ok and passwd(r, PW) == 0
		s3 = salt(r)
		check(ok and s2 and s3 and s2 != s3 and s1 != s2,
			"AC3.1: consecutive passwd runs give different salts "
			"(%s %s)" % (s2, s3))

		check(r.sh("pkg install /lib/pkg/tests_1.0.tar", 120) == 0,
			"install the tests package")
		check(r.sh("/usr/tests/pwdcheck " + PW, 600) == 0,
			"R3.3: tests/pwdcheck (pwdauth: root or the user, 2 s)")
		check(r.sh("/usr/tests/sticky", 30) == 0,
			"AC3.5: tests/sticky")
		check(r.sh("/usr/tests/pkgbad", 120) == 0,
			"AC3.4: tests/pkgbad")
		check(r.sh("/usr/tests/pkgplant plant", 30) == 0,
			"AC3.3: tests/pkgplant plant (as nobody)")
		r.send("pkg get; echo @@$?@@")
		ok = send_packages(r, [("p3d_1.0.tar", dummy_package())])
		m = r.wait(rb"@@(\d+)@@", 120)
		check(ok and m is not None and m.group(1) == b"0",
			"AC3.3: pkg get installs what it received")
		check(r.sh("test -f /usr/share/p3d", 15) == 0,
			"AC3.3: the received package is installed")
		check(r.sh("/usr/tests/pkgplant check", 30) == 0,
			"AC3.3: tests/pkgplant check (nothing planted installed)")
		check(r.sh("test -f /usr/tmp/incoming/hello_1.0.tar", 15) == 0,
			"AC3.3: the planted file was not touched")
		bad = powercut.BAD.search(r.out)
		check(bad is None, "console: no panic")
	finally:
		if os.environ.get("P3CHECK_LOG"):
			with open(os.environ["P3CHECK_LOG"], "wb") as f:
				f.write(r.out)
		if fails:
			sys.stdout.write("---- console (last 3000 bytes):\n%s\n----\n"
				% r.out[-3000:].decode("latin-1"))
		r.close()


if __name__ == "__main__":
	main()
	print("P3CHECK %s (%d failures)" % ("PASS" if fails == 0 else "FAIL",
		fails))
	sys.exit(fails)
