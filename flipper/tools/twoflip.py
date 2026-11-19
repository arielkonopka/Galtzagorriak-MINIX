#!/usr/bin/env python3
"""twoflip - functionality coverage map of Galtzagorriak MINIX against a
stock (Unleashed) Flipper: every two-device feature the Unleashed firmware
has, tried in both directions, one Flipper sending or emulating, the other
receiving or reading.

Usage: twoflip.py [-m PORT] [-u PORT] [-r N] [--map FILE] [--baseline FILE]
                  [--log FILE] [--t5577] [--rw1990] [PATTERN ...]
       twoflip.py --peer-minix [-m PORT] [-u PORT] [-r N] [modem...]
       twoflip.py --list [PATTERN ...] | --ports | --selftest

Both Flippers hang on the same PC by USB.  The MINIX one is logged in as
root (if it shows "login:", twoflip logs in: --password, the environment
variable TWOFLIP_PASSWORD, or it asks); the Unleashed one answers at its CLI
(">: ") and has its SD card in.  Close qFlipper, screen, PuTTY and anything
else that holds a port.  The physical setup and the installation on a new
computer are in docs/devguide/part2/06-two-flipper-tests.md.

Every row ends with one status:
  PASS     the other side got exactly what was sent
  FAIL     MINIX has the feature but the result is wrong (or a side it
           drives got nothing)
  MISSING  MINIX does not have it yet: the command or option is not there,
           or MINIX received the signal but has no decoder for it
  BLOCKED  the Unleashed side could not do its part here (region settings,
           its CLI cannot express it); the reason is given
  N/A      needs extra hardware that was not offered (--t5577, --rw1990)
  INFO     a measurement only
Exit status: the number of FAILs, plus with --baseline the rows that
passed in the baseline and do not now.

Where MINIX does not have a feature yet, the row uses the command the
spec plans for it, which mirrors the stock CLI (spec R9.0.3 for rfid; the
stock "ikey", "subghz" and "nfc" commands for the rest; see the guide).

PATTERN selects rows by id (shell-style, or a prefix): "rfid", "ir-*-m2u",
"subghz-decode-CAME*".  Options:
  -r N            run each row N times; a row that passed only sometimes is
                  FAIL "k/N passed"
  --map FILE      write the coverage map as Markdown to FILE and the rows
                  as JSON to FILE with .json
  --baseline F    a .json from an earlier --map: report what changed
  --t5577         a blank T5577 tag is at hand: run the RFID write rows
                  (interactive: the script asks you to move the tag)
  --rw1990        a blank RW1990 key is at hand: run the iButton write rows
  --freq HZ       Sub-GHz frequency of the raw and Princeton rows
  --log FILE      everything both consoles said, with times
  --peer-minix    the second Flipper (-u) runs MINIX too: only the "modem"
                  rows run (spec section P: data modems between two MINIX
                  Flippers, which the Unleashed firmware does not have);
                  without it those rows are N/A

Linux, macOS and Windows.  pyserial is needed on Windows and for --ports
("pip install pyserial"); on Linux and macOS the script runs without it.
Without -m/-u both ports are found by asking each Flipper-like port who
answers (both firmwares have the USB id 0483:5740).

The Unleashed commands and output formats were checked against
unleashed-firmware commit 53818b1 (applications/main/*/..._cli.c).  The
Sub-GHz and NFC test files in twoflip_data/ are Unleashed's own unit-test
files (GPLv3).  --selftest runs every row against two simulated Flippers on
pseudo-terminals (twoflip_sim.py): it checks this script, not the Flippers.
"""
import argparse
import fnmatch
import glob
import json
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

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "twoflip_data")
ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")

UL_PROMPT = rb">: "
UL_READY = rb"Press (?:Ctrl|CTRL)\+C"
UL_DIR = "/ext/twoflip"
MX_TMP = "/tmp"
MX_MISSING = re.compile(r"not found|usage:|unknown protocol|unknown command|"
	r"not supported|not implemented", re.I)

STATUSES = ["PASS", "FAIL", "MISSING", "BLOCKED", "N/A", "INFO"]
WORST = ["FAIL", "MISSING", "BLOCKED", "N/A", "INFO", "PASS"]


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
		self.started = 0
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
			s = s.encode("latin-1")
		self._logit(">", s)
		if self.ser:
			self.ser.write(s)
			self.ser.flush()
		else:
			for i in range(0, len(s), 1024):
				os.write(self.fd, s[i:i + 1024])

	def mark(self):
		self._read(0)
		while self._read(0.05):
			pass
		self.pos = len(self.buf)

	def wait(self, pat, secs):
		"""The first match of pat after pos (pos moves past it), or None
		after secs.  USB delivers a line in pieces: a pattern that ends
		in a number must also require the character after it ((?=\\D),
		(?=[^0-9A-F]), (?=\\r?\\n)), or "0x08" can match as "0x0" and
		"@@R=127" as "@@R=12" (seen on the device; the simulator splits
		lines to catch it)."""
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
	"""The stock CLI: commands end with the ">: " prompt ("[nfc]>: " in the
	nfc subshell); the long ones (emulate, rx, read) say "Press Ctrl+C to
	abort" and stop at Ctrl-C."""

	def sync(self):
		self.write(b"\x03")
		time.sleep(0.2)
		self.mark()
		start = self.pos
		self.write(b"\r")
		if self.wait(UL_PROMPT, 5) is None:
			return False
		if "[nfc]" in self.text(start):	# left in the nfc subshell
			self.run("exit", 5)
		return True

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

	def upload(self, path, data):
		"""Write data to a file on the Unleashed SD card (storage
		write_chunk appends, so the old file goes first)."""
		self.run("storage mkdir " + UL_DIR, 5)
		self.run("storage remove " + path, 5)
		self.mark()
		self.started = self.pos
		self.write("storage write_chunk %s %d\r" % (path, len(data)))
		if self.wait(rb"Ready", 5) is None:
			self.stop()
			return False
		self.write(data)
		return self.wait(UL_PROMPT, 20) is not None


class Minix(Port):
	"""The MINIX shell: each command is followed by an echo of a marker
	with the exit status; the marker is split by "" so the typed line's
	echo never matches it."""

	def __init__(self, *a):
		super().__init__(*a)
		self.have = {}

	def sync(self):
		self.write(b"\x03")
		time.sleep(0.2)
		self.mark()
		self.write('\recho @@S""Y\r')
		return self.wait(rb"@@SY", 5) is not None

	def run(self, cmd, secs=10, body=None):
		"""Run cmd; body: lines for a here document (cmd ends with
		<<'TFEOF')."""
		self.mark()
		start = self.started = self.pos
		self.write(cmd + '; echo @@R""=$?\r')
		if body is not None:
			for ln in body.split("\n"):
				self.write(ln + "\r")
			self.write("TFEOF\r")
		m = self.wait(rb"@@R=(\d+)(?=\D)", secs)
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

	def exists(self, word):
		"""Whether the command word exists (127 = not found), cached."""
		if word not in self.have:
			rc, out = self.run(word + " </dev/null >/dev/null 2>&1", 10)
			self.have[word] = rc is not None and rc != 127
		return self.have[word]

	def upload(self, path, text):
		rc, out = self.run("cat > %s <<'TFEOF'" % path, 30,
			text.rstrip("\n"))
		return rc == 0


