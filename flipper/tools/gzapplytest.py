#!/usr/bin/env python3
"""gzapplytest - `gzupdate apply` in QEMU (spec RS.10, RS.11; T33a part 3).

Boots a fresh copy of build/qemu/minix/disk.img (as tools/powercut.py
does), logs in as root and runs `gzupdate apply -y -n /lib/gzu/apply.upd`
(a real update file of this build, its own epoch, with -e one lower, signed with the
throw-away key of tools/updatetest.py; minix.mk).  Then it checks:

  - apply exits 0 and says "staged";
  - /.gzupdate/<epoch>/MANIFEST lists the base files, and each listed
    file is in /.gzupdate/<epoch>/ or /usr/.gzupdate/<epoch>/ (spot check:
    /bin/sh and its SHA-512 prefix);
  - /usr/.gzupdate/cur.update is the applied file;
  - the swap partition starts with the file (read back on the host) and
    holds the pending sector after it;
  - tools/ateasim.py (the simulated atea flash step) installs the flash
    section into a copy of the flash image and clears the pending sector;
    a second run has nothing to do;
  - QEMU boots the flashed image to a root shell; INIT ran gztree
    (RS.14): /etc/gz-tree has the new epoch; /bin/sum and a /usr file,
    changed before apply, are the update's again, the changed ones are
    kept in /.gzupdate/0/ and /usr/.gzupdate/0/;
  - ACS.3 for atea's flash step: a cut at every page operation leaves the
    first page erased or the whole new image, and the next run finishes;
  - /etc/rc ran `gzupdate finish` (staged trees removed) and `commit`
    (/usr/.gzupdate/trial removed).

Needs `make BOARD=qemu minix base-flash base disk`.

Usage: gzapplytest.py"""
import hashlib
import importlib.util
import os
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "powercut", os.path.join(TOOLS, "powercut.py"))
pc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pc)
MB = pc.MB
UPD = os.path.join(MB, "gzu", "apply.upd")
KEY = os.path.join(MB, "updatetest", "trusted.pub")


def epoch_of(path):
	with open(path, "rb") as f:
		return pc_hdr(f.read(512))[7]


def pc_hdr(sec0):
	spec = importlib.util.spec_from_file_location(
	    "gzcheck", os.path.join(TOOLS, "gzupdate-check.py"))
	g = importlib.util.module_from_spec(spec)
	spec.loader.exec_module(g)
	return g.HDR.unpack_from(sec0)


def ateasim(disk, flash, *more):
	r = subprocess.run([sys.executable, "-I",
			    os.path.join(TOOLS, "ateasim.py"), "--disk", disk,
			    "--flash", flash, "--key", KEY, "-v", *more],
			   capture_output=True, text=True)
	return r.returncode, (r.stdout + r.stderr).strip()


