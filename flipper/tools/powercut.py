#!/usr/bin/env python3
"""powercut - AC2b.5: cut the power during installs, in QEMU.

Usage: powercut.py [-j JOBS] [--all] [N[:MODE] ...]
       powercut.py --selftest	(the console filter, Run.feed, offline)

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
CFG = 0x000CFFF0
BAD = re.compile(rb"not erased|damaged|panic|does not read back")
PASSWORD = "Kaixo-test1"	# root's, set at the first boot (R3.4)
# The QEMU board mirrors every LCD flush on the serial console
# (hal/qemu/board.c hal_lcd_flush: kprintf, "\n" -> "\r\n"): a frame of
# 128 x 64 pixels as 32 rows of half blocks between two +---+ lines.  The
# kernel prints it straight to the UART, so it can land in the middle of a
# command's output (#278).  Run drops exactly such frames from what it
# reads; anything that only looks like one stays, so the checks on the
# commands' output are as strict as before.
LCD_W = 128
FRAME_BORDER = b"+" + b"-" * LCD_W + b"+\r\n"
FRAME_HEAD = b"\r\n" + FRAME_BORDER
FRAME_ROWS = 32
FRAME_CELLS = (b" ", "\u2580".encode(), "\u2584".encode(), "\u2588".encode())


def clean(s):
	"""What a program printed, without "\r" and empty lines.  feed()
	may leave the leading "\r\n" of a dropped LCD frame in the output
	(an empty line, #281 CI: cut 4 mode 0 printed "...0000006\n\n");
	no checked program prints an empty line, so dropping them is
	safe."""
	return re.sub(rb"\n\n+", b"\n", s.replace(b"\r", b"")).lstrip(b"\n")


def frame_end(buf):
	"""buf starts with FRAME_HEAD.  The end of the whole LCD frame at its
	start, 0 if buf holds only the start of one so far, -1 if it is not
	one (then its bytes are output like any other)."""
	pos = len(FRAME_HEAD)

	def lit(s):	# s at pos: 1 yes, 0 cannot tell yet, -1 no
		got = buf[pos:pos + len(s)]
		if not s.startswith(got):
			return -1
		return 1 if len(got) == len(s) else 0

	for _ in range(FRAME_ROWS):
		k = lit(b"|")
		if k <= 0:
			return k
		pos += 1
		for _ in range(LCD_W):
			for c in FRAME_CELLS:
				k = lit(c)
				if k != -1:
					break
			if k <= 0:
				return k
			pos += len(c)
		k = lit(b"|\r\n")
		if k <= 0:
			return k
		pos += 3
	k = lit(FRAME_BORDER)
	return k if k <= 0 else pos + len(FRAME_BORDER)


class Run:
	def __init__(self, n, mode, image="minix.bin", reuse=None, extra=()):
		"""reuse: the directory of an earlier, stopped Run: its
		card is booted again (tools/bigcard.py).  extra: more QEMU
		arguments (tools/radioslot.py: the fake SFSA)."""
		if reuse:
			self.dir = reuse
		else:
			self.dir = tempfile.mkdtemp(prefix="powercut")
			os.makedirs(os.path.join(self.dir, "build/qemu/minix"))
			subprocess.check_call(["cp", "--sparse=always",	# 4 GB, 2 MB used
				os.path.join(MB, "disk.img"),
				os.path.join(self.dir, "build/qemu/minix/disk.img")])
		cmd = QEMU + [
		    "-device", "loader,file=%s,addr=0" %
				os.path.abspath(os.path.join(MB, image)),
		    "-device", "loader,addr=0x%x,data=%d,data-len=4" % (CFG, n),
		    "-device", "loader,addr=0x%x,data=%d,data-len=4" %
							(CFG + 4, mode)] + list(extra)
		self.p = subprocess.Popen(cmd, cwd=self.dir, stdin=subprocess.PIPE,
			stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
		self.out = b""		# the console without LCD frames
		self.pend = b""		# read, maybe the start of a frame
		self.done = 0		# bytes of self.pend in self.out already
		self.frames = 0
		self.seen = 0

	def feed(self, d):
		"""Add what QEMU printed to self.out, without the LCD frames.

		Only a tail that may start a frame and is longer than "\r\n"
		(so from "\r\n+") is held back: almost every line ends in
		"\r\n", and wait() on a pattern ending in "\n" must see it
		at once (#281 review).  Such a "\r\n" goes to self.out and
		also stays in self.pend (self.done: how many bytes at the
		start of self.pend are in self.out already), so a frame that
		starts in the next read is still dropped; only its leading
		"\r\n" then stays in the output."""
		buf = self.pend + d
		done = self.done	# buf[:done] is in self.out already
		pos = 0			# buf[:pos] is handled

		def emit(upto):
			s = max(pos, done)
			if upto > s:
				self.out += buf[s:upto]
			return upto

		while True:
			i = buf.find(FRAME_HEAD, pos)
			if i < 0:	# keep back what may start a frame
				k = min(len(buf) - pos, len(FRAME_HEAD) - 1)
				while k > 0 and not FRAME_HEAD.startswith(buf[-k:]):
					k -= 1
				keep = len(buf) - k
				upto = len(buf) if k <= 2 else keep
				emit(upto)
				self.pend = buf[keep:]
				self.done = max(0, max(done, upto) - keep)
				return
			pos = emit(i)
			e = frame_end(buf[i:])
			if e == 0:		# the rest comes later
				self.pend = buf[i:]
				self.done = max(0, done - i)
				return
			if e < 0:		# not a frame: output
				pos = emit(i + len(FRAME_HEAD))
			else:
				self.frames += 1
				pos = i + e

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
				self.feed(d)
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

	def output(self, line, secs=20):
		"""What line prints, or None.  The markers are split by quotes
		in what is typed, so only the shell's output matches them: the
		echo of the typed line can be broken up by a late prompt
		("ech# o"), which the old match took for no output."""
		self.send("echo @@S''TART@@; %s; echo @@E''ND@@" % line)
		m = self.wait(rb"@@START@@\r?\n(.*?)@@END@@", secs)
		return clean(m.group(1)) if m else None

	def late(self, secs=60):
		"""After output() gave None: does it come later?  For the
		failure message only (the run has failed already): slow or
		hung."""
		t = time.time()
		m = self.wait(rb"@@START@@\r?\n(.*?)@@END@@", secs)
		if m is None:
			return "nothing %d s later either" % secs
		return "%r %d s later" % (clean(m.group(1)),
					  time.time() - t)

	def set_root_password(self, pw=PASSWORD):
		"""The first boot's question: getty runs passwd root before its
		first login prompt while root has none (spec R3.4)."""
		self.send("")
		for prompt in (rb"New password:", rb"Retype password:"):
			if self.wait(prompt, 30) is None:
				return False
			self.send(pw)
		return True

	def login(self, pw=PASSWORD):
		"""Log in as root; on a new card set its password first."""
		for _ in range(3):
			m = self.wait(rb"login: |Press Enter to set one", 120)
			if m is None:
				return False
			if m.group(0) != b"login: ":
				if not self.set_root_password(pw):
					return False
				continue
			self.send("root")
			m = self.wait(rb"Password:|# ", 30)
			if m is not None and m.group(0) == b"Password:":
				self.send(pw)
				m = self.wait(rb"# ", 120)	# SHA-crypt: slow in QEMU
			if m is not None:
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
# --compact: a file that is never changed again, installed late in the
# directory (compaction copies it last): its record must survive any cut.
# /tmp/p's records may go: each install of it follows a cp (new ctime),
# so the old record is stale and compaction drops it (R2b.12).
KEEP = "cp /bin/sum /tmp/k; sync; pkg flash /tmp/k"
STEPS = [A, B]


def build_epoch():
	"""GZ_BUILD_EPOCH of the image (gen/gzbuild.h), or "?"."""
	try:
		with open(os.path.join(MB, "gen", "gzbuild.h")) as f:
			m = re.search(r"GZ_BUILD_EPOCH\s+(\d+)", f.read())
		return m.group(1) if m else "?"
	except OSError:
		return "?"


def one(n, mode, expect):
	"""Returns (n, mode, problem or None, where the cut was)."""
	r = Run(n, mode)
	try:
		res = checks(r, n, mode, expect)
		if res[2] and res[2] != "no cut happened":	# console's last lines, for CI flakes
			sys.stdout.write("---- cut %d mode %d, console:\n%s\n----\n"
				% (n, mode, r.out[-1500:].decode("latin-1")))
		return res
	finally:
		r.close()


def checks(r, n, mode, expect):
	"""The checks of one run (one())."""
	if True:
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
		if len(STEPS) > 2:		# --compact: /tmp/k's record kept
			r.send("free -v | tail -1; echo @@end@@")
			m = r.wait(rb"installed programs: (\d+)", 20)
			if m is None or int(m.group(1)) < 1:
				return (n, mode, "the installed record was lost",
					where)
		is_od = r.sh("cmp -s /tmp/p /bin/od") == 0
		want = expect["od" if is_od else "cksum"]
		for step in ("as left", "reinstalled"):
			if step == "reinstalled" and r.sh("pkg flash /tmp/p") != 0:
				# pkg says "the clock is not set" for MM's
				# EAGAIN (finst.c in_clock_set): the clock
				# before the build's commit, or no answer
				# from CLOCK.  The two times tell which.
				return (n, mode, "pkg flash /tmp/p fails after "
					"the cut (time %r, build epoch %s)" %
					(r.output("date +%s"), build_epoch()),
					where)
			got = r.output("echo hello | /tmp/p")
			if got != want:
				return (n, mode, "/tmp/p (%s, %s) printed %r%s" %
					("od" if is_od else "cksum", step, got,
					 "" if got is not None else
					 " (" + r.late() + ")"), where)
		if r.sh("cp /bin/sum /tmp/q; sync; pkg flash /tmp/q") != 0:
			return (n, mode, "a new install fails after the cut",
				where)
		got = r.output("echo hello | /tmp/q")
		if got != expect["sum"]:
			return (n, mode, "/tmp/q (sum) printed %r%s" % (got,
				"" if got is not None else " (" + r.late() + ")"),
				where)
		if BAD.search(r.out):
			return (n, mode, "console: %r" %
				BAD.search(r.out).group(0), where)
		return (n, mode, None, where)


def reference():
	"""Trace run: the expected outputs and the operations to cut."""
	r = Run(0xFFFFFFFF, 0)
	try:
		if not r.login():
			sys.exit("powercut: no login in QEMU")
		expect = {}
		for prog in ("od", "cksum", "sum"):
			expect[prog] = r.output("echo hello | " + prog)
		for step in STEPS:
			r.sh(step, 600)
		ops = [(int(a, 16), b.decode(), int(c, 16)) for b, a, c in
		       re.findall(rb"\[flashop (\w+) ([0-9a-f]+) ([0-9a-f]+)\]",
				  r.out)]
		r.sh("echo x")
		return expect, ops
	finally:
		r.close()


def selftest():
	"""Offline test of Run.feed() (no QEMU); raises AssertionError."""
	assert clean(b"0000006\r\n\r\n") == b"0000006\n"
	assert clean(b"\r\na\r\n\r\nb\r\n") == b"a\nb\n"
	def run():
		r = Run.__new__(Run)
		r.out, r.pend, r.done, r.frames = b"", b"", 0, 0
		return r

	def frame(bad=False):
		row = "|" + "\u2580\u2584" * (LCD_W // 2) + "|\r\n"
		f = (FRAME_HEAD + row.encode() * FRAME_ROWS + FRAME_BORDER)
		if bad:		# one cell that is no half block
			k = f.index("\u2584".encode(), len(f) // 2)
			f = f[:k] + b"x" + f[k + 3:]
		return f

	# a trailing "\r\n" is output at once (p3check: GPKG READY\n)
	r = run()
	r.feed(b"GPKG READY\r\n")
	assert r.out == b"GPKG READY\r\n", r.out
	assert r.pend == b"\r\n" and r.done == 2
	r.feed(b"next\r\n")
	assert r.out == b"GPKG READY\r\nnext\r\n", r.out
	# a frame in a line, split across 3 reads, is dropped
	f = frame()
	d = b"12345" + f + b"67 abc\r\n$ "
	a, b = 9, 9 + len(f) // 2
	r = run()
	for x in (d[:a], d[a:b], d[b:]):
		r.feed(x)
	assert r.out == b"1234567 abc\r\n$ " and r.frames == 1, r.out
	assert r.pend == b""
	# split right after the "\r\n": out keeps only that "\r\n"
	r = run()
	r.feed(b"done\r\n")
	r.feed(f[2:] + b"$ ")
	assert r.out == b"done\r\n$ " and r.frames == 1, r.out
	# split inside the head after "\r\n+": held back, then dropped
	r = run()
	r.feed(b"x" + f[:5])
	assert r.out == b"x" and r.pend == f[:5], (r.out, r.pend)
	r.feed(f[5:] + b"y")
	assert r.out == b"xy" and r.frames == 1, r.out
	# a fake frame (one bad cell) stays byte for byte, also split
	g = frame(bad=True)
	d = b"a" + g + b"b\r\n"
	for cut in (None, 3, len(g) // 2, len(g)):
		r = run()
		for x in ((d,) if cut is None else (d[:cut], d[cut:])):
			r.feed(x)
		assert r.out == d and r.frames == 0, cut
	# byte by byte: each read ends in "\r\n" once, so the frame's
	# leading "\r\n" stays; nothing else differs from one read
	d = b"p\r\n" + f + b"q\r\n" + g + b"\r\nr\r\n"
	r = run()
	for i in range(len(d)):
		r.feed(d[i:i + 1])
	want = b"p\r\n\r\nq\r\n" + g + b"\r\nr\r\n"
	assert r.out == want and r.frames == 1, r.out[-300:]


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("-j", type=int, default=4)
	ap.add_argument("--all", action="store_true")
	ap.add_argument("--compact", action="store_true")
	ap.add_argument("--selftest", action="store_true",
			help="only the offline test of the console filter")
	ap.add_argument("cuts", nargs="*")
	a = ap.parse_args()
	selftest()		# milliseconds; also in every CI run
	if a.selftest:
		print("selftest ok")
		return
	if a.compact:
		STEPS[:] = [A, B, FILL, KEEP, A, B]

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