def missing(rc, out):
	return rc == 127 or MX_MISSING.search(out or "") is not None


def last(out, n=1):
	"""The last n lines a command printed (no prompts, echo or markers)."""
	ls = [x.strip() for x in (out or "").strip().split("\n")
		if x.strip() and "@@" not in x and not re.match(r"\s*(#|>:|\[nfc\]>:)", x)
		and not x.strip().endswith(">:")]
	return " | ".join(ls[-n:]) if ls else "(nothing)"


def compact(s):
	return re.sub(r"[\s:]", "", s).upper()


# --- test vectors --------------------------------------------------------

# Unleashed lib/lfrfid/protocols (53818b1): name, decoded data bytes.
# EM4100 uses the number of the earlier tests; the others a pattern.
LF_PROTOCOLS = [("EM4100", 5), ("EM4100/32", 5), ("EM4100/16", 5),
	("H10301", 3), ("Idteck", 8), ("Indala26", 4), ("IoProxXSF", 4),
	("AWID", 9), ("FDX-A", 5), ("FDX-B", 11), ("HIDProx", 6),
	("HIDExt", 12), ("Pyramid", 4), ("Viking", 4), ("Jablotron", 5),
	("Paradox", 6), ("PAC/Stanley", 4), ("Keri", 4), ("Gallagher", 8),
	("Nexwatch", 8), ("Electra", 8), ("Radio Key", 6), ("GProxII", 12),
	("Noralsy", 12), ("Indala224", 28)]
PATTERN = "1A2B3C4D5E6F708192A3B4C5D6E7F8091A2B3C4D5E6F708192A3B4C5D6E7F8"


# Protocols that do not carry every bit of the pattern: the pattern's
# canonical form, the bytes the Unleashed code itself gives back after
# encoding and decoding it (found on the device 2026-11-19 and checked on
# the host with the same code: Paradox drops the last nibble, Gallagher
# bytes 2 and 5, Radio Key moves a parity bit, FDX-A needs its parity,
# Keri sets bit 31, Idteck has the fixed "IDTK" prefix; HIDProx, AWID,
# HIDExt and Indala26 do not send their last bits, which a reader leaves 0).
# Computed by encoding with one protocol dictionary and decoding with a
# fresh one: decoding with the encoder's own dictionary keeps the bits a
# frame does not carry and hides that.  Nexwatch, GProxII,
# Noralsy and Indala224 do not take the pattern at all (their checks
# fail): they use the fixed vectors of flipper/tests/lfrfid/lfrfidtest.c,
# which make lfrfidtest round-trips.
LF_CANONICAL = {"Paradox": "1A2B3C4D5E60", "Gallagher": "1A003C4D006F7081",
	"Radio Key": "1A693C4D5E6F", "FDX-A": "1AABBCCD5E",
	"Keri": "9A2B3C4D", "Idteck": "4944544B5E6F7081",
	"HIDProx": "1A2B3C4D5E60", "AWID": "1A2B3C4D5E6F708180",
	"HIDExt": "1A2B3C4D5E6F708192A3B4C0", "Indala26": "1A2B3C48",
	"Nexwatch": "00345678DC0365AA",
	"GProxII": "FA0093200C421A161D080A00",
	"Noralsy": "BB0456789000AC1B1EA80000",
	"Indala224": "80000001B23EAEB0FFE8063AB156010F19D3457F5A3C96E12D4B78A5"}


def lf_data(proto, n):
	return LF_CANONICAL.get(proto, PATTERN[:2 * n])


# IR: one message per protocol; values from Unleashed's own infrared unit
# tests where it has them (Kaseikyo needs its vendor id in the address).
IR_VECTORS = [("NEC", 0x04, 0x08), ("NECext", 0x7984, 0xED12),
	("NEC42", 0xA5, 0x3C), ("NEC42ext", 0x12345, 0xABCD),
	("Samsung32", 0x0E, 0x0C), ("RC5", 0x13, 0x10), ("RC5X", 0x13, 0x10),
	("RC6", 0x94, 0xA0), ("SIRC", 0x10, 0x15), ("SIRC15", 0x45, 0x15),
	("SIRC20", 0x1A45, 0x15), ("Kaseikyo", 0x325441, 0x1B),
	("RCA", 0x0F, 0x54), ("Pioneer", 0xAF, 0x36)]
IR_RAW = [3000, 1000, 500, 500, 1000, 500, 500, 1000, 500, 1500, 500]

# iButton (Unleashed lib/ibutton): Metakom bytes need even parity.
IB_VECTORS = [("Dallas", "01A1B2C3D4E5F68F"), ("Cyfral", "A1B2"),
	("Metakom", "0305090A")]

SUBGHZ_DYNAMIC = {"KeeLoq", "Security+ 1.0", "Security+ 2.0", "Phoenix_V2"}
NFC_LEAF = [(r"NTAG|Ultralight", "NTAG/Ultralight"), (r"FeliCa", "FeliCa"),
	(r"SLIX", "SLIX"), (r"ISO14443-3A", "ISO14443-3A")]


def sub_files():
	res = []
	for f in sorted(glob.glob(os.path.join(DATA, "subghz", "*.sub"))):
		t = open(f).read()
		g = lambda k: (re.search(r"^%s: *(.*)$" % k, t, re.M)
			or [None, ""])[1].strip()
		res.append({"file": os.path.basename(f), "text": t,
			"proto": g("Protocol"), "freq": int(g("Frequency") or 0),
			"key": g("Key").replace(" ", "")})
	return res


def nfc_files():
	res = []
	for f in sorted(glob.glob(os.path.join(DATA, "nfc", "*.nfc"))):
		t = open(f).read()
		dt = re.search(r"^Device type: *(.*)$", t, re.M).group(1)
		leaf = next(l for p, l in NFC_LEAF if re.search(p, dt))
		res.append({"file": os.path.basename(f), "text": t, "type": dt,
			"leaf": leaf})
	return res


# --- RFID ----------------------------------------------------------------

def judge_lf_minix(proto, data, rc, out):
	"""MINIX's rfid read output against what was emulated."""
	if missing(rc, out):
		return "MISSING", "rfid: " + last(out)
	m = re.search(re.escape(proto) + r"(?![/\w])[ \t:]+((?:[0-9A-Fa-f]{2}"
		r"[ :]?)+)", out)
	if m:
		got = compact(m.group(1))
		if got == data:
			return "PASS", "MINIX read %s %s" % (proto, got)
		return "FAIL", "MINIX read %s but %s, sent %s" % (proto, got, data)
	for p, n in LF_PROTOCOLS:
		if p != proto and re.search(re.escape(p) + r"(?![/\w])[ \t:]+"
				r"[0-9A-Fa-f]{2}", out):
			return "FAIL", "MINIX read it as %s: %s" % (p, last(out))
	return "MISSING", "not decoded: " + last(out)


