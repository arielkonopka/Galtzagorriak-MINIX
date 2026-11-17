#!/usr/bin/env python3
"""twoflip - paired tests between a Galtzagorriak MINIX Flipper and a stock
(Unleashed) Flipper: one sends or emulates, the other receives or reads.

Usage: twoflip.py [-m PORT] [-u PORT] [-r N] [--log FILE] [CASE ...]
       twoflip.py --list | --ports | --selftest

Both Flippers hang on the same PC by USB.  The MINIX one is logged in as
root (if it shows "login:", twoflip logs in: --password, the environment
variable TWOFLIP_PASSWORD, or it asks); the Unleashed one answers at its CLI
(">: ").  Close qFlipper, screen, PuTTY and anything else that holds a
port.  The physical setup and the installation on a new computer are in
docs/devguide/part2/06-two-flipper-tests.md.

Linux, macOS and Windows.  pyserial is needed on Windows and for --ports
on any system ("pip install pyserial"); on Linux and macOS the script runs
without it.  Without -m/-u both ports are found by asking each Flipper-like
port who answers (both firmwares have the USB id 0483:5740); --ports shows
what it finds.  Without CASE names all
cases that can run on the main branch today run; --list shows them all,
the planned ones too (they are reported as SKIP with the reason).

Directions: "U>M" the Unleashed Flipper sends or emulates and MINIX reads,
"M>U" the other way round.

  -r N            run each case N times, report k/N (distance, angle and
                  timing problems show as a pass rate below N/N)
  --em HEX        the EM4100 number to use (10 hex digits, 1A2B3C4D5E)
  --rfid P:HEX    also: Unleashed emulates protocol P with data HEX and
                  MINIX's rfid must name it (spec 9.0 / T19, issue #210),
                  e.g. --rfid H10301:1A2B3C; repeatable
  --ir P:A:C      the IR messages to send both ways instead of the
                  default set (hex address and command); repeatable
  --freq HZ       Sub-GHz frequency (433920000); must be allowed for
                  transmitting in the Unleashed region settings
  --log FILE      everything both consoles said, with times

Exit status: the number of failed cases (INFO and SKIP do not count).

The Unleashed command names and output formats were checked against
unleashed-firmware commit 53818b1 (applications/main/*/..._cli.c); see
the guide for the list.  --selftest runs the cases against two simulated
Flippers on pseudo-terminals (no hardware): it checks this script's own
parsing.
"""
import argparse
import glob
import os
import re
import select
import sys
import time

try:
	import termios
except ImportError:		# Windows
	termios = None
try:
	import serial
	import serial.tools.list_ports
except ImportError:
	serial = None

ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")

UL_PROMPT = rb">: "
UL_READY = rb"Press (?:Ctrl|CTRL)\+C"


class Port:
	"""A serial console: everything read is kept in buf; wait() looks only
	at what came after self.pos (set before each command).  pyserial when
	it is installed (Windows needs it), else termios (Linux, macOS)."""

	def __init__(self, path, name, log):
		self.name = name
		self.log = log
		self.path = path
		self.buf = b""
		self.pos = 0
		self.ser = None
		if serial is not None:
			self.ser = serial.Serial(path, 115200, timeout=0)
			return
		if termios is None:
			sys.exit("twoflip: install pyserial (pip install pyserial)")
		self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
		if os.isatty(self.fd):
			t = termios.tcgetattr(self.fd)
			t[0] = 0
			t[1] = 0
			t[3] = 0
			t[2] |= termios.CLOCAL | termios.CREAD
			termios.tcsetattr(self.fd, termios.TCSANOW, t)

	def close(self):
		if self.ser:
			self.ser.close()
		else:
			os.close(self.fd)

	def _logit(self, tag, data):
		if self.log:
			self.log.write("%9.3f %s%s %r\n" % (time.time() % 1e5,
				self.name, tag, data))
			self.log.flush()

	def _read(self, secs):
		d = b""
		if self.ser:
			self.ser.timeout = secs
			d = self.ser.read(max(1, self.ser.in_waiting))
		elif select.select([self.fd], [], [], secs)[0]:
			try:
				d = os.read(self.fd, 4096)
			except OSError:
				d = b""
		if d:
			self._logit("<", d)
			self.buf += ANSI.sub(b"", d)
			return True
		return False

	def write(self, s):
		if isinstance(s, str):
			s = s.encode()
		self._logit(">", s)
		if self.ser:
			self.ser.write(s)
			self.ser.flush()
		else:
			os.write(self.fd, s)

	def mark(self):
		self._read(0)
		while self._read(0.05):
			pass
		self.pos = len(self.buf)

	def wait(self, pat, secs):
		"""The first match of pat after pos (pos moves past it), or None
		after secs."""
		end = time.time() + secs
		rx = re.compile(pat)
		while True:
			m = rx.search(self.buf, self.pos)
			if m:
				self.pos = m.end()
				return m
			left = end - time.time()
			if left <= 0:
				return None
			self._read(min(left, 0.1))

	def text(self, start):
		return self.buf[start:].decode("latin-1").replace("\r", "")


