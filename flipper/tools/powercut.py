#!/usr/bin/env python3
"""powercut - AC2b.5: cut the power during installs, in QEMU.

Usage: powercut.py [-j JOBS] [--all] [N[:MODE] ...]

Each run boots a fresh copy of build/qemu/minix/disk.img in QEMU and, as
root, installs two versions of /tmp/p into the installed-programs region
(od, then cksum, both with pkg flash).  The QEMU board (hal/qemu/board.c)
cuts the power at the Nth operation on the region: MODE 0 = that erase or
double word is not done at all, 1 = half done (half the page erased, one
word of the double word written); then the machine resets, keeping the
"flash", like a real power cut.  After the reboot the run checks:

  - /tmp/p runs the text of the file it is now (od or cksum), never the
    other one or a damaged copy;
  - pkg flash /tmp/p works again, and so does a new program (sum);
  - nothing printed "not erased", "damaged" or a panic.

Without N: a trace run finds the operations, then every erase, the first
and last double words of every text, and every directory write are cut,
in both modes.  --all: every operation.  Needs BOARD=qemu built (make
BOARD=qemu minix disk).
"""
import argparse
import os
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
MB = os.path.join(HERE, "..", "build", "qemu", "minix")
QEMU = ["qemu-system-arm", "-M", "mps2-an386", "-nographic", "-monitor",
	"none", "-serial", "stdio", "-semihosting-config",
	"enable=on,target=native,userspace=on"]
CFG = 0x0008FFF0
BAD = re.compile(rb"not erased|damaged|panic|does not read back")


