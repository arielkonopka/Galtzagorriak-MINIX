#!/usr/bin/env python3
"""mtphost - the PC side of mtpd (spec R10.4, T29a): MTP containers over
mtpd's framed stream (flipper/mtpd/mtpfd.c), and its acceptance test.

Usage:
  mtphost.py test --spawn MTPD [--root DIR] [--name-max N]
        start MTPD (a PC build of mtpd, "flipper/mtpd/build/mtpd -r DIR")
        on a pipe and run the test below; --root lets the test also look
        at the files directly.
  mtphost.py test --unix PATH [--name-max N] [--wait S]
        the same against mtpd on QEMU's second serial line (a socket).
  mtphost.py ls|get|put ... (--spawn/--unix as above): by hand.

The test: GetDeviceInfo, sessions, storage; folders; send and get back
(SHA-256 equal) files of 0 B, 1 KB, 100 KB; GetPartialObject; the object
properties Windows asks for; rename (SetObjectPropValue) and move with the
handle staying valid; a name of 60 characters round trip (when the
storage takes it) and NAME_MAX + 1 refused, not cut; bad names ('/',
'..'); delete a file and a folder tree; cancel in the middle of a send
(the partial file is gone); unknown operations and broken containers get
a response code; events ObjectAdded/ObjectRemoved.  It prints
"MTPHOST PASS" at the end, or the first failure and exits 1.

The last step leaves /galtzagorriak/etxea.sub (the example file of the
manual) with known contents, then pulls the cable in the middle of a send
(closes the stream): QEMU's m2test checks both after mtpd has exited.
"""
import hashlib, os, socket, struct, subprocess, sys, time

# Container types, operations, responses, events, properties (mtpd/mtp.h)
CMD, DATA, RESP, EVENT = 1, 2, 3, 4
OP = dict(GetDeviceInfo=0x1001, OpenSession=0x1002, CloseSession=0x1003,
	GetStorageIDs=0x1004, GetStorageInfo=0x1005, GetNumObjects=0x1006,
	GetObjectHandles=0x1007, GetObjectInfo=0x1008, GetObject=0x1009,
	DeleteObject=0x100B, SendObjectInfo=0x100C, SendObject=0x100D,
	GetDevicePropDesc=0x1014, GetDevicePropValue=0x1015,
	MoveObject=0x1019, GetPartialObject=0x101B,
	GetObjectPropsSupported=0x9801, GetObjectPropDesc=0x9802,
	GetObjectPropValue=0x9803, SetObjectPropValue=0x9804,
	GetObjectPropList=0x9805)
RC = dict(OK=0x2001, General_Error=0x2002, Session_Not_Open=0x2003,
	Operation_Not_Supported=0x2005, Invalid_StorageID=0x2008,
	Invalid_ObjectHandle=0x2009, Access_Denied=0x200F,
	Invalid_ParentObject=0x201A, Invalid_Parameter=0x201D,
	Session_Already_Open=0x201E, Transaction_Cancelled=0x201F,
	Invalid_ObjectPropCode=0xA801, Invalid_ObjectProp_Value=0xA803,
	Invalid_Dataset=0xA806, Object_Too_Large=0xA809,
	Spec_By_Depth_Unsupported=0xA808, No_Valid_ObjectInfo=0x2015)
RCNAME = {v: k for k, v in RC.items()}
EV_ADDED, EV_REMOVED = 0x4002, 0x4003
STORAGE, ROOT = 0x00010001, 0xFFFFFFFF
FMT_FILE, FMT_DIR = 0x3000, 0x3001
PR_NAME, PR_SIZE, PR_PARENT, PR_FORMAT = 0xDC07, 0xDC04, 0xDC0B, 0xDC02
FRAME, F_CANCEL, F_EVENT = 1024, 0xFFFF, 0x8000

EXAMPLE = b"Filetype: Flipper SubGhz Key File\nVersion: 1\n" \
	b"Frequency: 433920000\nPreset: FuriHalSubGhzPresetOok650Async\n" \
	b"Protocol: Princeton\nBit: 24\nKey: 00 00 00 00 00 95 D5 D4\nTE: 400\n"