class Unleashed(Port):
	"""The stock CLI: commands end with the ">: " prompt; the long ones
	(emulate, rx, read) say "Press Ctrl+C to abort" and stop at Ctrl-C."""

	def sync(self):
		self.write(b"\x03")
		time.sleep(0.2)
		self.mark()
		self.write(b"\r")
		return self.wait(UL_PROMPT, 5) is not None

	def run(self, cmd, secs=10):
		self.mark()
		start = self.started = self.pos
		self.write(cmd + "\r")
		ok = self.wait(UL_PROMPT, secs) is not None
		if not ok:
			self.stop()
		return ok, self.text(start)

	def start(self, cmd, ready=UL_READY, secs=5):
		self.mark()
		self.started = self.pos
		self.write(cmd + "\r")
		return self.wait(ready, secs) is not None

	def stop(self):
		self.write(b"\x03")
		self.wait(UL_PROMPT, 5)
		return self.text(self.started)


class Minix(Port):
	"""The MINIX shell: each command is followed by an echo of a marker
	with the exit status; the marker is split by "" so the typed line's
	echo never matches it."""

	def sync(self):
		self.write(b"\x03")
		time.sleep(0.2)
		self.mark()
		self.write('\recho @@S""Y\r')
		return self.wait(rb"@@SY", 5) is not None

	def run(self, cmd, secs=10):
		self.mark()
		start = self.pos
		self.write(cmd + '; echo @@R""=$?\r')
		m = self.wait(rb"@@R=(\d+)", secs)
		if not m:
			self.sync()
			return None, self.text(start)
		return int(m.group(1)), self.text(start)

	def start(self, cmd, ready=None, secs=5):
		self.mark()
		self.started = self.pos
		self.write(cmd + "\r")
		if ready is None:
			time.sleep(0.5)
			return True
		return self.wait(ready, secs) is not None

	def stop(self):
		self.sync()
		return self.text(self.started)


# --- helpers -------------------------------------------------------------

def crc8_dallas(data):
	crc = 0
	for b in data:
		for _ in range(8):
			mix = (crc ^ b) & 1
			crc >>= 1
			if mix:
				crc ^= 0x8C
			b >>= 1
	return crc


def od_bytes(text):
	"""The bytes in "od -b" output (7-digit octal offset, 3-digit bytes)."""
	out = []
	for line in text.split("\n"):
		t = line.split()
		if t and re.fullmatch(r"[0-7]{7}", t[0]):
			out += [int(x, 8) for x in t[1:] if re.fullmatch(r"[0-7]{3}", x)]
	return out


def hexs(b):
	return " ".join("%02X" % x for x in b)


def ir_msgs(opt):
	if opt.ir:
		res = []
		for s in opt.ir:
			p, a, c = s.split(":")
			res.append((p, int(a, 16), int(c, 16)))
		return res
	# one of each family the MINIX ir command and Unleashed share
	return [("NEC", 0x04, 0x08), ("NECext", 0x1234, 0x5678),
		("Samsung32", 0x07, 0x02), ("RC5", 0x01, 0x10),
		("RC6", 0x00, 0x0C), ("SIRC", 0x01, 0x15)]


MX_IR = rb"(\w+) address 0x([0-9a-fA-F]+) command 0x([0-9a-fA-F]+)"
UL_IR = rb"(\w+), A:0x([0-9A-Fa-f]+), C:0x([0-9A-Fa-f]+)"
MX_SG = rb"rssi=(-?\d+)(?: peak=(-?\d+))?"


