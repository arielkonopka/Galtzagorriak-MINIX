"""twoflip_sim - two simulated Flippers on pseudo-terminals, for
"twoflip.py --selftest": checks twoflip's command lines, its waiting and its
parsing without hardware.

The Unleashed side imitates unleashed-firmware 53818b1's CLI output (prompt
">: ", "Press Ctrl+C to abort", ANSI colours, CR LF); the MINIX side the
shell with the commands of flipper/cmds (rfid, ir, od) and /dev/subghz as
they are on main.  A shared World carries what one emulates or sends to
the other.  MINIX's rfid knows only EM4100 here, as on main, so the
--rfid case is expected to FAIL until T19 lands.
"""
import os
import re
import threading
import time
import tty


class World:
	def __init__(self):
		self.lock = threading.Lock()
		self.ul_em = None	# (protocol, hex) the Unleashed one emulates
		self.mx_em = None	# hex the MINIX one emulates (EM4100)
		self.ikey = None	# bytes the Unleashed one emulates
		self.sg_tx = 0		# time the Unleashed tx ends
		self.ul = self.mx = None


class Sim(threading.Thread):
	def __init__(self, world):
		super().__init__(daemon=True)
		self.w = world
		self.master, self.slave = os.openpty()
		tty.setraw(self.master)
		self.path = os.ttyname(self.slave)
		self.line = b""
		self.busy = None	# the long command running
		self.lock = threading.Lock()
		self.stopped = False

	def out(self, s):
		with self.lock:
			os.write(self.master, s.encode() if isinstance(s, str) else s)

	def run(self):
		while not self.stopped:
			try:
				d = os.read(self.master, 1024)
			except OSError:
				return
			for c in d:
				self.char(bytes([c]))

	def char(self, c):
		if c == b"\x03":
			self.line = b""
			self.interrupt()
		elif self.busy:
			pass			# the long command does not read
		elif c in (b"\r", b"\n"):
			self.out("\r\n")
			ln, self.line = self.line.decode(), b""
			self.command(ln)
		else:
			self.line += c
			self.out(c)		# echo


class UlSim(Sim):
	def prompt(self):
		self.out("\r\n\x1b[0m>: ")

	def interrupt(self):
		b, self.busy = self.busy, None
		if b == "emulate":
			self.w.ul_em = None
			self.out("Emulation stopped\r\n")
		elif b == "ikey":
			self.w.ikey = None
		elif b == "read":
			self.out("Reading stopped\r\n")
		self.prompt()

	def ready(self, what):
		self.out(what + "\r\nPress Ctrl+C to abort\r\n")

	def command(self, ln):
		a = ln.split()
		if not a:
			return self.prompt()
		if a[:2] == ["rfid", "emulate"] and len(a) == 4:
			self.w.ul_em = (a[2], a[3])
			self.busy = "emulate"
			return self.ready("Emulating RFID...")
		if a[:3] == ["rfid", "read", "normal"]:
			self.busy = "read"
			self.ready("Reading RFID...")
			threading.Thread(target=self.read_rfid, daemon=True).start()
			return
		if a[:2] == ["ikey", "emulate"] and a[2] == "Dallas":
			self.w.ikey = bytes.fromhex(a[3])
			self.busy = "ikey"
			self.out("Emulating key Dallas %s\r\n" % a[3])
			return self.out("Press Ctrl+C to abort\r\n")
		if a == ["ir", "rx"]:
			self.busy = "irrx"
			return self.ready("Receiving  INFRARED...")
		if a[:2] == ["ir", "tx"] and len(a) == 5:
			self.w.mx.ir_in(a[2], int(a[3], 16), int(a[4], 16))
			return self.prompt()
		if a[:2] == ["subghz", "tx"]:
			self.busy = "tx"
			self.out("Transmitting at %s, key %s, te 403, repeat 100 "
				"device 0. Press CTRL+C to stop\r\n" % (a[3], a[2]))
			self.w.sg_tx = time.time() + 1.5
			threading.Thread(target=self.tx_end, daemon=True).start()
			return
		self.out("`%s` command not found\r\n" % a[0])
		self.prompt()

	def read_rfid(self):
		for _ in range(40):
			time.sleep(0.1)
			if self.busy != "read":
				return
			if self.w.mx_em:
				self.busy = None
				self.out("EM4100 %s\r\nFC: 044 Card: 15360\r\n"
					"Reading stopped\r\n" % self.w.mx_em)
				return self.prompt()

	def tx_end(self):
		time.sleep(1.5)
		if self.busy == "tx":
			self.busy = None
			self.out("....\r\n")
			self.prompt()

	def ir_in(self, p, a, c):
		if self.busy == "irrx":
			self.out("\x1b[32m%s, A:0x%02X, C:0x%02X\x1b[0m\r\n" % (p, a, c))