def judge_lf_unleashed(proto, data, out):
	m = re.search(r"^" + re.escape(proto) + r" ([0-9A-F]+)\s*$", out, re.M)
	if m:
		if m.group(1) == data:
			return "PASS", "Unleashed read %s %s" % (proto, m.group(1))
		return "FAIL", "Unleashed read %s %s, sent %s" % (proto,
			m.group(1), data)
	m = re.search(r"^([A-Za-z][\w/ -]*?) ([0-9A-F]{4,})\s*$", out, re.M)
	if m:
		return "FAIL", "Unleashed read it as %s %s" % m.groups()
	return "FAIL", "Unleashed read nothing"


def mx_lf_cmd(verb, proto, data):
	if proto == "EM4100":		# the syntax MINIX has had since R6.5
		return "rfid %s EM %s" % (verb, " ".join(data[i:i + 2]
			for i in range(0, len(data), 2)))
	return 'rfid %s "%s" %s' % (verb, proto, data)


# The RFID rows depend on how far apart the two antennas are (devguide
# 6.4, checked 2026-11-19 with the Flippers in their silicone cases): the
# ASK and FSK rows read with a thick spacer (about 5 mm of card), the PSK
# rows with none (their short loads are too weak further away; with a thin
# card 3 of 5, and that close most ASK/FSK rows are not read).  --spacer
# says which is in; "ask" runs the thick rows first, then asks for the
# change.
LF_PSK = {"Keri", "Indala26", "Idteck", "Nexwatch", "Indala224"}
SPACERS = ("none", "thin", "thick")


def lf_spacers(proto):
	"""The spacers a protocol's rows read with."""
	return ("none",) if proto in LF_PSK else ("thick",)


def spacer_gate(opt, proto):
	"""None if the row may run with the spacer in, else its BLOCKED
	status; with --spacer ask, ask for the change first."""
	sp = getattr(opt, "spacer", None)
	want = lf_spacers(proto)
	if sp is None:
		return None
	if sp == "ask":
		if getattr(opt, "spacer_now", None) not in want:
			opt.spacer_now = want[-1]
			ask(opt, "RFID: put the two Flippers back to back %s." % (
				"with no spacer (cases on, backs touching)"
				if want[-1] == "none" else
				"with the thick spacer between them (about 5 mm of card)"))
		return None
	if sp not in want:
		return "BLOCKED", "needs the %s spacer (--spacer %s or ask)" % (
			" or ".join(want), want[-1])
	return None


def rfid_u2m(ul, mx, opt, proto, data):
	g = spacer_gate(opt, proto)
	if g:
		return g
	if " " in proto:
		return "BLOCKED", "the Unleashed CLI cannot take a protocol name " \
			"with a space"
	if not ul.start("rfid emulate %s %s" % (proto, data)):
		return "BLOCKED", "Unleashed refused: " + last(ul.stop())
	rc, out = mx.run("rfid -n 8", 25)
	ul.stop()
	return judge_lf_minix(proto, data, rc, out)


def ul_read_lf(ul):
	if not ul.start("rfid read", rb"Reading RFID"):
		ul.stop()
		return ""
	if ul.wait(rb"Reading stopped", 15) is None:
		return ul.stop()
	return ul.text(ul.started)


def rfid_m2u(ul, mx, opt, proto, data):
	g = spacer_gate(opt, proto)
	if g:
		return g
	# the run list of a long PSK frame takes a few seconds to find
	# (Nexwatch 3.9 s on the device)
	ok = mx.start(mx_lf_cmd("emulate", proto, data),
		rb"emulating|[Uu]sage|not found|[Uu]nknown|rror", 8)
	txt = mx.text(mx.started)
	if missing(None, txt) or "needs T19b" in txt:
		mx.stop()
		return "MISSING", "rfid emulate: " + last(txt)
	if not ok:
		mx.stop()
		return "FAIL", "MINIX did not start emulating: " + last(txt)
	out = ul_read_lf(ul)
	mx.stop()
	return judge_lf_unleashed(proto, data, out)


def ask(opt, text):
	print("\n>>> " + text + " Then press Enter.", flush=True)
	input()


def rfid_write_u2m(ul, mx, opt, proto, data):
	if not opt.t5577:
		return "N/A", "needs a blank T5577 tag (--t5577)"
	if " " in proto:
		return "BLOCKED", "the Unleashed CLI cannot take a protocol name " \
			"with a space"
	ask(opt, "Hold the blank T5577 to the back of the UNLEASHED Flipper "
		"(stack apart).")
	ul.start("rfid write %s %s" % (proto, data), rb"Writing RFID")
	w = ul.wait(rb"Written!|cannot be written|is disabled", 15)
	ul.stop()
	if not w or w.group(0) != b"Written!":
		return "BLOCKED", "Unleashed did not write the tag"
	ask(opt, "Now hold the tag to the back of the MINIX Flipper.")
	rc, out = mx.run("rfid -n 8", 25)
	return judge_lf_minix(proto, data, rc, out)


def rfid_write_m2u(ul, mx, opt, proto, data):
	if not opt.t5577:
		return "N/A", "needs a blank T5577 tag (--t5577)"
	ask(opt, "Hold the blank T5577 to the back of the MINIX Flipper.")
	rc, out = mx.run(mx_lf_cmd("write", proto, data), 90)
	if missing(rc, out):
		return "MISSING", "rfid write: " + last(out)
	if rc != 0:
		return "FAIL", "MINIX: " + last(out)
	ask(opt, "Now hold the tag to the back of the UNLEASHED Flipper.")
	return judge_lf_unleashed(proto, data, ul_read_lf(ul))


# --- infrared ------------------------------------------------------------

MX_IR = rb"(\w+) address 0x([0-9a-fA-F]+) command 0x([0-9a-fA-F]+)(?=[^0-9a-fA-F])"
UL_IR = rb"(\w+), A:0x([0-9A-Fa-f]+), C:0x([0-9A-Fa-f]+)(?=[^0-9A-Fa-f])"


def ir_u2m(ul, mx, opt, p, a, c):
	got = None
	for _ in range(3):
		mx.start("ir rx -n 1")
		ok, uo = ul.run("ir tx %s %X %X" % (p, a, c), 5)
		if "Wrong arguments" in uo:
			mx.stop()
			return "BLOCKED", "Unleashed refused the message"
		got = mx.wait(MX_IR, 2)
		out = mx.stop()
		if got:
			break
		if re.search(r"^\d+:( \d+)+", out, re.M):
			return "MISSING", "MINIX received it but has no %s decoder" % p
	if not got:
		return "FAIL", "MINIX received nothing"
	g = (got.group(1).decode(), int(got.group(2), 16), int(got.group(3), 16))
	if g != (p, a, c):
		return "FAIL", "MINIX decoded %s 0x%X 0x%X" % g
	return "PASS", "%s 0x%X 0x%X" % (p, a, c)