# --- the cases -----------------------------------------------------------
# Each returns (status, detail); status PASS, FAIL, INFO or SKIP.

def rfid_u2m(ul, mx, opt):
	em = opt.em.upper()
	if not ul.start("rfid emulate EM4100 " + em):
		ul.stop()
		return "FAIL", "Unleashed did not start emulating"
	rc, out = mx.run("rfid -n 8", 20)
	ul.stop()
	m = re.search(r"EM4100 ((?:[0-9A-F]{2} ?){5})", out)
	if not m:
		last = out.strip().split("\n")[-2:] if out.strip() else ["(nothing)"]
		return "FAIL", "MINIX read no EM4100: " + " | ".join(last)
	got = m.group(1).replace(" ", "")
	if got != em:
		return "FAIL", "MINIX read %s, expected %s" % (got, em)
	return "PASS", "MINIX read EM4100 " + got


def rfid_m2u(ul, mx, opt):
	em = opt.em.upper()
	spaced = " ".join(em[i:i + 2] for i in range(0, 10, 2))
	if not mx.start("rfid emulate EM " + spaced, rb"emulating EM4100"):
		mx.stop()
		return "FAIL", "MINIX did not start emulating"
	ok = ul.start("rfid read normal", rb"Reading RFID")
	got = ul.wait(rb"Reading stopped", 10) if ok else None
	out = ul.stop() if got is None else ul.text(ul.started)
	mx.stop()
	m = re.search(r"\bEM4100 ([0-9A-F]{10})\b", out)
	if not m:
		return "FAIL", "Unleashed read no EM4100 in 10 s"
	if m.group(1) != em:
		return "FAIL", "Unleashed read %s, expected %s" % (m.group(1), em)
	return "PASS", "Unleashed read EM4100 " + m.group(1)


def rfid_proto_u2m(ul, mx, opt):
	if not opt.rfid:
		return "SKIP", "no --rfid PROTO:HEX given"
	bad = []
	for s in opt.rfid:
		p, data = s.rsplit(":", 1)
		if not ul.start("rfid emulate %s %s" % (p, data.upper())):
			ul.stop()
			bad.append("%s: Unleashed refused (name or data length)" % p)
			continue
		rc, out = mx.run("rfid -n 8", 20)
		ul.stop()
		flat = re.sub(r"[\s:]", "", out).upper()
		if p.upper().replace(" ", "") in flat and data.upper() in flat:
			continue
		bad.append("%s: MINIX said %r" % (p, out.strip().split("\n")[-1]))
	if bad:
		return "FAIL", "; ".join(bad)
	return "PASS", "%d protocols read by MINIX" % len(opt.rfid)


def ir_u2m(ul, mx, opt):
	bad = []
	for p, a, c in ir_msgs(opt):
		got = None
		for _ in range(3):
			mx.start("ir rx -n 1")
			ul.run("ir tx %s %X %X" % (p, a, c), 5)
			got = mx.wait(MX_IR, 2)
			mx.stop()
			if got:
				break
		if not got:
			bad.append("%s: nothing decoded" % p)
			continue
		g = (got.group(1).decode(), int(got.group(2), 16),
			int(got.group(3), 16))
		if g != (p, a, c):
			bad.append("%s %X %X: got %s %X %X" % ((p, a, c) + g))
	if bad:
		return "FAIL", "; ".join(bad)
	return "PASS", "%d protocols" % len(ir_msgs(opt))


def ir_m2u(ul, mx, opt):
	bad = []
	for p, a, c in ir_msgs(opt):
		got = None
		for _ in range(3):
			if not ul.start("ir rx"):
				ul.stop()
				return "FAIL", "Unleashed 'ir rx' did not start"
			mx.run("ir tx %s 0x%x 0x%x" % (p, a, c), 5)
			got = ul.wait(UL_IR, 2)
			ul.stop()
			if got:
				break
		if not got:
			bad.append("%s: nothing decoded" % p)
			continue
		g = (got.group(1).decode(), int(got.group(2), 16),
			int(got.group(3), 16))
		if g != (p, a, c):
			bad.append("%s %X %X: got %s %X %X" % ((p, a, c) + g))
	if bad:
		return "FAIL", "; ".join(bad)
	return "PASS", "%d protocols" % len(ir_msgs(opt))