class MxSim(Sim):
	state = "login"		# login, password, then the shell

	def prompt(self):
		if self.state == "login":
			self.out("\r\nlogin: ")
		elif self.state == "shell":
			self.out("# ")

	def interrupt(self):
		if self.busy == "emulate":
			self.w.mx_em = None
		self.busy = None
		self.out("\r\n")
		self.prompt()

	def say(self, s):
		self.out(s.replace("\n", "\r\n"))

	def command(self, ln):
		if self.state == "login":
			if ln == "root":
				self.state = "password"
				return self.out("Password:")
			return self.prompt()
		if self.state == "password":
			self.state = "shell" if ln == "Kaixo-test1" else "login"
			self.out("\r\n")
			return self.prompt()
		m = re.match(r'(.*?)(?:; echo @@R""=\$\?)?$', ln)
		cmd, marked = m.group(1).strip(), ln.endswith('=$?')
		rc = 0
		a = cmd.split()
		if cmd == 'echo @@S""Y':
			self.say("@@SY\n")
		elif not a:
			pass
		elif a[0] == "rfid" and a[1:2] == ["emulate"]:
			self.w.mx_em = "".join(a[3:8])
			self.busy = "emulate"
			self.say("emulating EM4100 %s; hold to a reader, Ctrl-C to "
				"stop\n" % " ".join(a[3:8]))
			return
		elif a[0] == "rfid":
			time.sleep(0.5)
			e = self.w.ul_em
			if e and e[0] == "EM4100":
				h = e[1]
				self.say("EM4100 %s\n" % " ".join(h[i:i + 2]
					for i in range(0, 10, 2)))
			elif e:
				self.say("no EM4100; 812 changes, 7-90 us, mean 25 us\n")
			else:
				self.say("no tag (no signal)\n")
				rc = 1
		elif a[:3] == ["ir", "rx", "-n"]:
			self.busy = "irrx"
			return
		elif a[:2] == ["ir", "rx"]:	# -r -n 1 -d /dev/subghzraw
			time.sleep(0.3)
			if time.time() < self.w.sg_tx:
				self.say("48: 403 1209 403 1209 1209 403 77 5 403 1209"
					+ " 403" * 38 + "\nunknown\n")
			else:
				self.say("200: " + " 9" * 200 + "\n")
		elif a[:2] == ["ir", "tx"]:
			self.w.ul.ir_in(a[2], int(a[3], 0), int(a[4], 0))
		elif a[:2] == ["od", "-b"]:
			k = self.w.ikey
			if k:
				self.say("0000000 " + " ".join("%03o" % x for x in k)
					+ "\n0000010\n")
			else:
				self.say("od: read error: No such device\n")
				rc = 1
		elif a[0] == "echo" and a[-1] == "/dev/subghz":
			pass
		elif a[:2] == ["cat", "/dev/subghz"]:
			pk = -28 if time.time() < self.w.sg_tx else -93
			self.say("rssi=-96 peak=%d freq=433919830\n" % pk)
		else:
			self.say("sh: %s: not found\n" % a[0])
			rc = 127
		if marked:
			self.say("@@R=%d\n" % rc)
		self.prompt()

	def ir_in(self, p, a, c):
		if self.busy == "irrx":
			self.busy = None
			self.say("%s address 0x%02x command 0x%02x\n" % (p, a, c))
			self.prompt()


EXPECT = {
	"rfid-u2m": "PASS", "rfid-m2u": "PASS", "rfid-proto-u2m": "FAIL",
	"ir-u2m": "PASS", "ir-m2u": "PASS", "ibutton-u2m": "PASS",
	"ibutton-m2u": "SKIP", "subghz-u2m": "PASS", "subghz-raw-u2m": "INFO",
	"subghz-m2u": "SKIP", "nfc-u2m": "SKIP", "nfc-m2u": "SKIP",
}


def selftest(tf, opt):
	w = World()
	w.ul, w.mx = UlSim(w), MxSim(w)
	w.ul.start()
	w.mx.start()
	w.ul.prompt()
	w.mx.prompt()
	log = open(opt.log, "w") if opt.log else None
	ul = tf.Unleashed(w.ul.path, "U", log)
	mx = tf.Minix(w.mx.path, "M", log)
	who = (tf.probe(w.ul.path), tf.probe(w.mx.path))
	if who != ("unleashed", "login"):
		print("selftest: FAIL probe() said %s %s" % who)
		return 1
	if not ul.sync() or not tf.login(mx, "Kaixo-test1") or not mx.sync():
		print("selftest: FAIL no prompt or login on the simulated Flippers")
		return 1
	if not opt.rfid:
		opt.rfid = ["H10301:1A2B3C"]
	res = {}
	tf.run_cases(ul, mx, opt, opt.cases, res)
	bad = [n for n in res if res[n] != EXPECT[n]]
	for n in bad:
		print("selftest: %s gave %s, expected %s" % (n, res[n], EXPECT[n]))
	print("selftest: %s (%d cases)" % ("FAIL" if bad else "ok", len(res)))
	return len(bad)
