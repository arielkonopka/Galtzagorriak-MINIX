#!/usr/bin/env python3
"""homev3 - the O.7 layout and setup -H in QEMU (spec O.7, ACO.6 in part).

Usage: homev3.py

  1. A fresh copy of build/qemu/minix/disk.img (make minix disk, BOARD
     qemu) boots: the first boot's /etc/setup makes /home MINIX V3
     (magic 0x4D5A) with mkfs -3 -g; /home mounts read-write.
  2. As root, /home is made MINIX V2 again (mkfs /dev/hd4, an old card)
     and filled: copies of programs in a few directories.  The cksum list
     of its files is kept.  'sh /etc/setup -H' checks, writes the step
     and reboots; the boot copies /home to the swap partition.
  3. Power cut: QEMU is killed while the next step (mkfs -3 and the copy
     back) runs.  Booted again, /etc/rc goes on from that step.
  4. After it: /home is V3, every file has the same cksum as before,
     /etc/.home-v3 is gone; on the host 'fsck.minix -f' finds /home
     clean.
Exit status: the number of failures.
"""
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import v3card				# noqa: E402  (check, part4, copy_part)

powercut = v3card.powercut
check = v3card.check

FILL = ("cd /; mkdir /home/a /home/a/b /home/c && "
	"for f in sh ls cp tar find sort cksum; do cp /bin/$f /home/a/$f; "
	"cp /bin/$f /home/a/b/$f.2; done && cp /etc/rc /home/c/rc && "
	"cp /etc/setup /home/c/setup && echo hello >/home/hello && sync")
SUMS = "cd /home && find . -type f -exec cksum {} \\; | sort; cd /"
MAGIC = "dd if=/dev/hd4 bs=8 skip=131 count=1 2>/dev/null | od -x"


def main():
	r = powercut.Run(0, 0)
	keep = r.dir
	img = os.path.join(keep, "build/qemu/minix/disk.img")
	part = tempfile.mktemp(prefix="homev3")
	try:
		if not r.login():
			check(False, "first boot and login")
			return 1
		out = r.output(MAGIC, 30) or b""
		check(b"4d5a" in out, "first boot: /home is MINIX V3 (%s)"
			% out.strip().decode(errors="replace"))
		out = r.output("mount", 30) or b""
		check(b"/dev/hd4" in out and b"/home" in out, "/home mounted")

		# An old card: a V2 /home with files.
		check(r.sh("cd /; umount /dev/hd4 && mkfs /dev/hd4 && "
			"mount /dev/hd4 /home", 300) == 0, "V2 /home (mkfs)")
		check(r.sh(FILL, 300) == 0, "fill /home")
		before = r.output(SUMS, 120) or b""
		n = before.count(b"\n")
		check(n >= 15, "cksum list of %d files" % n)
		r.send("sh /etc/setup -H")
		check(r.wait(rb"converted at the next boot", 300) is not None,
			"setup -H starts")
		check(r.wait(rb"setup -H: copying /home", 600) is not None,
			"boot: copy to the swap partition")
		m = r.wait(rb"setup -H: making a MINIX V3 /home|setup -H: the copy"
			, 900)
		ok = m is not None and b"making" in m.group(0)
		check(ok, "the copy reads back the same")
		if not ok:
			return v3card.fails
		time.sleep(1.5)		# into mkfs -3 / the copy back
		r.p.kill()
		r.p.wait()
		check(True, "power cut in the mkfs -3 step")

		r = powercut.Run(0, 0, reuse=keep)
		check(r.wait(rb"setup -H: making a MINIX V3 /home", 600)
			is not None, "the next boot goes on with mkfs -3")
		check(r.wait(rb"setup -H: /home is MINIX V3 now", 900)
			is not None, "setup -H: done, every file the same")
		if not r.login():
			check(False, "login after setup -H")
			return v3card.fails + 1
		out = r.output(MAGIC, 30) or b""
		check(b"4d5a" in out, "/home is MINIX V3")
		after = r.output(SUMS, 120) or b""
		check(after == before and n > 0,
			"same %d files, same cksums" % n)
		check(r.sh("test ! -f /etc/.home-v3", 30) == 0,
			"/etc/.home-v3 removed")
		r.sh("cd /; umount /dev/hd4; sync", 60)
		r.p.kill()
		r.p.wait()
		start, size = v3card.part4(img)
		with open(part, "wb") as p:
			p.truncate(size * 512)
		v3card.copy_part(img, start, size, part, False)
		st, out = v3card.host(["fsck.minix", "-f", part])
		check(st == 0, "host fsck.minix -f: clean (%s)"
			% out.strip().splitlines()[-1:])
	finally:
		try:
			r.p.kill()
		except Exception:
			pass
		if os.path.exists(part):
			os.unlink(part)
		subprocess.call(["rm", "-rf", keep])
	return v3card.fails


if __name__ == "__main__":
	f = main()
	print("homev3: %d failure%s" % (f, "" if f == 1 else "s"))
	sys.exit(min(f, 125))