def main():
	fails = []

	def check(ok, what):
		print(("ok    " if ok else "FAIL  ") + what)
		if not ok:
			fails.append(what)

	data = open(UPD, "rb").read()
	ep = epoch_of(UPD)
	r = pc.Run(0xFFFFFFFF, 0)
	keep = r.dir
	try:
		check(r.login(), "login")
		# Two base files that differ from the update's, so gztree has
		# something to switch (the rest is the same build: skipped).
		sumref = r.output("echo hello | sum")
		r.sh("cp /bin/od /bin/sum; mkdir /usr/share /usr/share/region; "
		     "echo x > /usr/share/region/AL.reg; sync")
		r.send("gzupdate apply -y -n -e %d /lib/gzu/apply.upd; echo @@$?@@"
		       % (ep - 1))
		m = r.wait(rb"@@(\d+)@@", 300)
		out = r.out[-2000:].decode("latin-1")
		check(m is not None and m.group(1) == b"0"
		      and "staged" in out, "gzupdate apply exits 0, staged")
		n = r.output("wc -l < /.gzupdate/%d/MANIFEST" % ep)
		check(n is not None and n.strip().isdigit()
		      and int(n.strip()) > 20,
		      "MANIFEST lists the base files (%r)" % n)
		line = r.output("grep '^/bin/sh ' /.gzupdate/%d/MANIFEST" % ep)
		got = r.output("cmp /.gzupdate/%d/bin/sh /bin/sh && echo same"
			       % ep)
		check(line is not None and line.startswith(b"/bin/sh 755 "),
		      "MANIFEST has /bin/sh (%r)" % line)
		check(got is not None and b"same" in got,
		      "the staged /bin/sh is this build's")
		usr = r.output("ls /usr/.gzupdate/%d | head -3" % ep)
		check(usr is not None and len(usr.split()) > 0,
		      "/usr files staged in /usr/.gzupdate/%d (%r)" % (ep, usr))
		got = r.output("cmp /usr/.gzupdate/cur.update /lib/gzu/apply.upd"
			       " && echo same", 180)
		check(got is not None and b"same" in got,
		      "cur.update is the applied file (%r)" % got)
		r.sh("sync")
		r.p.kill()
		r.p.wait()
		disk = os.path.join(keep, "build/qemu/minix/disk.img")
		spec2 = importlib.util.spec_from_file_location(
		    "ateasim", os.path.join(TOOLS, "ateasim.py"))
		at = importlib.util.module_from_spec(spec2)
		spec2.loader.exec_module(at)
		part = at.swap_partition(disk)
		check(part is not None, "the card has a swap partition")
		with open(disk, "rb") as d:
			d.seek(part[0])
			staged = d.read(len(data))
			pend = d.read(512)
		check(staged == data, "the swap partition starts with the file")
		check(pend[:8] == at.PEND
		      and pend[16:80] == hashlib.sha512(data).digest(),
		      "the pending sector follows it")
		# ACS.3 (S.5 "atea flashing"): a power cut at every page
		# operation of the flash step.  Each cut leaves either the
		# first page erased (the image counts as incomplete) or the
		# whole new image; the next start finishes the job.
		base = open(os.path.join(MB, "minix-base.bin"), "rb").read()
		cutf = os.path.join(keep, "cut.bin")
		bad_cut, n = None, 0
		while True:
			n += 1
			with open(os.path.join(MB, "minix.bin"), "rb") as f, \
			     open(cutf, "wb") as g:
				g.write(f.read())
			rc, msg = ateasim(disk, cutf, "--cut", str(n))
			if rc == 0:
				break		# no operation left to cut
			img = open(cutf, "rb").read()
			if rc != 3 or not (img[:4096] == b"\xff" * 4096
					   or img[:len(base)] == base):
				bad_cut = "cut %d: %s" % (n, msg)
				break
			rc, msg = ateasim(disk, cutf)
			img = open(cutf, "rb").read()
			if rc != 0 or img[:len(base)] != base:
				bad_cut = "after cut %d: %s" % (n, msg)
				break
			with open(disk, "r+b") as d:	# pending again
				d.seek(part[0] + len(data))
				d.write(pend)
		check(bad_cut is None and n > 2,
		      "ACS.3: a cut at each of the %d page operations of atea's "
		      "flash step, the next start finishes it%s" %
		      (n - 1, "" if bad_cut is None else " (" + bad_cut + ")"))
		with open(disk, "r+b") as d:
			d.seek(part[0] + len(data))
			d.write(pend)
		flash = os.path.join(keep, "flash.bin")
		with open(os.path.join(MB, "minix.bin"), "rb") as f, \
		     open(flash, "wb") as g:
			g.write(f.read())
		rc, msg = ateasim(disk, flash)
		print("      " + msg.replace("\n", "\n      "))
		check(rc == 0 and "installed" in msg, "atea installs it")
		check(open(flash, "rb").read()[:len(base)] == base,
		      "the flash image is this build's minix-base.bin")
		rc, msg = ateasim(disk, flash)
		check(rc == 0 and "nothing pending" in msg,
		      "a second atea run has nothing to do")
		r2 = pc.Run(0xFFFFFFFF, 0, image=flash, reuse=keep)
		r = r2
		check(r2.login(), "the flashed image boots to a root shell")
		v = r2.output("ver | head -1")
		check(v is not None and len(v) > 0, "ver runs (%r)" % v)
		# RS.14: INIT ran gztree before /etc/rc.
		ok = b"gztree: done" in r2.out
		if not ok:
			print(r2.out[-3000:].decode("latin-1"))
		check(ok, "INIT ran gztree, it is done")
		check(b"finishing the update" in r2.out,
		      "rc ran gzupdate finish (RS.15)")
		v = r2.output("ls -d /.gzupdate/%d /usr/.gzupdate/%d "
			      "/usr/.gzupdate/trial 2>&1 | grep -c 'No such'"
			      % (ep, ep))
		check(v is not None and v.strip() == b"3", "the staged trees "
		      "and the trial flag are gone after finish and commit "
		      "(%r)" % v)
		v = r2.output("cat /etc/gz-tree")
		check(v == b"%d\n" % ep, "/etc/gz-tree is %d (%r)" % (ep, v))
		v = r2.output("echo hello | sum")
		check(v == sumref, "the staged /bin/sum is in place (%r)" % v)
		v = r2.output("cmp /.gzupdate/0/bin/sum /bin/od && echo same")
		check(v is not None and b"same" in v,
		      "the old /bin/sum kept in /.gzupdate/0 (%r)" % v)
		v = r2.output("cat /usr/.gzupdate/0/share/region/AL.reg")
		check(v == b"x\n", "the old /usr file kept in "
		      "/usr/.gzupdate/0 (%r)" % v)
		v = r2.output("wc -c < /usr/share/region/AL.reg")
		check(v is not None and v.strip().isdigit() and int(v) > 2,
		      "the staged /usr file is in place (%r)" % v)
		if pc.BAD.search(r2.out):
			check(False, "console: %r" % pc.BAD.search(r2.out).group(0))
	finally:
		r.close()
	if fails:
		print("gzapplytest: %d FAILED" % len(fails))
		return 1
	print("gzapplytest: all passed")
	return 0


if __name__ == "__main__":
	sys.exit(main())