class Run:
	def __init__(self, n, mode):
		self.dir = tempfile.mkdtemp(prefix="powercut")
		os.makedirs(os.path.join(self.dir, "build/qemu/minix"))
		subprocess.check_call(["cp", "--sparse=always",	# 4 GB, 2 MB used
			os.path.join(MB, "disk.img"),
			os.path.join(self.dir, "build/qemu/minix/disk.img")])
		cmd = QEMU + [
		    "-device", "loader,file=%s,addr=0" %
				os.path.abspath(os.path.join(MB, "minix.bin")),
		    "-device", "loader,addr=0x%x,data=%d,data-len=4" % (CFG, n),
		    "-device", "loader,addr=0x%x,data=%d,data-len=4" %
							(CFG + 4, mode)]
		self.p = subprocess.Popen(cmd, cwd=self.dir, stdin=subprocess.PIPE,
			stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
		self.out = b""
		self.seen = 0

	def wait(self, pat, secs=60):
		"""Read until pat (bytes regex) appears after the last match."""
		end = time.time() + secs
		r = re.compile(pat, re.S)
		while time.time() < end:
			m = r.search(self.out, self.seen)
			if m:
				self.seen = m.end()
				return m
			if select.select([self.p.stdout], [], [], 0.2)[0]:
				d = os.read(self.p.stdout.fileno(), 65536)
				if not d:
					break
				self.out += d
		return None

	def send(self, line):
		for c in (line + "\r").encode():	# the UART takes it slowly
			self.p.stdin.write(bytes([c]))
			self.p.stdin.flush()
			time.sleep(0.002)

	def sh(self, line, secs=60):
		self.send(line + "; echo @@$?@@")
		m = self.wait(rb"@@(\d+)@@", secs)
		return None if m is None else int(m.group(1))

	def login(self):
		for _ in range(3):
			if self.wait(rb"login: ", 60) is None:
				return False
			self.send("root")
			if self.wait(rb"# ", 15):
				return True
		return False

	def close(self):
		self.p.kill()
		self.p.wait()
		shutil.rmtree(self.dir, ignore_errors=True)


A = "cp /bin/od /tmp/p; sync; pkg flash /tmp/p"
B = "cp /bin/cksum /tmp/p; sync; pkg flash /tmp/p"
# --compact: 60 more installs of the same two texts (records only: the
# texts are in flash already), then two that fill the directory and make
# the next record compact it.
FILL = ("for i in 1 2 3 4 5 6 7 8 9 10; do for j in 1 2 3; do "
	"cp /bin/od /tmp/p; pkg flash /tmp/p; cp /bin/cksum /tmp/p; "
	"pkg flash /tmp/p; done; done; sync")
STEPS = [A, B]


def one(n, mode, expect):
	"""Returns (n, mode, problem or None, where the cut was)."""
	r = Run(n, mode)
	try:
		if not r.login():
			return (n, mode, "no login", "")
		cut = None
		for step in STEPS:
			r.send(step + "; echo @@$?@@")
			m = r.wait(rb"@@(\d+)@@|\[power cut", 600)
			if m is None:
				return (n, mode, "hung in: " + step, "")
			if m.group(0).startswith(b"[power"):
				cut = m
				break
		where = ""
		if cut is None and n != 0:
			return (n, mode, "no cut happened", "")
		if cut is not None:
			m = re.search(rb"cut at flash op ([0-9a-f]+)", r.out)
			where = str(int(m.group(1), 16)) if m else "?"
			if not r.login():
				return (n, mode, "no login after the cut", where)
		if len(STEPS) > 2:		# --compact: no record lost
			r.send("free -v | tail -1; echo @@end@@")
			m = r.wait(rb"installed programs: (\d+)", 20)
			if m is None or int(m.group(1)) < 1:
				return (n, mode, "the installed record was lost",
					where)
		is_od = r.sh("cmp -s /tmp/p /bin/od") == 0
		want = expect["od" if is_od else "cksum"]
		for step in ("as left", "reinstalled"):
			if step == "reinstalled" and r.sh("pkg flash /tmp/p") != 0:
				return (n, mode, "pkg flash /tmp/p fails after "
					"the cut", where)
			r.send("echo hello | /tmp/p; echo @@end@@")
			m = r.wait(rb"echo @@end@@\r?\n(.*?)@@end@@", 20)
			got = m.group(1).replace(b"\r", b"") if m else None
			if got != want:
				return (n, mode, "/tmp/p (%s, %s) printed %r" %
					("od" if is_od else "cksum", step, got),
					where)
		if r.sh("cp /bin/sum /tmp/q; sync; pkg flash /tmp/q") != 0:
			return (n, mode, "a new install fails after the cut",
				where)
		r.send("echo hello | /tmp/q; echo @@end@@")
		m = r.wait(rb"echo @@end@@\r?\n(.*?)@@end@@", 20)
		if m is None or m.group(1).replace(b"\r", b"") != expect["sum"]:
			return (n, mode, "/tmp/q (sum) printed wrong", where)
		if BAD.search(r.out):
			return (n, mode, "console: %r" %
				BAD.search(r.out).group(0), where)
		return (n, mode, None, where)
	finally:
		r.close()


def reference():
	"""Trace run: the expected outputs and the operations to cut."""
	r = Run(0xFFFFFFFF, 0)
	try:
		if not r.login():
			sys.exit("powercut: no login in QEMU")
		expect = {}
		for prog in ("od", "cksum", "sum"):
			r.send("echo hello | %s; echo @@end@@" % prog)
			m = r.wait(rb"echo @@end@@\r?\n(.*?)@@end@@", 20)
			expect[prog] = m.group(1).replace(b"\r", b"")
		for step in STEPS:
			r.sh(step, 600)
		ops = [(int(a, 16), b.decode(), int(c, 16)) for b, a, c in
		       re.findall(rb"\[flashop (\w+) ([0-9a-f]+) ([0-9a-f]+)\]",
				  r.out)]
		r.sh("echo x")
		return expect, ops
	finally:
		r.close()


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("-j", type=int, default=4)
	ap.add_argument("--all", action="store_true")
	ap.add_argument("--compact", action="store_true")
	ap.add_argument("cuts", nargs="*")
	a = ap.parse_args()
	if a.compact:
		STEPS[:] = [A, B, FILL, A, B]

	expect, ops = reference()
	print("reference: od %r, cksum %r, sum %r; traced %d erases and "
	      "directory writes" % (expect["od"], expect["cksum"], expect["sum"],
				    len(ops)))
	if a.cuts:
		cuts = []
		for c in a.cuts:
			n, _, m = c.partition(":")
			cuts.append((int(n), int(m or 0)))
	else:
		last = max(o[0] for o in ops)
		pick = set()
		erases = [o[0] for o in ops if o[1] == "erase"]
		for i, e in enumerate(erases):	# the erase, a text's ends
			pick |= {e, e + 1, e + 2}
			nxt = erases[i + 1] if i + 1 < len(erases) else last + 1
			pick |= {nxt - 1}
		pick |= {o[0] for o in ops if o[1] == "write"}	# directory
		pick |= {last + 50}		# after everything: no cut
		if a.compact:		# every operation of the compaction
			dirs = [o[0] for o in ops if o[1] == "erase"
				and o[2] < ops[0][2] + 2 * 4096 and o[0] > 1]
			pick = set(range(dirs[0], dirs[1] + 1)) | {last + 50}
		if a.all:
			pick = set(range(1, last + 1))
		cuts = [(n, m) for n in sorted(pick) for m in (0, 1)]
	print("%d runs" % len(cuts))
	bad = 0
	with ThreadPoolExecutor(a.j) as ex:
		for n, mode, prob, where in ex.map(
				lambda c: one(c[0], c[1], expect), cuts):
			if prob and not (prob == "no cut happened"
					 and n > max(o[0] for o in ops)):
				bad += 1
				print("FAIL cut %d mode %d (at %s): %s" %
				      (n, mode, where, prob))
			else:
				print("ok   cut %d mode %d" % (n, mode))
	print("%d of %d runs failed" % (bad, len(cuts)))
	sys.exit(1 if bad else 0)


if __name__ == "__main__":
	main()