LONGNAME = "galtzagorriak_etxeko_atea_irekitzeko_urruneko_agintea.sub"


class Gone(Exception):
	"""mtpd closed the stream."""


class Link:
	"""The framed byte stream to mtpd: a pipe pair or a socket."""
	def __init__(self, rd, wr, closer):
		self.rd, self.wr, self.closer = rd, wr, closer
		self.bulk = b""
		self.events = []

	def send(self, data):
		for i in range(0, len(data), FRAME):
			c = data[i:i + FRAME]
			self.wr(struct.pack("<H", len(c)) + c)

	def cancel(self):
		self.wr(struct.pack("<H", F_CANCEL))

	def _exact(self, n):
		b = b""
		while len(b) < n:
			c = self.rd(n - len(b))
			if not c:
				raise Gone()
			b += c
		return b

	def _frame(self):
		h, = struct.unpack("<H", self._exact(2))
		if h & F_EVENT:
			self.events.append(self._exact(h & ~F_EVENT))
		else:
			if h == 0 or h > FRAME:
				raise AssertionError("bad frame header %#x" % h)
			self.bulk += self._exact(h)

	def container(self):
		while len(self.bulk) < 12:
			self._frame()
		n, = struct.unpack("<I", self.bulk[:4])
		assert n >= 12, "container length %d" % n
		while len(self.bulk) < n:
			self._frame()
		c, self.bulk = self.bulk[:n], self.bulk[n:]
		return c

	def close(self):
		self.closer()


def spawn(cmd):
	p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE)

	def wr(b):
		p.stdin.write(b)
		p.stdin.flush()

	def close():
		try:
			p.stdin.close()
		except OSError:
			pass
	link = Link(p.stdout.read1 if hasattr(p.stdout, "read1") else p.stdout.read,
		wr, close)
	link.proc = p
	return link


def unix(path, wait):
	end = time.time() + wait
	while True:
		s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
		try:
			s.connect(path)
			break
		except OSError:
			s.close()
			if time.time() > end:
				raise
			time.sleep(0.5)
	link = Link(s.recv, s.sendall, lambda: s.shutdown(socket.SHUT_WR))
	link.sock = s
	return link


def u16s(b, o):
	return struct.unpack_from("<H", b, o)[0], o + 2


def u32s(b, o):
	return struct.unpack_from("<I", b, o)[0], o + 4