def ir_m2u(ul, mx, opt, p, a, c):
	got = None
	for _ in range(3):
		if not ul.start("ir rx"):
			return "BLOCKED", "Unleashed 'ir rx' did not start: " + \
				last(ul.stop())
		rc, out = mx.run("ir tx %s 0x%x 0x%x" % (p, a, c), 5)
		if missing(rc, out):
			ul.stop()
			return "MISSING", "ir tx: " + last(out)
		got = ul.wait(UL_IR, 2)
		ul.stop()
		if got:
			break
	if not got:
		return "FAIL", "Unleashed decoded nothing"
	g = (got.group(1).decode(), int(got.group(2), 16), int(got.group(3), 16))
	if g != (p, a, c):
		return "FAIL", "Unleashed decoded %s 0x%X 0x%X" % g
	return "PASS", "%s 0x%X 0x%X" % (p, a, c)


def close_to(want, got):
	if len(got) < len(want):
		return False
	return all(abs(w - g) <= max(150, w // 4) for w, g in zip(want, got))


def ir_raw_u2m(ul, mx, opt):
	mx.start("ir rx -r -n 1")
	ul.run("ir tx RAW F:38000 DC:33 " + " ".join(map(str, IR_RAW)), 5)
	m = mx.wait(rb"(?m)^\d+:((?: \d+)+)(?=\r?\n)", 3)
	out = mx.stop()
	if missing(None, out):
		return "MISSING", "ir rx -r: " + last(out)
	if not m:
		return "FAIL", "MINIX received nothing"
	got = [int(x) for x in m.group(1).split()]
	if close_to(IR_RAW, got):
		return "PASS", "%d durations within 25 %%" % len(IR_RAW)
	return "FAIL", "MINIX got %s" % got[:len(IR_RAW)]


def ir_raw_m2u(ul, mx, opt):
	if not ul.start("ir rx raw"):
		return "BLOCKED", "Unleashed 'ir rx raw' did not start"
	rc, out = mx.run("ir tx raw " + " ".join(map(str, IR_RAW)), 5)
	if missing(rc, out):
		ul.stop()
		return "MISSING", "ir tx raw: " + last(out)
	m = ul.wait(rb"RAW, \d+ samples:\s*([\d ]+)(?=\r?\n)", 3)
	ul.stop()
	if not m:
		return "FAIL", "Unleashed received nothing"
	got = [int(x) for x in m.group(1).split()]
	if close_to(IR_RAW, got):
		return "PASS", "%d durations within 25 %%" % len(IR_RAW)
	return "FAIL", "Unleashed got %s" % got[:len(IR_RAW)]


# --- iButton -------------------------------------------------------------

UL_KEY = rb"(Dallas|Cyfral|Metakom) ([0-9A-F]+)(?=[^0-9A-F])"


def od_bytes(text):
	"""The bytes in "od -b" output (7-digit octal offset, 3-digit bytes)."""
	out = []
	for line in text.split("\n"):
		t = line.split()
		if t and re.fullmatch(r"[0-7]{7}", t[0]):
			out += [int(x, 8) for x in t[1:] if re.fullmatch(r"[0-7]{3}", x)]
	return out


def mx_read_dallas_dev(mx):
	"""Up to 10 one-shot reads of /dev/ibutton: (hex or None, last)."""
	lastout = ""
	for _ in range(10):
		rc, out = mx.run("od -b /dev/ibutton", 5)
		b = od_bytes(out)
		if len(b) == 8:
			return "".join("%02X" % x for x in b), out
		lastout = out
		time.sleep(0.3)
	return None, lastout


def ib_dev_u2m(ul, mx, opt, p, key):
	if not ul.start("ikey emulate %s %s" % (p, key)):
		return "BLOCKED", "Unleashed refused: " + last(ul.stop())
	got, out = mx_read_dallas_dev(mx)
	ul.stop()
	if got is None:
		return "FAIL", "no ROM in 10 reads of /dev/ibutton (%s); pin 17 " \
			"and GND wired?" % last(out)
	if got != key:
		return "FAIL", "MINIX read %s" % got
	return "PASS", "MINIX read ROM " + got


def ikey_knows(mx, p):
	"""Whether MINIX's ikey names the key type p in its usage (Dallas
	only until spec R6.7 part 3b brings Cyfral and Metakom), cached."""
	k = "ikey:" + p
	if k not in mx.have:
		rc, out = mx.run("ikey 2>&1", 10)
		mx.have[k] = p in out
	return mx.have[k]


def ib_u2m(ul, mx, opt, p, key):
	if not mx.exists("ikey"):
		return "MISSING", "no ikey command"
	if not ikey_knows(mx, p):
		return "MISSING", "ikey has no %s keys (spec R6.7 part 3b)" % p
	if not ul.start("ikey emulate %s %s" % (p, key)):
		return "BLOCKED", "Unleashed refused: " + last(ul.stop())
	mx.start("ikey read")
	m = mx.wait(UL_KEY + rb"|[Uu]sage", 10)
	out = mx.stop()
	ul.stop()
	if missing(None, out):
		return "MISSING", "ikey read: " + last(out)
	if not m or not m.group(1):
		return "FAIL", "MINIX read nothing"
	if (m.group(1).decode(), m.group(2).decode()) != (p, key):
		return "FAIL", "MINIX read %s %s" % (m.group(1).decode(),
			m.group(2).decode())
	return "PASS", "MINIX read %s %s" % (p, key)


def ul_read_key(ul):
	if not ul.start("ikey read", rb"Reading iButton"):
		ul.stop()
		return None
	m = ul.wait(UL_KEY, 10)
	ul.stop()
	return m


def ib_m2u(ul, mx, opt, p, key):
	if not mx.exists("ikey"):
		return "MISSING", "no ikey command"
	if not ikey_knows(mx, p):
		return "MISSING", "ikey has no %s keys (spec R6.7 part 3b)" % p
	mx.start("ikey emulate %s %s" % (p, key),
		rb"Emulating |so far|[Uu]sage|not found|[Uu]nknown", 3)
	txt = mx.text(mx.started)
	if missing(None, txt) or "so far" in txt:
		mx.stop()
		return "MISSING", "ikey emulate: " + last(txt)
	m = ul_read_key(ul)
	mx.stop()
	if not m:
		return "FAIL", "Unleashed read nothing"
	g = (m.group(1).decode(), m.group(2).decode())
	if g != (p, key):
		return "FAIL", "Unleashed read %s %s" % g
	return "PASS", "Unleashed read %s %s" % g


def ib_write_u2m(ul, mx, opt, p, key):
	if not opt.rw1990:
		return "N/A", "needs a blank RW1990 key (--rw1990)"
	ask(opt, "Hold the blank RW1990 to the iButton contacts of either "
		"Flipper (the 1-Wire line is shared) and keep it there.")
	ul.start("ikey write Dallas " + key, rb"Writing key")
	w = ul.wait(rb"Write success|cannot|disabled", 15)
	ul.stop()
	if not w or w.group(0) != b"Write success":
		return "BLOCKED", "Unleashed did not write the key"
	got, out = mx_read_dallas_dev(mx)
	if got != key:
		return "FAIL", "MINIX read %s" % (got or last(out))
	return "PASS", "written by Unleashed, MINIX read " + got


def ib_write_m2u(ul, mx, opt, p, key):
	if not opt.rw1990:
		return "N/A", "needs a blank RW1990 key (--rw1990)"
	if not mx.exists("ikey"):
		return "MISSING", "no ikey command"
	ask(opt, "Hold the blank RW1990 to the iButton contacts and keep it "
		"there.")
	rc, out = mx.run("ikey write Dallas " + key, 30)
	if missing(rc, out):
		return "MISSING", "ikey write: " + last(out)
	m = ul_read_key(ul)
	if not m or m.group(2).decode() != key:
		return "FAIL", "Unleashed read %s" % (m.group(0).decode() if m
			else "nothing")
	return "PASS", "written by MINIX, Unleashed read " + key


# --- Sub-GHz -------------------------------------------------------------

MX_SG = r"rssi=(-?\d+)(?: peak=(-?\d+))?"
UL_TX_REFUSED = rb"restricted|only be used for RX|must be in|[Ee]rror|failed"


def ul_refused(out):
	"""Why Unleashed did not transmit, with the usual cause named."""
	why = last(out)
	if "failed to start" in why:
		why += " (Unleashed unlshd-095 CLI bug: the internal CC1101 has " \
			"no begin(), so every subghz tx/rx on device 0 fails; use " \
			"another Unleashed release)"
	return why


def sg_level(mx):
	rc, out = mx.run("cat /dev/subghz", 5)
	m = re.search(MX_SG, out)
	if not m:
		return None
	return max(int(m.group(1)), int(m.group(2) or m.group(1)))


def subghz_rssi_u2m(ul, mx, opt, freq):
	rc, out = mx.run("echo %d > /dev/subghz" % freq, 5)
	if missing(rc, out):
		return "MISSING", "/dev/subghz: " + last(out)
	if rc != 0:
		return "FAIL", "MINIX refused %d Hz: %s" % (freq, last(out))
	quiet = [x for x in (sg_level(mx) for _ in range(3)) if x is not None]
	if not quiet:
		mx.run("echo 0 > /dev/subghz")
		return "FAIL", "no rssi line from /dev/subghz"
	base = max(quiet)
	ul.start("subghz tx 74BADE %d 403 100 0" % freq,
		rb"Transmitting at|" + UL_TX_REFUSED)
	if not re.search(rb"Transmitting at", ul.buf[ul.started:]):
		out = ul.stop()
		mx.run("echo 0 > /dev/subghz")
		return "BLOCKED", "Unleashed does not transmit at %d Hz here: %s" \
			% (freq, ul_refused(out))
	loud = [x for x in (sg_level(mx) for _ in range(6)) if x is not None]
	if ul.wait(UL_PROMPT, 15) is None:
		ul.stop()
	mx.run("echo 0 > /dev/subghz")
	best = max(loud) if loud else -200
	detail = "quiet %d dBm, during Unleashed tx %d dBm" % (base, best)
	if best >= -60 and best - base >= 15:
		return "PASS", detail
	return "FAIL", detail


def subghz_raw_u2m(ul, mx, opt):
	rc, out = mx.run("echo %d > /dev/subghz" % opt.freq, 5)
	if missing(rc, out):
		return "MISSING", "/dev/subghz: " + last(out)
	if not ul.start("subghz tx 74BADE %d 403 100 0" % opt.freq,
			rb"Transmitting at"):
		out = ul.stop()
		mx.run("echo 0 > /dev/subghz")
		return "BLOCKED", "Unleashed did not transmit: " + last(out)
	rc, out = mx.run("ir rx -r -n 1 -d /dev/subghzraw", 10)
	if ul.wait(UL_PROMPT, 15) is None:
		ul.stop()
	mx.run("echo 0 > /dev/subghz")
	if missing(rc, out):
		return "MISSING", "/dev/subghzraw: " + last(out)
	m = re.search(r"^(\d+):((?: \d+)+)", out, re.M)
	if not m:
		return "INFO", "no raw durations from /dev/subghzraw"
	d = [int(x) for x in m.group(2).split()]
	near = sum(1 for x in d if 280 <= x <= 530 or 1000 <= x <= 1420)
	return "INFO", "%d durations, %d near TE/3TE (403/1209 us)" % (
		len(d), near)


def judge_sub(f, out, who, nothing):
	"""Decoded text (either side's rx) against the .sub file."""
	if f["proto"] not in out:
		return nothing
	if f["proto"] not in SUBGHZ_DYNAMIC:
		k = f["key"].lstrip("0") or "0"
		if k[-6:] not in compact(out):
			return "FAIL", "%s decoded %s but not key ...%s: %s" % (who,
				f["proto"], k[-6:], last(out, 2))
		return "PASS", "%s decoded %s key ...%s" % (who, f["proto"], k[-6:])
	return "PASS", "%s decoded %s (rolling code: key not compared)" % (
		who, f["proto"])


# A remote sends its frame several times; most decoders report a frame only
# once they have seen it twice in a row (or after a first frame's guard
# time), so one transmission is not enough.
SUB_REPEAT = 10
# Unleashed unlshd-093 says "Error transmitter" and then reboots (its USB
# port goes away) on tx_from_file with these files.
UL_TX_CRASH = {"Proteco": "Unleashed unlshd-093 reboots on tx_from_file of this file (its own bug)", "Freezer": "Unleashed unlshd-093 reboots on tx_from_file "
	"of this file (its own bug)"}


def subghz_decode_u2m(ul, mx, opt, f):
	if not mx.exists("subghz"):
		return "MISSING", "no subghz command (MINIX: subghz rx FREQ)"
	path = "%s/%s" % (UL_DIR, f["file"])
	if not ul.upload(path, f["text"].encode()):
		return "BLOCKED", "could not write %s on the Unleashed SD card" % path
	mx.start("subghz rx %d" % f["freq"],
		rb"[Rr]eceiv|[Ll]isten|[Uu]sage|not found", 3)
	txt = mx.text(mx.started)
	if missing(None, txt):
		mx.stop()
		return "MISSING", "subghz rx: " + last(txt)
	if f["proto"] in UL_TX_CRASH:
		mx.stop()
		return "BLOCKED", UL_TX_CRASH[f["proto"]]
	ok, uo = ul.run("subghz tx_from_file %s %d 0" % (path, SUB_REPEAT), 30)
	if re.search(UL_TX_REFUSED.decode(), uo):
		mx.stop()
		return "BLOCKED", "Unleashed: " + ul_refused(uo)
	mx.wait(re.escape(f["proto"]).encode(), 3)
	time.sleep(0.5)
	out = mx.stop()
	return judge_sub(f, out, "MINIX", ("MISSING", "MINIX decoded nothing "
		"(no %s decoder?): %s" % (f["proto"], last(out))))


def subghz_decode_m2u(ul, mx, opt, f):
	if not mx.exists("subghz"):
		return "MISSING", "no subghz command (MINIX: subghz tx_from_file)"
	path = MX_TMP + "/twoflip.sub"
	if not mx.upload(path, f["text"]):
		return "FAIL", "could not write %s on MINIX" % path
	if not ul.start("subghz rx %d 0" % f["freq"]):
		return "BLOCKED", "Unleashed rx did not start: " + last(ul.stop())
	rc, out = mx.run("subghz tx_from_file %s 1" % path, 30)
	if missing(rc, out):
		ul.stop()
		return "MISSING", "subghz tx_from_file: " + last(out)
	ul.wait(re.escape(f["proto"]).encode(), 3)
	time.sleep(0.5)
	uo = ul.stop()
	return judge_sub(f, uo, "Unleashed", ("FAIL", "Unleashed decoded "
		"nothing (MINIX: %s)" % last(out)))


def subghz_tx_m2u(ul, mx, opt):
	if not mx.exists("subghz"):
		return "MISSING", "no subghz command (MINIX: subghz tx KEY FREQ TE " \
			"REPEAT)"
	if not ul.start("subghz rx %d 0" % opt.freq):
		return "BLOCKED", "Unleashed rx did not start: " + last(ul.stop())
	rc, out = mx.run("subghz tx 74BADE %d 403 10" % opt.freq, 30)
	if missing(rc, out):
		ul.stop()
		return "MISSING", "subghz tx: " + last(out)
	ul.wait(rb"Princeton", 3)
	time.sleep(0.5)
	uo = ul.stop()
	f = {"proto": "Princeton", "key": "74BADE"}
	return judge_sub(f, uo, "Unleashed", ("FAIL", "Unleashed decoded "
		"nothing"))


CHAT = "twoflip-hello-%d"


def subghz_chat(ul, mx, opt, dirn):
	if not mx.exists("subghz"):
		return "MISSING", "no subghz command (MINIX: subghz chat FREQ)"
	mx.start("subghz chat %d" % opt.freq,
		rb"[Rr]eceiv|[Uu]sage|not found|[Uu]nknown", 3)
	txt = mx.text(mx.started)
	if missing(None, txt):
		mx.stop()
		return "MISSING", "subghz chat: " + last(txt)
	if not ul.start("subghz chat %d 0" % opt.freq):
		mx.stop()
		return "BLOCKED", "Unleashed chat did not start: " + last(ul.stop())
	word = CHAT % (int(time.time()) % 10000)
	time.sleep(0.5)
	if dirn == "u2m":
		ul.write(word + "\r")
		got = mx.wait(word.encode(), 5)
	else:
		mx.write(word + "\r")
		got = ul.wait(word.encode(), 5)
	ul.stop()
	mx.stop()
	if got:
		return "PASS", "the line arrived"
	return "FAIL", "the line did not arrive"


# --- NFC -----------------------------------------------------------------

def nfc_u2m(ul, mx, opt, f):
	if not mx.exists("nfc"):
		return "MISSING", "no nfc command (MINIX: nfc scanner)"
	path = "%s/%s" % (UL_DIR, f["file"])
	if not ul.upload(path, f["text"].encode()):
		return "BLOCKED", "could not write %s on the Unleashed SD card" % path
	ul.run("nfc", 5)
	if not ul.start("emulate -f " + path,
			rb"Emulating|Failed|Wrong|rror"):
		out = ul.stop()
		ul.run("exit", 5)
		return "BLOCKED", "Unleashed did not emulate: " + last(out)
	rc, out = mx.run("nfc scanner", 20)
	ul.stop()
	ul.run("exit", 5)
	if missing(rc, out):
		return "MISSING", "nfc scanner: " + last(out)
	if f["leaf"] in out:
		return "PASS", "MINIX detected " + f["leaf"]
	return "FAIL", "MINIX did not detect %s: %s" % (f["leaf"], last(out))


def nfc_m2u(ul, mx, opt, f):
	if not mx.exists("nfc"):
		return "MISSING", "no nfc command (MINIX: nfc emulate -f FILE)"
	path = MX_TMP + "/twoflip.nfc"
	if not mx.upload(path, f["text"]):
		return "FAIL", "could not write %s on MINIX" % path
	mx.start("nfc emulate -f " + path,
		rb"[Ee]mulat|[Uu]sage|not found|[Uu]nknown|rror", 3)
	txt = mx.text(mx.started)
	if missing(None, txt):
		mx.stop()
		return "MISSING", "nfc emulate: " + last(txt)
	ul.run("nfc", 5)
	ul.start("scanner")
	if ul.wait(UL_PROMPT, 15) is None:
		ul.stop()
	out = ul.text(ul.started)
	ul.run("exit", 5)
	mx.stop()
	if f["leaf"] in out:
		return "PASS", "Unleashed detected " + f["leaf"]
	return "FAIL", "Unleashed did not detect %s: %s" % (f["leaf"],
		last(out, 2))


# --- data modems between two MINIX Flippers (spec section P) -----------

# A test text of 1 KB, the same every run, a little Basque in it.
MODEM_TEXT = "".join("%04d Kaixo, galtzagorriak! Datuak irratiz doaz, "
	"bit bat, bi bit, hiru bit.\n" % i for i in range(16))
MODEM_IN = MX_TMP + "/mdm.in"
MODEM_OUT = MX_TMP + "/mdm.out"
MODEM_MEDIA = [("rfid", ["fast", "robust"]),
	("subghz", ["narrow", "std", "wide", "max"]),
	("nfc", [""]), ("ble", [""])]


def modem_row(ul, mx, opt, medium, preset, dirn):
	"""MINIX sends MODEM_TEXT with `gmodem send`, the other MINIX Flipper
	receives it with `gmodem recv`; PASS when the checksums agree."""
	if not opt.peer_minix:
		return "N/A", "needs a second Flipper with MINIX (--peer-minix): " \
			"the Unleashed firmware has no such modem"
	tx, rx = (mx, ul) if dirn == "m2u" else (ul, mx)
	for side, name in ((tx, "sender"), (rx, "receiver")):
		if not side.exists("gmodem"):
			return "MISSING", "no gmodem command on the " + name
	p = " -p " + preset if preset else ""
	if not tx.upload(MODEM_IN, MODEM_TEXT):
		return "FAIL", "could not write " + MODEM_IN
	rc, want = tx.run("cksum < " + MODEM_IN, 10)
	rx.run("rm -f " + MODEM_OUT, 5)
	rx.start("gmodem recv -m %s%s -o %s -t 60; echo @@M\"\"=$?"
		% (medium, p, MODEM_OUT))
	time.sleep(1)
	tx.start("gmodem send -m %s%s %s" % (medium, p, MODEM_IN))
	m = rx.wait(rb"@@M=(\d+)(?=\D)", 180)
	txt = tx.stop()
	got = rx.stop()
	if missing(None, txt) or missing(None, got):
		return "MISSING", "gmodem: " + last(txt if missing(None, txt)
			else got)
	if not m:
		return "FAIL", "the receiver did not finish in 180 s: " + last(got)
	if m.group(1) != b"0":
		return "FAIL", "gmodem recv exit %s: %s" % (m.group(1).decode(),
			last(got))
	rc, have = rx.run("cksum < " + MODEM_OUT, 10)
	if last(have) != last(want):
		return "FAIL", "checksums differ: sent %s, got %s" % (last(want),
			last(have))
	s = re.search(r"(\d+) bytes, (\d+) frames, (\d+) s", got)
	return "PASS", ("%s B in %s s" % (s.group(1), s.group(3))) if s else ""


# --- the rows ------------------------------------------------------------

def rid(*parts):
	return "-".join(re.sub(r"[^A-Za-z0-9.+-]+", "_", p).strip("_")
		for p in parts)


# Sub-GHz: where the reference Flipper may transmit for a row.  The
# Unleashed Flipper applies no region (it showed provisioned "00" with a
# valid one), so the script must: by default only the EU short-range
# bands (433.05-434.79, 868.0-868.6 MHz).  315 MHz is not allowed in the
# EU; it ran once by mistake on 2026-11-18.  --region US or any to change.
TX_BANDS = {"EU": [(433050000, 434790000), (868000000, 868600000)],
	"US": [(304100000, 321950000), (433050000, 434790000),
		(902000000, 928000000)]}


def tx_allowed(opt, hz):
	if opt.region == "any":
		return True
	return any(lo <= hz <= hi for lo, hi in TX_BANDS[opt.region])


def tx_refused(ul, mx, opt, hz):
	return "BLOCKED", "%.2f MHz is not a transmit band of region %s " \
		"(--region)" % (hz / 1e6, opt.region)


def build_rows(opt):
	"""[(id, area, feature, dirn, fn, args)]: everything the Unleashed
	Flipper can do with a partner."""
	rows = []

	def add(area, feature, dirn, fn, *args):
		rows.append((rid(area, feature, dirn), area, feature, dirn, fn, args))

	for p, n in LF_PROTOCOLS:
		d = lf_data(p, n)
		add("rfid", p, "u2m", rfid_u2m, p, d)
		add("rfid", p, "m2u", rfid_m2u, p, d)
	for p, n in LF_PROTOCOLS:
		d = lf_data(p, n)
		add("rfid-write", p, "u2m", rfid_write_u2m, p, d)
		add("rfid-write", p, "m2u", rfid_write_m2u, p, d)
	for p, a, c in IR_VECTORS:
		add("ir", p, "u2m", ir_u2m, p, a, c)
		add("ir", p, "m2u", ir_m2u, p, a, c)
	add("ir", "RAW", "u2m", ir_raw_u2m)
	add("ir", "RAW", "m2u", ir_raw_m2u)
	add("ibutton", "Dallas /dev/ibutton", "u2m", ib_dev_u2m,
		"Dallas", IB_VECTORS[0][1])
	for p, k in IB_VECTORS:
		add("ibutton", p, "u2m", ib_u2m, p, k)
		add("ibutton", p, "m2u", ib_m2u, p, k)
	add("ibutton", "write Dallas", "u2m", ib_write_u2m, "Dallas",
		IB_VECTORS[0][1])
	add("ibutton", "write Dallas", "m2u", ib_write_m2u, "Dallas",
		IB_VECTORS[0][1])
	for fr in (315000000, 433920000, 868350000):
		add("subghz", "level %.2f MHz" % (fr / 1e6), "u2m",
			subghz_rssi_u2m if tx_allowed(opt, fr) else tx_refused, fr)
	add("subghz", "raw timings", "u2m", subghz_raw_u2m)
	add("subghz", "tx Princeton", "m2u", subghz_tx_m2u)
	add("subghz", "chat", "u2m", subghz_chat, "u2m")
	add("subghz", "chat", "m2u", subghz_chat, "m2u")
	for f in sub_files():
		if not tx_allowed(opt, f["freq"]):
			for d in ("u2m", "m2u"):
				add("subghz-decode", f["proto"], d, tx_refused, f["freq"])
			continue
		add("subghz-decode", f["proto"], "u2m", subghz_decode_u2m, f)
		add("subghz-decode", f["proto"], "m2u", subghz_decode_m2u, f)
	for f in nfc_files():
		add("nfc", f["file"][:-4], "u2m", nfc_u2m, f)
		add("nfc", f["file"][:-4], "m2u", nfc_m2u, f)
	for medium, presets in MODEM_MEDIA:
		for pr in presets:
			feat = medium + (" " + pr if pr else "")
			add("modem", feat, "m2u", modem_row, medium, pr, "m2u")
			add("modem", feat, "u2m", modem_row, medium, pr, "u2m")
	if getattr(opt, "peer_minix", False):	# the rest needs Unleashed
		rows = [r for r in rows if r[1] == "modem"]
	return rows


def spacer_order(rows):
	"""The rfid rows that need no spacer after the others, so that
	--spacer ask asks only once."""
	rf = [i for i, r in enumerate(rows) if r[1] == "rfid"]
	if not rf:
		return rows
	thin = [rows[i] for i in rf if "thick" not in lf_spacers(rows[i][5][0])]
	thick = [rows[i] for i in rf if "thick" in lf_spacers(rows[i][5][0])]
	first = rf[0]
	rest = [r for i, r in enumerate(rows) if i not in rf]
	return rest[:first] + thick + thin + rest[first:]


def select_rows(rows, pats):
	if not pats:
		return rows
	return [r for r in rows if any(fnmatch.fnmatch(r[0], p)
		or r[0].startswith(p) for p in pats)]


def run_rows(ul, mx, opt, rows):
	res = []
	for i, (id_, area, feat, dirn, fn, args) in enumerate(rows):
		sts, det = [], ""
		for k in range(opt.repeat):
			ul.sync()
			mx.sync()
			t0 = time.time()
			try:
				st, det = fn(ul, mx, opt, *args)
			except (OSError, ValueError) as e:
				st, det = "FAIL", "error: %s" % e
			sts.append(st)
			if opt.repeat > 1:
				print("        %-40s #%d %s %s (%.1f s)" % (id_, k + 1, st,
					det, time.time() - t0), flush=True)
		st = next(s for s in WORST if s in sts)
		if opt.repeat > 1 and len(set(sts)) > 1:
			st = "FAIL"
			det = "%d/%d passed" % (sts.count("PASS"), opt.repeat)
		print("%-7s %-40s %s" % (st, id_, det), flush=True)
		res.append({"id": id_, "area": area, "feature": feat, "dir": dirn,
			"status": st, "detail": det})
	return res


def summary(res):
	areas = []
	for r in res:
		if r["area"] not in areas:
			areas.append(r["area"])
	print("\n%-14s" % "" + "".join("%9s" % s for s in STATUSES))
	for a in areas:
		c = [sum(1 for r in res if r["area"] == a and r["status"] == s)
			for s in STATUSES]
		print("%-14s" % a + "".join("%9d" % x for x in c))
	c = [sum(1 for r in res if r["status"] == s) for s in STATUSES]
	print("%-14s" % "total" + "".join("%9d" % x for x in c))


def write_map(path, res, info):
	areas = []
	for r in res:
		if r["area"] not in areas:
			areas.append(r["area"])
	L = ["# Coverage map: Galtzagorriak MINIX vs Unleashed", "",
		"Made by `flipper/tools/twoflip.py` on %s." % info["date"],
		"MINIX: %s. Unleashed: %s." % (info["minix"], info["unleashed"]),
		"RFID spacer: %s (devguide 6.4)." % (info.get("spacer") or "not given"),
		"", "Statuses: PASS works; FAIL MINIX has it but it is wrong; "
		"MISSING not in MINIX yet; BLOCKED the Unleashed side could not "
		"do its part; N/A needs extra hardware; INFO measurement only.",
		"", "| Area | " + " | ".join(STATUSES) + " |",
		"|---|" + "---|" * len(STATUSES)]
	for a in areas:
		L.append("| %s | " % a + " | ".join(str(sum(1 for r in res
			if r["area"] == a and r["status"] == s)) for s in STATUSES)
			+ " |")
	for a in areas:
		L += ["", "## " + a, "",
			"| Feature | Unleashed → MINIX | MINIX → Unleashed |",
			"|---|---|---|"]
		feats = []
		for r in res:
			if r["area"] == a and r["feature"] not in feats:
				feats.append(r["feature"])
		for f in feats:
			cell = {}
			for r in res:
				if r["area"] == a and r["feature"] == f:
					cell[r["dir"]] = "**%s** %s" % (r["status"],
						r["detail"].replace("|", "/"))
			L.append("| %s | %s | %s |" % (f, cell.get("u2m", "—"),
				cell.get("m2u", "—")))
	with open(path, "w", encoding="utf-8") as fp:
		fp.write("\n".join(L) + "\n")
	with open(os.path.splitext(path)[0] + ".json", "w") as fp:
		json.dump({"info": info, "rows": res}, fp, indent=1)


def compare(base_path, res):
	old = {r["id"]: r["status"] for r in json.load(open(base_path))["rows"]}
	worse = better = 0
	print("\nChanges against %s:" % base_path)
	for r in res:
		o = old.get(r["id"])
		if o is None or o == r["status"]:
			continue
		tag = "now"
		if o == "PASS":
			worse += 1
			tag = "REGRESSED"
		elif r["status"] == "PASS":
			better += 1
			tag = "NEW PASS"
		print("  %-10s %-40s %s -> %s" % (tag, r["id"], o, r["status"]))
	print("  %d newly passing, %d regressed" % (better, worse))
	return worse


# --- ports ---------------------------------------------------------------

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


def versions(ul, mx):
	rc, out = mx.run("ver", 5)
	mv = last(out) if rc == 0 else "?"
	if isinstance(ul, Minix):		# --peer-minix
		rc, out = ul.run("ver", 5)
		return mv, "MINIX " + (last(out) if rc == 0 else "?")
	ok, uo = ul.run("info device", 5)
	m = (re.search(r"firmware\.version\s*:\s*(\S+)", uo)
		or re.search(r"firmware\.branch\.name\s*:\s*(\S+)", uo))
	return mv, (m.group(1) if m else "?")


def main():
	ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
	ap.add_argument("-m", "--minix", help="MINIX Flipper's serial port")
	ap.add_argument("-u", "--unleashed", help="Unleashed Flipper's port")
	ap.add_argument("-r", "--repeat", type=int, default=1)
	ap.add_argument("--freq", type=int, default=433920000)
	ap.add_argument("--region", choices=["EU", "US", "any"], default="EU",
		help="Sub-GHz transmit bands the rows may use (default EU)")
	ap.add_argument("--map")
	ap.add_argument("--baseline")
	ap.add_argument("--spacer", choices=SPACERS + ("ask",),
		help="RFID: the spacer between the Flippers (devguide 6.4); "
		"rows that need another are BLOCKED; ask: thick rows first, then "
		"it asks for the change")
	ap.add_argument("--t5577", action="store_true")
	ap.add_argument("--rw1990", action="store_true")
	ap.add_argument("--log")
	ap.add_argument("--list", action="store_true")
	ap.add_argument("--ports", action="store_true")
	ap.add_argument("--password")
	ap.add_argument("--selftest", action="store_true")
	ap.add_argument("--peer-minix", action="store_true")
	ap.add_argument("patterns", nargs="*")
	opt = ap.parse_args()
	if not tx_allowed(opt, opt.freq):
		ap.error("--freq %d is not a transmit band of region %s (use --region)"
			% (opt.freq, opt.region))

	rows = select_rows(build_rows(opt), opt.patterns)
	if opt.spacer == "ask":
		rows = spacer_order(rows)
	if not rows:
		sys.exit("twoflip: no row matches (see --list)")
	if opt.list:
		for r in rows:
			print("%-40s %s" % (r[0], r[2]))
		print("%d rows" % len(rows))
		return 0
	if opt.selftest:
		import importlib.util
		spec = importlib.util.spec_from_file_location("twoflip_sim",
			os.path.join(HERE, "twoflip_sim.py"))
		sim = importlib.util.module_from_spec(spec)
		spec.loader.exec_module(sim)
		return sim.selftest(sys.modules[__name__], opt, rows)
	if opt.ports:
		for path, desc in candidates():
			print("%-14s %-10s %s" % (path, probe(path), desc))
		return 0

	mport, uport = opt.minix, opt.unleashed
	if opt.peer_minix:
		return main_peer_minix(opt, rows, mport, uport)
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
	return finish(ul, mx, opt, rows)


def main_peer_minix(opt, rows, mport, uport):
	"""Both Flippers run MINIX: -m is "this" one, -u the peer."""
	if not mport or not uport:
		for path, desc in candidates():
			if path in (mport, uport):
				continue
			if probe(path) not in ("minix", "login", "user shell"):
				continue
			if not mport:
				mport = path
			elif not uport:
				uport = path
	if not mport or not uport:
		sys.exit("twoflip: --peer-minix needs two MINIX Flippers (found %s, "
			"%s); run with --ports, then give -m and -u" % (mport, uport))
	print("MINIX on %s, MINIX peer on %s" % (mport, uport), flush=True)
	log = open(opt.log, "w") if opt.log else None
	mx = Minix(mport, "M", log)
	ul = Minix(uport, "P", log)
	pw = opt.password or os.environ.get("TWOFLIP_PASSWORD")
	for side, port in ((mx, mport), (ul, uport)):
		if not login(side, pw) or not side.sync():
			sys.exit("twoflip: no root shell on the MINIX Flipper on %s"
				% port)
	return finish(ul, mx, opt, rows)


def finish(ul, mx, opt, rows):
	mv, uv = versions(ul, mx)
	print("MINIX %s, Unleashed %s; %d rows" % (mv, uv, len(rows)),
		flush=True)
	res = run_rows(ul, mx, opt, rows)
	summary(res)
	if opt.map:
		write_map(opt.map, res, {"date": time.strftime("%Y-%m-%d %H:%M"),
			"minix": mv, "unleashed": uv,
			"spacer": getattr(opt, "spacer", None)})
		print("map: %s (+ .json)" % opt.map)
	bad = sum(1 for r in res if r["status"] == "FAIL")
	if opt.baseline:
		bad += compare(opt.baseline, res)
	opt.results = res
	return bad


if __name__ == "__main__":
	sys.exit(main())