def ibutton_u2m(ul, mx, opt):
	key = [0x01, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6]
	key.append(crc8_dallas(key))
	if not ul.start("ikey emulate Dallas " + hexs(key).replace(" ", "")):
		ul.stop()
		return "FAIL", "Unleashed did not start emulating"
	got, last = None, ""
	for _ in range(10):			# one read is one try (~10 ms)
		rc, out = mx.run("od -b /dev/ibutton", 5)
		b = od_bytes(out)
		if len(b) == 8:
			got = b
			break
		last = out.strip().split("\n")[-2] if "\n" in out.strip() \
			else out.strip()
		time.sleep(0.3)
	ul.stop()
	if not got:
		return "FAIL", "MINIX read no ROM in 10 tries (last: %s); " \
			"pin 17 and GND wired?" % last
	if got != key:
		return "FAIL", "MINIX read %s, expected %s" % (hexs(got), hexs(key))
	return "PASS", "MINIX read ROM " + hexs(got)


def _sg_level(mx):
	rc, out = mx.run("cat /dev/subghz", 5)
	m = re.search(MX_SG.decode(), out)
	if not m:
		return None
	return max(int(m.group(1)), int(m.group(2) or m.group(1)))


def subghz_u2m(ul, mx, opt):
	rc, out = mx.run("echo %d > /dev/subghz" % opt.freq, 5)
	if rc != 0:
		return "FAIL", "MINIX refused %d Hz: %s" % (opt.freq, out.strip())
	quiet = [x for x in (_sg_level(mx) for _ in range(3)) if x is not None]
	if not quiet:
		mx.run("echo 0 > /dev/subghz")
		return "FAIL", "no rssi line from /dev/subghz"
	base = max(quiet)
	if not ul.start("subghz tx 74BADE %d 403 100 0" % opt.freq,
			rb"Transmitting at|restricted|only be used for RX|"
			rb"must be in|failed"):
		ul.stop()
		mx.run("echo 0 > /dev/subghz")
		return "FAIL", "Unleashed did not start transmitting"
	if not re.search(rb"Transmitting at", ul.buf[ul.started:]):
		ul.stop()
		mx.run("echo 0 > /dev/subghz")
		return "SKIP", "Unleashed may not transmit at %d Hz here " \
			"(region settings)" % opt.freq
	loud = [x for x in (_sg_level(mx) for _ in range(6)) if x is not None]
	if ul.wait(UL_PROMPT, 15) is None:
		ul.stop()
	mx.run("echo 0 > /dev/subghz")
	best = max(loud) if loud else -200
	detail = "quiet %d dBm, during Unleashed tx %d dBm" % (base, best)
	if best >= -60 and best - base >= 15:
		return "PASS", detail
	return "FAIL", detail


def subghz_raw_u2m(ul, mx, opt):
	"""Informational: /dev/subghzraw has no RSSI threshold yet, so noise
	shares the buffer with the Princeton frames (TE 403 us)."""
	mx.run("echo %d > /dev/subghz" % opt.freq, 5)
	if not ul.start("subghz tx 74BADE %d 403 100 0" % opt.freq,
			rb"Transmitting at"):
		ul.stop()
		mx.run("echo 0 > /dev/subghz")
		return "SKIP", "Unleashed did not transmit (region settings?)"
	rc, out = mx.run("ir rx -r -n 1 -d /dev/subghzraw", 10)
	if ul.wait(UL_PROMPT, 15) is None:
		ul.stop()
	mx.run("echo 0 > /dev/subghz")
	m = re.search(r"^(\d+):((?: \d+)+)", out, re.M)
	if not m:
		return "INFO", "no raw durations from /dev/subghzraw"
	d = [int(x) for x in m.group(2).split()]
	near = sum(1 for x in d if 280 <= x <= 530 or 1000 <= x <= 1420)
	return "INFO", "%d durations, %d near TE/3TE (403/1209 us)" % (
		len(d), near)