def pstr(s):
	"""A PTP string."""
	if s == "":
		return b"\0"
	u = s.encode("utf-16-le") + b"\0\0"
	return bytes([len(u) // 2]) + u


def gstr(b, o):
	n = b[o]
	o += 1
	if n == 0:
		return "", o
	s = b[o:o + 2 * n].decode("utf-16-le")
	assert s.endswith("\0"), "string without NUL"
	return s[:-1], o + 2 * n


class Mtp:
	def __init__(self, link):
		self.l = link
		self.tid = 0

	def raw(self, code, params=(), data=None, header=None):
		"""One transaction: (response code, its parameters, data in)."""
		self.tid += 1
		c = struct.pack("<IHHI", 12 + 4 * len(params), CMD, code, self.tid)
		c += b"".join(struct.pack("<I", p) for p in params)
		self.l.send(header if header is not None else c)
		if data is not None:
			self.l.send(struct.pack("<IHHI", 12 + len(data), DATA, code,
				self.tid) + data)
		din = None
		while True:
			k = self.l.container()
			n, t, cd, tid = struct.unpack_from("<IHHI", k)
			if t == DATA:
				din = k[12:]
				continue
			assert t == RESP, "container type %d" % t
			ps = [struct.unpack_from("<I", k, 12 + 4 * i)[0]
				for i in range((n - 12) // 4)]
			return cd, ps, din

	def ok(self, code, params=(), data=None):
		rc, ps, din = self.raw(code, params, data)
		if rc != RC["OK"]:
			raise AssertionError("%#06x %s: %s" % (code, params,
				RCNAME.get(rc, hex(rc))))
		return ps, din

	def expect(self, want, code, params=(), data=None):
		rc, ps, din = self.raw(code, params, data)
		if rc != RC[want]:
			raise AssertionError("%#06x %s: %s, not %s" % (code, params,
				RCNAME.get(rc, hex(rc)), want))

	# --- the operations
	def open(self):
		self.ok(OP["OpenSession"], [1])

	def handles(self, parent=ROOT, fmt=0):
		_, d = self.ok(OP["GetObjectHandles"], [STORAGE, fmt, parent])
		n, = struct.unpack_from("<I", d)
		return list(struct.unpack_from("<%dI" % n, d, 4))

	def info(self, h):
		_, d = self.ok(OP["GetObjectInfo"], [h])
		st, o = u32s(d, 0)
		fmt, o = u16s(d, o)
		_, o = u16s(d, o)
		size, o = u32s(d, o)
		o += 2 + 4 * 6
		parent, o = u32s(d, o)
		o += 2 + 4 + 4
		name, o = gstr(d, o)
		cdate, o = gstr(d, o)
		mdate, o = gstr(d, o)
		return dict(storage=st, fmt=fmt, size=size, parent=parent,
			name=name, mdate=mdate, dir=fmt == FMT_DIR)

	def ls(self, parent=ROOT):
		return {self.info(h)["name"]: h for h in self.handles(parent)}

	def get(self, h):
		_, d = self.ok(OP["GetObject"], [h])
		return d

	def objinfo(self, name, size, folder=False):
		return struct.pack("<IHHIHIIIIIIIHII", STORAGE,
			FMT_DIR if folder else FMT_FILE, 0, size, 0, 0, 0, 0, 0, 0,
			0, 0, 1 if folder else 0, 0, 0) + pstr(name) + pstr("") \
			+ pstr("20261118T120000") + pstr("")

	def mkdir(self, name, parent=ROOT):
		ps, _ = self.ok(OP["SendObjectInfo"], [STORAGE, parent],
			self.objinfo(name, 0, True))
		return ps[2]

	def put(self, name, data, parent=ROOT):
		ps, _ = self.ok(OP["SendObjectInfo"], [STORAGE, parent],
			self.objinfo(name, len(data)))
		self.ok(OP["SendObject"], [], data)
		return ps[2]

	def rename(self, h, name):
		return self.raw(OP["SetObjectPropValue"], [h, PR_NAME], pstr(name))[0]

	def prop(self, h, p):
		_, d = self.ok(OP["GetObjectPropValue"], [h, p])
		return d

	def events(self):
		"""The events so far (one more transaction first: an event
		follows the response that caused it)."""
		self.ok(OP["GetDeviceInfo"])
		e = [struct.unpack_from("<IHHII", x) for x in self.l.events]
		self.l.events = []
		return [(c, p) for _, _, c, _, p in e]


def sha(b):
	return hashlib.sha256(b).hexdigest()


def check(cond, what):
	if not cond:
		raise AssertionError(what)
	print("  ok: " + what)


def test(m, root, name_max):
	print("mtphost: GetDeviceInfo, sessions, storage")
	_, d = m.ok(OP["GetDeviceInfo"])
	o = 8
	ext, o = gstr(d, o)
	check("microsoft.com" in ext, "MTP vendor extension: " + ext)
	o += 2
	n, o = u32s(d, o)
	ops = struct.unpack_from("<%dH" % n, d, o)
	check(all(v in ops for v in OP.values()), "every R10.4 operation is announced")
	m.expect("Session_Not_Open", OP["GetStorageIDs"])
	m.open()
	m.expect("Session_Already_Open", OP["OpenSession"], [1])
	_, d = m.ok(OP["GetStorageIDs"])
	check(struct.unpack("<II", d) == (1, STORAGE), "one storage")
	_, d = m.ok(OP["GetStorageInfo"], [STORAGE])
	cap, free = struct.unpack_from("<QQ", d, 6)
	label, _ = gstr(d, gstr(d, 26)[1])
	check(True, "storage info: capacity %#x, free %#x, label %r" % (cap, free, label))
	m.expect("Invalid_StorageID", OP["GetStorageInfo"], [7])
	m.expect("Operation_Not_Supported", 0x9999)
	m.expect("Operation_Not_Supported", 0x1010)	# ResetDevice
	m.expect("Invalid_ObjectHandle", OP["GetObjectInfo"], [0])
	m.expect("Invalid_ObjectHandle", OP["GetObjectInfo"], [0x7FFFFFF0])

	print("mtphost: folders, send and get back")
	if "galtzagorriak" in m.ls():
		m.ok(OP["DeleteObject"], [m.ls()["galtzagorriak"]])
	m.events()
	top = m.mkdir("galtzagorriak")
	check(m.info(top)["dir"] and m.info(top)["parent"] == 0, "folder in the root")
	check((EV_ADDED, top) in m.events(), "ObjectAdded for the folder")
	sub = m.mkdir("azpi", top)
	check(m.info(sub)["parent"] == top, "folder in a folder")
	blobs = {"zero.bin": b"", "kilo.bin": os.urandom(1024),
		"big.bin": os.urandom(100 * 1024 + 7)}
	hs = {}
	for name, data in blobs.items():
		hs[name] = m.put(name, data, top)
		check(sha(m.get(hs[name])) == sha(data),
			"%s: %d B round trip, SHA-256 equal" % (name, len(data)))
		if root:
			with open(os.path.join(root, "galtzagorriak", name), "rb") as f:
				check(f.read() == data, "%s: the same on the storage" % name)
	check(set(m.ls(top)) == set(blobs) | {"azpi"}, "the folder lists them")
	_, n = m.ok(OP["GetNumObjects"], [STORAGE, 0, top])[0], None
	check(m.ok(OP["GetNumObjects"], [STORAGE, FMT_DIR, top])[0] == [1],
		"GetNumObjects with a format")
	check(m.handles(top, FMT_DIR) == [sub], "GetObjectHandles with a format")
	big = blobs["big.bin"]
	ps, d = m.ok(OP["GetPartialObject"], [hs["big.bin"], 5000, 3000])
	check(d == big[5000:8000] and ps == [3000], "GetPartialObject")
	ps, d = m.ok(OP["GetPartialObject"], [hs["big.bin"], len(big) - 10, 3000])
	check(d == big[-10:] and ps == [10], "GetPartialObject at the end")
	m.expect("Invalid_ParentObject", OP["SendObjectInfo"], [STORAGE, hs["kilo.bin"]],
		m.objinfo("x", 1))
	m.expect("Invalid_Parameter", OP["SendObjectInfo"], [STORAGE, top],
		m.objinfo("kilo.bin", 1))		# exists: never replaced

	print("mtphost: properties")
	_, d = m.ok(OP["GetObjectPropsSupported"], [FMT_FILE])
	n, = struct.unpack_from("<I", d)
	sup = struct.unpack_from("<%dH" % n, d, 4)
	check(all(p in sup for p in (PR_NAME, PR_SIZE, PR_PARENT, PR_FORMAT,
		0xDC09, 0xDC41)), "props: name, size, format, parent, date, PUID")
	for p in sup:
		m.ok(OP["GetObjectPropDesc"], [p, FMT_FILE])
	check(gstr(m.prop(hs["kilo.bin"], PR_NAME), 0)[0] == "kilo.bin", "name")
	check(struct.unpack("<Q", m.prop(hs["big.bin"], PR_SIZE))[0] == len(big), "size")
	check(struct.unpack("<I", m.prop(hs["big.bin"], PR_PARENT))[0] == top, "parent")
	_, d = m.ok(OP["GetObjectPropList"], [hs["kilo.bin"], 0, 0xFFFFFFFF, 0, 0])
	check(struct.unpack_from("<I", d)[0] == len(sup), "GetObjectPropList: all")
	_, d = m.ok(OP["GetObjectPropList"], [hs["kilo.bin"], 0, PR_NAME, 0, 0])
	check(struct.unpack_from("<IIHH", d) == (1, hs["kilo.bin"], PR_NAME, 0xFFFF),
		"GetObjectPropList: one")
	m.expect("Spec_By_Depth_Unsupported", OP["GetObjectPropList"],
		[hs["kilo.bin"], 0, 0xFFFFFFFF, 0, 1])
	m.expect("Invalid_ObjectPropCode", OP["GetObjectPropValue"], [hs["kilo.bin"], 0xDC99])
	m.ok(OP["GetDevicePropValue"], [0xD402])

	print("mtphost: rename and move keep the handle")
	h = hs["kilo.bin"]
	check(m.rename(h, "kilo2.bin") == RC["OK"], "rename")
	check(m.info(h)["name"] == "kilo2.bin", "same handle, new name")
	check(m.rename(h, "zero.bin") == RC["Invalid_Parameter"], "rename onto a file: refused")
	for bad in ("a/b", "..", ".", "", "a\x01b"):
		check(m.rename(h, bad) == RC["Invalid_ObjectProp_Value"], "bad name %r refused" % bad)
	check(m.info(h)["name"] == "kilo2.bin", "the name is unchanged")
	m.ok(OP["MoveObject"], [h, STORAGE, sub])
	i = m.info(h)
	check(i["parent"] == sub and sha(m.get(h)) == sha(blobs["kilo.bin"]),
		"moved: same handle, same data")
	m.expect("Invalid_ParentObject", OP["MoveObject"], [top, STORAGE, sub])
	m.expect("Invalid_ParentObject", OP["MoveObject"], [top, STORAGE, top])
	m.ok(OP["MoveObject"], [h, STORAGE, ROOT])
	check(m.info(h)["parent"] == 0, "moved to the root")
	m.ok(OP["MoveObject"], [h, STORAGE, top])

	print("mtphost: long names (NAME_MAX %d)" % name_max)
	if len(LONGNAME.encode()) <= name_max:
		lh = m.put(LONGNAME, EXAMPLE, top)
		check(m.info(lh)["name"] == LONGNAME and m.get(lh) == EXAMPLE,
			"%d-character name round trip" % len(LONGNAME))
		m.ok(OP["DeleteObject"], [lh])
	exact = ("k" * name_max)
	eh = m.put(exact, b"x", top)
	check(m.info(eh)["name"] == exact, "a name of NAME_MAX (%d) bytes" % name_max)
	m.ok(OP["DeleteObject"], [eh])
	before = set(m.ls(top))
	rc, _, _ = m.raw(OP["SendObjectInfo"], [STORAGE, top],
		m.objinfo("k" * (name_max + 1), 1))
	check(rc == RC["Invalid_ObjectProp_Value"], "NAME_MAX + 1: an error")
	check(set(m.ls(top)) == before, "NAME_MAX + 1: no file, not a cut name")
	rc, _, _ = m.raw(OP["SendObjectInfo"], [STORAGE, top],
		m.objinfo("é" * (name_max // 2 + 1), 1))
	check(rc == RC["Invalid_ObjectProp_Value"], "too long in UTF-8 bytes: an error")
	u = "étxea-€-\U0001F511.sub"
	if len(u.encode()) <= name_max:
		uh = m.put(u, b"u", top)
		check(m.info(uh)["name"] == u, "UTF-8 name round trip (%r)" % u)
		m.ok(OP["DeleteObject"], [uh])

	print("mtphost: delete")
	m.events()
	m.ok(OP["DeleteObject"], [hs["zero.bin"]])
	check((EV_REMOVED, hs["zero.bin"]) in m.events(), "ObjectRemoved")
	m.expect("Invalid_ObjectHandle", OP["GetObjectInfo"], [hs["zero.bin"]])
	deep = m.mkdir("deep", sub)
	m.put("a.txt", b"a", deep)
	m.put("b.txt", b"b", sub)
	m.ok(OP["DeleteObject"], [sub])
	check("azpi" not in m.ls(top), "a folder tree deleted")
	m.expect("Invalid_ObjectHandle", OP["GetObjectInfo"], [deep])

	print("mtphost: cancel in the middle of a send")
	ps, _ = m.ok(OP["SendObjectInfo"], [STORAGE, top], m.objinfo("half.bin", 50000))
	m.tid += 1
	m.l.send(struct.pack("<IHHI", 12, CMD, OP["SendObject"], m.tid))
	m.l.send(struct.pack("<IHHI", 12 + 50000, DATA, OP["SendObject"], m.tid)
		+ os.urandom(3000))
	m.l.cancel()
	k = m.l.container()
	check(struct.unpack_from("<H", k, 6)[0] == RC["Transaction_Cancelled"],
		"Transaction_Cancelled")
	check("half.bin" not in m.ls(top), "the partial file is gone")
	if root:
		check(not os.path.exists(os.path.join(root, "galtzagorriak", "half.bin")),
			"and not on the storage")
	ps, _ = m.ok(OP["SendObjectInfo"], [STORAGE, top], m.objinfo("drop.bin", 5))
	m.ok(OP["GetStorageInfo"], [STORAGE])
	m.ok(OP["SendObjectInfo"], [STORAGE, top], m.objinfo("drop2.bin", 0, True))
	check("drop.bin" not in m.ls(top), "SendObjectInfo without SendObject: dropped")
	m.ok(OP["DeleteObject"], [m.ls(top)["drop2.bin"]])

	print("mtphost: broken containers get a response")
	m.tid += 1
	rc, _, _ = m.raw(OP["GetObjectInfo"], header=struct.pack("<IHHI", 40, CMD,
		OP["GetObjectInfo"], m.tid) + bytes(28))
	check(rc == RC["Invalid_Parameter"], "a command of 7 parameters")
	rc, _, _ = m.raw(OP["SendObjectInfo"], [STORAGE, top], bytes(3000))
	check(rc == RC["Invalid_Dataset"], "an ObjectInfo bigger than the buffer")
	rc, _, _ = m.raw(OP["SendObjectInfo"], [STORAGE, top], bytes(10))
	check(rc == RC["Invalid_Dataset"], "a short ObjectInfo")
	rc, _, _ = m.raw(OP["SendObjectInfo"], [STORAGE, top],
		m.objinfo("x", 0xFFFFFFFF))
	check(rc == RC["Object_Too_Large"], "4 GiB and more: Object_Too_Large")
	rc, _, _ = m.raw(OP["SendObject"], [], b"stray")
	check(rc == RC["No_Valid_ObjectInfo"], "SendObject without ObjectInfo")
	d = bytearray(m.objinfo("ok.bin", 1))
	d[52] = 200				# the name's count past the end
	rc, _, _ = m.raw(OP["SendObjectInfo"], [STORAGE, top], bytes(d))
	check(rc == RC["Invalid_Dataset"], "a string count past the end")
	m.ok(OP["GetStorageIDs"])

	print("mtphost: the example file, then the cable is pulled mid-send")
	m.ok(OP["DeleteObject"], [hs["big.bin"]])
	m.ok(OP["DeleteObject"], [h])
	m.put("etxea.sub", EXAMPLE, top)
	check(set(m.ls(top)) == {"etxea.sub"}, "galtzagorriak/etxea.sub")
	m.ok(OP["SendObjectInfo"], [STORAGE, top], m.objinfo("pulled.bin", 20000))
	m.tid += 1
	m.l.send(struct.pack("<IHHI", 12, CMD, OP["SendObject"], m.tid))
	m.l.send(struct.pack("<IHHI", 12 + 20000, DATA, OP["SendObject"], m.tid)
		+ os.urandom(2500))
	m.l.close()
	return top


def seeds(d):
	"""A few hand-made streams for the fuzzer's corpus (flipper/mtpd)."""
	out = []

	class Rec:
		def __init__(self):
			self.b = b""
	r = Rec()

	def wr(b):
		r.b += b
	l = Link(None, wr, lambda: None)
	m = Mtp(l)

	def cmd(code, params=(), data=None):
		m.tid += 1
		l.send(struct.pack("<IHHI", 12 + 4 * len(params), CMD, code, m.tid)
			+ b"".join(struct.pack("<I", p) for p in params))
		if data is not None:
			l.send(struct.pack("<IHHI", 12 + len(data), DATA, code, m.tid)
				+ data)
	cmd(OP["GetDeviceInfo"])
	cmd(OP["OpenSession"], [1])
	cmd(OP["GetStorageIDs"])
	cmd(OP["GetStorageInfo"], [STORAGE])
	cmd(OP["GetObjectHandles"], [STORAGE, 0, ROOT])
	out.append(r.b)
	cmd(OP["SendObjectInfo"], [STORAGE, ROOT], m.objinfo("a.txt", 5))
	cmd(OP["SendObject"], [], b"kaixo")
	cmd(OP["SendObjectInfo"], [STORAGE, ROOT], m.objinfo("d", 0, True))
	cmd(OP["SetObjectPropValue"], [1, PR_NAME], pstr("b.txt"))
	cmd(OP["GetObjectPropList"], [1, 0, 0xFFFFFFFF, 0, 0])
	out.append(r.b)
	cmd(OP["SendObjectInfo"], [STORAGE, ROOT], m.objinfo("c.txt", 3000))
	cmd(OP["SendObject"])
	l.send(struct.pack("<IHHI", 3012, DATA, OP["SendObject"], m.tid) + bytes(1500))
	l.cancel()
	cmd(OP["CloseSession"])
	out.append(r.b)
	os.makedirs(d, exist_ok=True)
	for i, b in enumerate(out):
		with open(os.path.join(d, "seed%d" % i), "wb") as f:
			f.write(b)


def connect(a):
	if a.spawn:
		return spawn(a.spawn.split())
	return unix(a.unix, a.wait)


def main():
	import argparse
	p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
	p.add_argument("cmd", choices=["test", "ls", "get", "put", "seeds"])
	p.add_argument("args", nargs="*")
	p.add_argument("--spawn")
	p.add_argument("--unix")
	p.add_argument("--root")
	p.add_argument("--name-max", type=int, default=60)
	p.add_argument("--wait", type=float, default=5)
	p.add_argument("--record", help="write the stream sent to mtpd here")
	a = p.parse_args()
	if a.cmd == "seeds":
		seeds(a.args[0])
		return
	if not a.spawn and not a.unix:
		p.error("--spawn or --unix")
	link = connect(a)
	if a.record:
		rec = open(a.record, "wb")
		wr = link.wr

		def wrrec(b):
			rec.write(b)
			wr(b)
		link.wr = wrrec
	m = Mtp(link)
	if a.cmd != "test":
		m.open()
		if a.cmd == "ls":
			for n, h in sorted(m.ls().items()):
				i = m.info(h)
				print("%10d %s %s%s" % (i["size"], i["mdate"], n,
					"/" if i["dir"] else ""))
		elif a.cmd == "get":
			sys.stdout.buffer.write(m.get(m.ls()[a.args[0]]))
		else:
			with open(a.args[0], "rb") as f:
				m.put(os.path.basename(a.args[0]), f.read())
		m.ok(OP["CloseSession"])
		link.close()
		return
	try:
		test(m, a.root, a.name_max)
		if hasattr(link, "proc"):
			rc = link.proc.wait(10)
			check(rc == 0, "mtpd ended on the pulled cable (status %d)" % rc)
			if a.root:
				check(not os.path.exists(os.path.join(a.root,
					"galtzagorriak", "pulled.bin")),
					"the file it was receiving is gone")
	except (AssertionError, Gone) as e:
		print("MTPHOST FAIL: %s" % (e or "mtpd closed the stream"))
		sys.exit(1)
	print("MTPHOST PASS")


if __name__ == "__main__":
	main()
