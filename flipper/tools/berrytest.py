#!/usr/bin/env python3
"""berrytest - the Berry scripting runtime in QEMU (spec Phase 7c, issue #385).

Usage: berrytest.py

Boots a fresh copy of build/qemu/minix/disk.img (make minix disk, BOARD
qemu) like tools/powercut.py, logs in as root, installs the berry package
and checks:

  R7c.1  berry runs a source script and the -e code; reals and the
         VM's real % real (gzmath.c)
  R7c.2  the examples' bytecode compiled on the host (berryc, .bec) runs:
         hello and primes (R7c.6, pure computation) give the same output
         as their source
  R7c.3  the tsv, pipe and sensor modules (records sorted and mapped,
         a line joined, sensor.read of a missing sensor is nil)
  R7c.4  an endless loop ends with budget_error and status 3 (default
         budget), a heap bomb with status 4 (default 8 KB heap, -m), a
         bigger -m lets more through; after each the free RAM is back
  R7c.4  responsiveness wins: with two endless loops without a budget
         (-n 0) in the background, and then one of them next to a
         background subshell loop (a shadow group with the login shell,
         issue #429), a shell command still answers within 2 s, and the
         loops die on kill
  R7c.5  berry -i: the REPL evaluates lines typed on the console, keeps
         its variables, and ends on end of input
  R7c.1  the shared Berry library (issue #431): free -v shows it in
         flash and its per-script data area
  RAM    free -v with one script running, and the VM's heap at start;
         two idle scripts (REPLs reading a FIFO) at once, the second
         costs no more than the first (printed as "RAM:" lines;
         AC7c.1's 6 KB is checked on the device)

Exit status: the number of failures.
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


def freebytes(r):
	"""Free RAM in bytes: the sum of the holes free -v lists."""
	out = r.output("free -v", 20) or b""
	return sum(int(m.group(1)) for m in
		   re.finditer(rb"(?m)^  hole at \S+ +(\d+) bytes", out))


def ram_back(r, base, secs=20):
	end = time.time() + secs
	while True:
		now = freebytes(r)
		if now >= base - 64 or time.time() >= end:
			return now
		r.sh("sleep 1", 20)


def status(r, line, secs=60):
	"""Exit status of line and what it printed (stdout and stderr)."""
	out = r.output("%s >/tmp/bo 2>&1; echo st=$?; cat /tmp/bo" % line, secs)
	if out is None:
		return None, b""
	m = re.match(rb"st=(\d+)\n", out)
	return (int(m.group(1)) if m else None), out[m.end():] if m else out


def main():
	r = powercut.Run(0, 0)
	try:
		if not r.login():
			check(False, "login")
			return 1
		check(r.sh("pkg install /lib/pkg/berry_1.0.tar", 180) == 0,
			"install the berry package")
		base = freebytes(r)

		# R7c.1 / R7c.2 / R7c.6
		st, out = status(r, "berry /usr/lib/berry/hello.be")
		check(st == 0 and out == b"Kaixo from Berry 3\n",
			"R7c.1: berry runs hello.be")
		if fails: print(out)
		st, out = status(r, "berry /usr/lib/berry/hello.bec")
		check(st == 0 and out == b"Kaixo from Berry 3\n",
			"R7c.2: the host-compiled hello.bec runs")
		want = b"2 3 5 7 11 13 17 19 23 29\n10 primes below 30\n"
		st, out = status(r, "berry /usr/lib/berry/primes.bec 30", 120)
		check(st == 0 and out == want,
			"R7c.6: primes.bec 30 (bytecode) prints the primes")
		if st != 0 or out != want: print(out)
		st, out = status(r, "berry /usr/lib/berry/primes.be 30", 120)
		check(st == 0 and out == want, "R7c.6: primes.be 30 (source) too")
		st, out = status(r, "berry -e 'print(7 % 3, 7.5 % 2, 3 / 2.0, "
				 "int(\"12\") * 2)'")
		check(st == 0 and out == b"1 1.5 1.5 24\n",
			"R7c.1: -e code, integers and reals, real % real")
		if fails: print(out)

		# R7c.3: the tsv, pipe and sensor modules (records.be).
		st, out = status(r, "berry /usr/lib/berry/records.bec")
		check(st == 0 and out == b"name\ttemp_c\nhall\t19\n"
			b"kitchen\t21.5\n['kitchen', 'hall']\ntrue nil\n",
			"R7c.3: records.bec: tsv.records/join, pipe.where/sortby/"
			"head/map, sensor.list/read")
		if st != 0 or not out.startswith(b"name"): print(st, out)

		# R7c.4 limits
		t = time.time()
		st, out = status(r, "berry -e 'while true end'", 300)
		dt = time.time() - t
		check(st == 3 and b"budget_error" in out,
			"R7c.4: an endless loop ends with budget_error, status 3 "
			"(%.1f s in QEMU)" % dt)
		if st != 3: print(st, out)
		check(ram_back(r, base) >= base - 64,
			"R7c.4: its RAM is free again")
		bomb = ("berry %s -e 'l=[] while true "
			"l.push(\"xxxxxxxxxxxxxxxx\" + str(size(l))) end'")
		st, out = status(r, bomb % "", 120)
		check(st == 4 and b"heap limit" in out,
			"R7c.4: a heap bomb ends at the 8 KB heap limit, status 4")
		if st != 4: print(st, out)
		check(ram_back(r, base) >= base - 64,
			"R7c.4: its RAM is free again")
		st, out = status(r, "berry -e 'l=[] for i:1..60 "
				 "l.push(\"xxxxxxxxxxxxxxxx\" + str(i)) end "
				 "print(size(l))'")
		check(st == 0 and out == b"60\n",
			"R7c.4: 60 strings fit in the default heap")
		st, out = status(r, "berry -m 2048 -e 'l=[] for i:1..60 "
				 "l.push(\"xxxxxxxxxxxxxxxx\" + str(i)) end "
				 "print(size(l))'")
		check(st == 4, "R7c.4: -m 2048 is a smaller heap (status %s)" % st)

		# Responsiveness: runaway scripts without a budget (-n 0).
		# #429: the shell and a fork child of it that shares its
		# addresses (the second `&' before its exec, or a subshell)
		# are a shadow group; next to a runaway loop the group's
		# members swapped places every quantum and neither ran, so the
		# console hung (kernel, arm/shadow.c unshadow).
		loop = "berry -n 0 -e 'while true end' & echo $! >>/tmp/p1; "
		sub = "(while :; do :; done) & echo $! >>/tmp/p1; "
		for what, cmd in (("2 endless scripts", loop * 2),
				  ("an endless script and a subshell loop",
				   loop + sub)):
			r.sh("rm -f /tmp/p1; " + cmd + "sleep 1", 30)
			worst = 0.0
			for _ in range(8):
				t = time.time()
				out = r.output("echo alive", 20)
				dt = time.time() - t
				worst = max(worst, dt)
				if out != b"alive\n":
					worst = 99
					break
			check(worst < 2.0, "R7c.4: the shell answers in %.2f s "
				"at most while %s run" % (worst, what))
			if worst >= 99: return fails	# the console is gone
			r.sh("kill `cat /tmp/p1`; sleep 1", 20)
			out = r.output("for p in `cat /tmp/p1`; do kill -0 $p "
				       "2>/dev/null && echo $p; done; echo done", 20)
			check(out == b"done\n", "R7c.4: the loops die on kill")
			check(ram_back(r, base) >= base - 64,
				"R7c.4: their RAM is free again")

		# R7c.5: the REPL.
		mark = len(r.out)
		r.send("berry -i; echo @@R''EPL@@")
		ok = r.wait(rb"> ", 30) is not None
		r.send("a = 6 * 7")
		ok = ok and r.wait(rb"> ", 20) is not None
		r.send("print(a + 1)")
		ok = ok and r.wait(rb"43\r?\n", 20) is not None
		r.send("\x04")		# end of input
		ok = ok and r.wait(rb"@@REPL@@", 20) is not None
		check(ok, "R7c.5: berry -i keeps a = 42 and prints a + 1")
		if not ok: print(r.out[mark:][-400:])

		# RAM of a running script (the process: chmem, data, GOT) and
		# of two at once: idle scripts, REPLs blocked reading a FIFO
		# that a sleep holds open (no runaway loop: without a budget
		# that is the #429 case).  The first comes with the sleep, so
		# the second's cost is the clean number; a third must cost
		# the same (nothing of the VM is copied per script).
		st, out = status(r, "berry -e 'import gc print(gc.allocated())'")
		print("  RAM: VM heap at start: %s bytes" % out.strip().decode(),
			flush=True)
		out = r.output("free -v", 20) or b""
		m = re.search(rb"shared Berry: (\d+) KB at (\S+), id \S+, \d+ "
			      rb"functions, (\d+) bytes of data per process", out)
		check(m is not None and int(m.group(3)) <= 512,
			"R7c.1: the shared Berry library is in flash (%s KB at %s), "
			"%s bytes of data per script" % (m.group(1).decode(),
			m.group(2).decode(), m.group(3).decode()) if m else
			"R7c.1: free -v shows the shared Berry library")
		r.sh("rm -f /tmp/bf /tmp/p1; mkfifo /tmp/bf; "
		     "sleep 120 >/tmp/bf & echo $! >/tmp/p1; "
		     "berry -i </tmp/bf >/dev/null & echo $! >>/tmp/p1; sleep 2",
		     30)
		f1 = freebytes(r)
		r.sh("berry -i </tmp/bf >/dev/null & echo $! >>/tmp/p1; sleep 2",
		     30)
		f2 = freebytes(r)
		if os.environ.get("BERRYTEST_RAM"):
			print((r.output("free -v", 20) or b"").decode())
		r.sh("berry -i </tmp/bf >/dev/null & echo $! >>/tmp/p1; sleep 2",
		     30)
		f3 = freebytes(r)
		one, two = f1 - f2, f2 - f3
		print("  RAM: one idle script: %6d bytes (free %d -> %d), "
			"another: %d bytes (-> %d)" % (one, f1, f2, two, f3),
			flush=True)
		check(0 < one <= 14336,
			"AC7c.1: one script process <= 14 KB in QEMU (%d B; the "
			"8 KB default heap is reserved up front, PORTING.md Berry)"
			% one)
		out = r.output("echo alive", 20)
		check(out == b"alive\n" and one - 256 <= two <= one + 256,
			"AC7c.1: scripts side by side: the next one takes %d B, "
			"like the one before (%d B)" % (two, one))
		r.sh("kill `cat /tmp/p1`; sleep 1; rm -f /tmp/bf", 20)
		check(ram_back(r, base) >= base - 64,
			"AC7c.1: their RAM is free again")
		print("%d failed" % fails)
		if os.environ.get("BERRYTEST_LOG"):
			with open(os.environ["BERRYTEST_LOG"], "wb") as f:
				f.write(r.out)
		return fails
	finally:
		r.close()


if __name__ == "__main__":
	sys.exit(main())
