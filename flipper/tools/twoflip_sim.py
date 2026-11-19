"""twoflip_sim - two simulated Flippers on pseudo-terminals, for
"twoflip.py --selftest": checks twoflip's command lines, its waiting, its
parsing and its statuses without hardware.

The Unleashed side imitates unleashed-firmware 53818b1's CLI (prompt ">: ",
"Press Ctrl+C to abort", ANSI colours, CR LF; it refuses to transmit at
868 MHz, as with a region that forbids it).  The MINIX side imitates the
shell with what main has today: rfid (EM4100 only), ir (all 14 protocols
and raw), od on /dev/ibutton, /dev/subghz and /dev/subghzraw, ver; no
ikey, subghz or nfc command.  A shared World carries what one emulates or
sends to the other.  expected() says what each row must give then.
"""
import os
import threading
import time
import tty


class World:
	def __init__(self):
		self.ul_em = None	# (protocol, hex) the Unleashed one emulates
		self.mx_em = None	# hex the MINIX one emulates (EM4100)
		self.ikey = None	# (type, hex) the Unleashed one emulates
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

	def out(self, s):
		"""Like USB: a line can arrive in two pieces.  The last character
		before the line break comes 20 ms later, so a pattern matched on
		a line that is not complete yet ("command 0x0" of "0x08") shows
		up here as a wrong value."""
		b = s.encode() if isinstance(s, str) else s
		i = b.rfind(b"\r\n")
		with self.lock:
			if i > 1:
				os.write(self.master, b[:i - 1])
				time.sleep(0.02)
				b = b[i - 1:]
			os.write(self.master, b)

	def run(self):
		while True:
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
			ln, self.line = self.line.decode("latin-1"), b""
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
		if a[:2] == ["rfid", "read"]:
			self.busy = "read"
			self.ready("Reading RFID...")
			threading.Thread(target=self.read_rfid, daemon=True).start()
			return
		if a[:2] == ["ikey", "emulate"] and len(a) == 4:
			self.w.ikey = (a[2], a[3])
			self.busy = "ikey"
			self.out("Emulating key %s %s\r\n" % (a[2], a[3]))
			return self.out("Press Ctrl+C to abort\r\n")
		if a == ["ikey", "read"]:
			self.busy = "ikey"
			return self.out("Reading iButton...\r\nPress Ctrl+C to abort\r\n")
		if a == ["ir", "rx"]:
			self.busy = "irrx"
			return self.ready("Receiving  INFRARED...")
		if a == ["ir", "rx", "raw"]:
			self.busy = "irraw"
			return self.ready("Receiving RAW INFRARED...")
		if a[:3] == ["ir", "tx", "RAW"]:
			self.w.mx.ir_raw_in([int(x) for x in a[5:]])
			return self.prompt()
		if a[:2] == ["ir", "tx"] and len(a) == 5:
			self.w.mx.ir_in(a[2], int(a[3], 16), int(a[4], 16))
			return self.prompt()
		if a[:2] == ["subghz", "tx"]:
			if a[3] == "868350000":
				self.out("Transmission on this frequency is restricted in "
					"your settings\r\n")
				return self.prompt()
			self.busy = "tx"
			self.out("Transmitting at %s, key %s, te 403, repeat 100 "
				"device 0. Press CTRL+C to stop\r\n" % (a[3], a[2]))
			self.w.sg_tx = time.time() + 1.5
			threading.Thread(target=self.tx_end, daemon=True).start()
			return
		if a[:2] == ["info", "device"]:
			self.out("firmware.version              : unlshd-083\r\n")
			return self.prompt()
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

	def ir_raw_in(self, d):
		if self.busy == "irraw":
			self.out("RAW, %d samples:\r\n%s \r\n" % (len(d),
				" ".join(str(x + 20) for x in d)))