PLANNED = {
	"ibutton-m2u": "MINIX cannot emulate an iButton key yet (spec R6.7, "
		"later parts); Unleashed side: ikey read",
	"subghz-m2u": "MINIX /dev/subghz has no transmit path yet (spec R8.1 "
		"later parts); Unleashed side: subghz rx FREQ 0",
	"nfc-u2m": "no NFC driver on MINIX yet (spec Phase 14); Unleashed "
		"side: nfc, then emulate -f /ext/nfc/FILE.nfc",
	"nfc-m2u": "no NFC driver on MINIX yet (spec Phase 14); Unleashed "
		"side: nfc, then scanner",
}

CASES = [
	("rfid-u2m", "U>M", "125 kHz EM4100: Unleashed emulates, MINIX "
		"rfid reads", rfid_u2m),
	("rfid-m2u", "M>U", "125 kHz EM4100: MINIX emulates, Unleashed "
		"rfid read normal", rfid_m2u),
	("rfid-proto-u2m", "U>M", "125 kHz, other protocols (--rfid, T19)",
		rfid_proto_u2m),
	("ir-u2m", "U>M", "infrared: Unleashed ir tx, MINIX ir rx", ir_u2m),
	("ir-m2u", "M>U", "infrared: MINIX ir tx, Unleashed ir rx", ir_m2u),
	("ibutton-u2m", "U>M", "1-Wire (pin 17): Unleashed ikey emulate "
		"Dallas, MINIX od /dev/ibutton", ibutton_u2m),
	("ibutton-m2u", "M>U", "1-Wire: MINIX emulates a key", None),
	("subghz-u2m", "U>M", "Sub-GHz: Unleashed subghz tx, MINIX "
		"/dev/subghz rssi rises", subghz_u2m),
	("subghz-raw-u2m", "U>M", "Sub-GHz raw: MINIX /dev/subghzraw "
		"timings (INFO only)", subghz_raw_u2m),
	("subghz-m2u", "M>U", "Sub-GHz: MINIX transmits", None),
	("nfc-u2m", "U>M", "NFC: Unleashed emulates, MINIX reads", None),
	("nfc-m2u", "M>U", "NFC: MINIX emulates, Unleashed reads", None),
]


def candidates():
	"""Serial ports that may be a Flipper: [(port, description)].  Both
	firmwares use the USB id 0483:5740; the manufacturer string
	("Flipper Devices Inc." or "Galtzagorriak") is not shown on every
	system, so the ports are told apart by probe()."""
	res = []
	if serial is not None:
		for p in serial.tools.list_ports.comports():
			d = " ".join(str(x) for x in (p.manufacturer, p.product,
				p.description) if x)
			if (p.vid, p.pid) == (0x0483, 0x5740) or "Flipper" in d \
					or "Galtzagorriak" in d:
				res.append((p.device, "%s [%s]" % (d, p.hwid)))
		return res
	for p in sorted(glob.glob("/dev/serial/by-id/*")):
		if "Flipper" in p or "Galtzagorriak" in p:
			res.append((os.path.realpath(p), os.path.basename(p)))
	if not res:
		for p in sorted(glob.glob("/dev/ttyACM*")
				+ glob.glob("/dev/cu.usbmodem*")):
			res.append((p, ""))
	return res


def probe(path):
	"""Who answers on path: "unleashed", "minix" (root shell), "login"
	(MINIX, nobody logged in), "busy: ..." or None."""
	try:
		pt = Port(path, "?", None)
	except (OSError, ValueError) as e:
		return "busy: %s" % e
	try:
		pt.write(b"\x03\r")
		m = pt.wait(rb">: |# |\$ |login: |Password:", 3)
	finally:
		pt.close()
	if not m:
		return None
	return {b">: ": "unleashed", b"# ": "minix", b"$ ": "user shell"
		}.get(m.group(0), "login")


def login(mx, password):
	"""Log in as root on the MINIX console if it shows "login:"."""
	mx.mark()
	mx.write(b"\x03\r")
	m = mx.wait(rb"login: |# |\$ ", 5)
	if m and m.group(0) == b"$ ":
		mx.write(b"exit\r")
		m = mx.wait(rb"login: ", 5)
	if not m or m.group(0) != b"login: ":
		return m is not None and m.group(0) == b"# "
	if password is None:
		import getpass
		password = getpass.getpass("MINIX root password: ")
	mx.write(b"root\r")
	if mx.wait(rb"Password:|# ", 10) is None:
		return False
	mx.write(password + "\r")
	return mx.wait(rb"# ", 20) is not None


def run_cases(ul, mx, opt, names, results=None):
	fails = 0
	sel = [c for c in CASES if not names or c[0] in names]
	for name, dirn, desc, fn in sel:
		if fn is None:
			print("SKIP  %-15s %s  %s" % (name, dirn, PLANNED[name]),
				flush=True)
			if results is not None:
				results[name] = "SKIP"
			continue
		res = []
		for i in range(opt.repeat):
			ul.sync()
			mx.sync()
			t0 = time.time()
			try:
				st, det = fn(ul, mx, opt)
			except (OSError, ValueError) as e:
				st, det = "FAIL", "error: %s" % e
			res.append(st)
			if opt.repeat > 1:
				print("      %-15s #%d %s %s (%.1f s)" % (name, i + 1,
					st, det, time.time() - t0), flush=True)
		npass = res.count("PASS")
		if "FAIL" in res:
			st = "FAIL"
			fails += 1
		else:
			st = res[0]
		tail = det if opt.repeat == 1 else "%d/%d passed" % (
			npass, opt.repeat)
		print("%-5s %-15s %s  %s" % (st, name, dirn, tail), flush=True)
		if results is not None:
			results[name] = st
	return fails


def main():
	ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
	ap.add_argument("-m", "--minix", help="MINIX Flipper's serial port")
	ap.add_argument("-u", "--unleashed", help="Unleashed Flipper's port")
	ap.add_argument("-r", "--repeat", type=int, default=1)
	ap.add_argument("--em", default="1A2B3C4D5E")
	ap.add_argument("--rfid", action="append")
	ap.add_argument("--ir", action="append")
	ap.add_argument("--freq", type=int, default=433920000)
	ap.add_argument("--log")
	ap.add_argument("--list", action="store_true")
	ap.add_argument("--ports", action="store_true")
	ap.add_argument("--password")
	ap.add_argument("--selftest", action="store_true")
	ap.add_argument("cases", nargs="*")
	opt = ap.parse_args()

	if opt.list:
		for name, dirn, desc, fn in CASES:
			print("%-15s %s  %s%s" % (name, dirn, desc,
				"" if fn else "  (planned)"))
		return 0
	known = {c[0] for c in CASES}
	for c in opt.cases:
		if c not in known:
			sys.exit("twoflip: no case %s (see --list)" % c)
	if not re.fullmatch(r"[0-9A-Fa-f]{10}", opt.em):
		sys.exit("twoflip: --em takes 10 hex digits")
	if opt.selftest:
		import importlib.util
		spec = importlib.util.spec_from_file_location("twoflip_sim",
			os.path.join(os.path.dirname(os.path.abspath(__file__)),
			"twoflip_sim.py"))
		sim = importlib.util.module_from_spec(spec)
		spec.loader.exec_module(sim)
		return sim.selftest(sys.modules[__name__], opt)

	if opt.ports:
		for path, desc in candidates():
			print("%-14s %-10s %s" % (path, probe(path), desc))
		return 0
	mport, uport = opt.minix, opt.unleashed
	if not mport or not uport:
		for path, desc in candidates():
			if path in (mport, uport):
				continue
			who = probe(path)
			if who == "unleashed" and not uport:
				uport = path
			elif who in ("minix", "login", "user shell") and not mport:
				mport = path
	if not mport or not uport:
		sys.exit("twoflip: did not find both Flippers (MINIX: %s, "
			"Unleashed: %s); run with --ports, then give -m and -u"
			% (mport, uport))
	print("MINIX on %s, Unleashed on %s" % (mport, uport), flush=True)
	log = open(opt.log, "w") if opt.log else None
	mx = Minix(mport, "M", log)
	ul = Unleashed(uport, "U", log)
	if not ul.sync():
		sys.exit("twoflip: no '>: ' prompt from the Unleashed Flipper on "
			+ uport)
	if not login(mx, opt.password or os.environ.get("TWOFLIP_PASSWORD")) \
			or not mx.sync():
		sys.exit("twoflip: no root shell on the MINIX Flipper on %s"
			% mport)
	return run_cases(ul, mx, opt, opt.cases)


if __name__ == "__main__":
	sys.exit(main())