IR14 = ["NEC", "NECext", "NEC42", "NEC42ext", "Samsung32", "RC5", "RC5X",
	"RC6", "SIRC", "SIRC15", "SIRC20", "Kaseikyo", "RCA", "Pioneer"]


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
		marked = ln.endswith('; echo @@R""=$?')
		cmd = ln[:-len('; echo @@R""=$?')] if marked else ln
		rc = self.do(cmd.strip())
		if rc is None:		# a long command: it prompts itself
			return
		if marked:
			self.say("@@R=%d\n" % rc)
		self.prompt()

	def do(self, cmd):
		a = cmd.split()
		if cmd == 'echo @@S""Y':
			self.say("@@SY\n")
			return 0
		if not a:
			return 0
		if a[0] == "rfid" and a[1:2] in (["emulate"], ["write"]):
			if len(a) != 8 or a[2] != "EM":
				self.say("usage: rfid [-r] [-n TRIES]\n")
				return 1
			self.w.mx_em = "".join(a[3:8])
			self.busy = "emulate"
			self.say("emulating EM4100 %s; hold to a reader, Ctrl-C to "
				"stop\n" % " ".join(a[3:8]))
			return None
		if a[0] == "rfid":
			time.sleep(0.3)
			e = self.w.ul_em
			if e and e[0] == "EM4100":
				h = e[1]
				self.say("EM4100 %s\n" % " ".join(h[i:i + 2]
					for i in range(0, 10, 2)))
				return 0
			if e:
				self.say("no EM4100; 812 changes, 7-90 us, mean 25 us\n")
			else:
				self.say("no tag (no signal)\n")
			return 1
		if a[:2] == ["ir", "rx"] and "/dev/subghzraw" in a:
			time.sleep(0.3)
			if time.time() < self.w.sg_tx:
				self.say("48: 403 1209 403 1209 1209 403 77 5 403 1209"
					+ " 403" * 38 + "\nunknown\n")
			else:
				self.say("200: " + " 9" * 200 + "\n")
			return 0
		if a[:2] == ["ir", "rx"]:
			self.busy = "irraw" if "-r" in a else "irrx"
			return None
		if a[:3] == ["ir", "tx", "raw"]:
			self.w.ul.ir_raw_in([int(x) for x in a[3:]])
			return 0
		if a[:2] == ["ir", "tx"] and len(a) == 5:
			if a[2] not in IR14:
				self.say("Usage: ir rx [-r] [-n COUNT] [-d DEV]\n")
				return 1
			self.w.ul.ir_in(a[2], int(a[3], 0), int(a[4], 0))
			return 0
		if a[:2] == ["od", "-b"]:
			k = self.w.ikey
			if k and k[0] == "Dallas":
				b = bytes.fromhex(k[1])
				self.say("0000000 " + " ".join("%03o" % x for x in b)
					+ "\n0000010\n")
				return 0
			self.say("od: read error: No such device\n")
			return 1
		if a[0] == "echo" and a[-1] == "/dev/subghz":
			return 0
		if a[:2] == ["cat", "/dev/subghz"]:
			pk = -28 if time.time() < self.w.sg_tx else -93
			self.say("rssi=-96 peak=%d freq=433919830\n" % pk)
			return 0
		if a[0] == "ver":
			self.say("Galtzagorriak MINIX 0.1.1 (sim)\n")
			return 0
		if a[0] not in ("echo",):
			self.say("sh: %s: not found\n" % a[0])
			return 127
		return 0

	def ir_in(self, p, a, c):
		if self.busy == "irrx":
			self.busy = None
			self.say("%s address 0x%02x command 0x%02x\n" % (p, a, c))
			self.prompt()

	def ir_raw_in(self, d):
		if self.busy == "irraw":
			self.busy = None
			self.say("%d: %s\nunknown\n" % (len(d), " ".join(str(x - 30)
				for x in d)))
			self.prompt()


def expected(rid):
	"""What each row gives against this simulation."""
	if rid.startswith("rfid-write") or rid.startswith("ibutton-write"):
		return "N/A"
	if rid in ("rfid-EM4100-u2m", "rfid-EM4100-m2u"):
		return "PASS"
	if rid == "rfid-Radio_Key-u2m":
		return "BLOCKED"
	if rid.startswith("rfid-"):
		return "MISSING"
	if rid.startswith("ir-"):
		return "PASS"
	if rid == "ibutton-Dallas_dev_ibutton-u2m":
		return "PASS"
	if rid.startswith("ibutton-"):
		return "MISSING"
	if rid.startswith("subghz-level_868"):
		return "BLOCKED"
	if rid.startswith("subghz-level_315"):
		return "BLOCKED"	# not an EU band: twoflip refuses it (--region)
	if rid.startswith("subghz-level"):
		return "PASS"
	if rid == "subghz-raw_timings-u2m":
		return "INFO"
	if rid.startswith("modem-"):
		return "N/A"		# needs --peer-minix (spec section P)
	return "MISSING"		# subghz tx/chat/decode, nfc


def selftest(tf, opt, rows):
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
	tf.finish(ul, mx, opt, rows)
	bad = [r for r in opt.results if r["status"] != expected(r["id"])]
	for r in bad:
		print("selftest: %s gave %s, expected %s" % (r["id"], r["status"],
			expected(r["id"])))
	print("selftest: %s (%d rows)" % ("FAIL" if bad else "ok",
		len(opt.results)))
	return len(bad)
