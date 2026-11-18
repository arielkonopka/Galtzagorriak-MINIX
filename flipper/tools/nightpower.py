#!/usr/bin/env python3
"""nightpower.py - the overnight battery test (AC6.6), read over the console.

Usage:
	python3 -I nightpower.py [--port /dev/ttyACM0] start [--state F]
	(unplug USB, leave the Flipper alone for the night, plug it back in)
	python3 -I nightpower.py [--port /dev/ttyACM0] end [--state F]
					[--report F.md]
	python3 -I nightpower.py --selftest

"start" reads the MINIX Flipper's version and reset cause, its clock
against this computer's, /dev/power (charge, voltage, gauge currents, and
since R6.8 the seconds up, in Stop2 and the wake-ups) and, on a latency
trace build, `lat`.  It saves them in a JSON state file (default
nightpower.json in the current directory).

"end" reads the same again and judges the night by AC6.6 (spec R6.8,
docs/spec/power-plan.md PR 2, test 2):
	- no reset during the night (uptime only grew, the cause is not
	  "watchdog");
	- the clock within the LSE tolerance (+-2 s over 8 h);
	- more than 99 % of the time in Stop2;
plus the charge used and the average current it implies, and the gauge's
own standby estimate.  It prints a report and a ready line for the Power
table in flipper/PORTING.md, and writes the report to --report.

The console must be logged in at a shell prompt (any user can read
/dev/power; `lat` needs read access to /dev/lat).  The full procedure:
docs/devguide/part2/07-overnight-power-test.md.

Linux and macOS work without extra modules; Windows needs pyserial
("pip install pyserial").  The serial code is twoflip.py's.
--selftest checks the parsing and the verdicts on canned console text:
it checks this script, not the Flipper.
"""
import argparse
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

CAPACITY_MAH = 2100		# the Flipper Zero's battery (stock: 2100 mAh)
DRIFT_LIMIT = 2.0		# s over the night (AC6.6, about 70 ppm over 8 h)
STOP2_LIMIT = 99.0		# % of the time asleep (AC6.6)
MIN_HOURS = 8.0			# AC6.6 asks for 8 h

POWER_RX = re.compile(r"soc=\S+.*")
# One Stop2 sleep is at most PM_MAX = HZ - 2 = 58 ticks (arm/klib.c), and
# each one ends in a counted wake-up: so wake-ups >= Stop2 s / this.
MAX_SLEEP_S = 58 / 60.0
WRAP = 65536			# wbtn/wother are u16 in older kernels (#284)


def unwrap_wakes(wb, wo, stop2):
	"""Wake-up deltas made right after a u16 wrap (#284).  A negative
	delta wrapped once; a count below what the Stop2 time needs wrapped
	(more) times.  Returns (wb, wo, wrapped)."""
	wrapped = False
	if wb < 0:
		wb += WRAP; wrapped = True
	if wo < 0:
		wo += WRAP; wrapped = True
	while wb + wo < stop2 / MAX_SLEEP_S * 0.999:
		wo += WRAP; wrapped = True
	return wb, wo, wrapped


def usb_since(port):
	"""When the console port appeared (the Flipper was plugged in), from
	its device node's ctime (Linux, macOS); None if unknown."""
	try:
		return os.stat(port).st_ctime
	except (OSError, ValueError):
		return None


def parse_power(text):
	"""The key=value pairs of the /dev/power line (ints where possible)."""
	m = POWER_RX.search(text)
	if not m:
		return None
	d = {}
	for kv in m.group(0).split():
		if "=" not in kv:
			continue
		k, v = kv.split("=", 1)
		try:
			d[k] = int(v, 0)
		except ValueError:
			d[k] = v
	return d


def parse_ver(text):
	d = {}
	m = re.search(r"^(Galtzagorriak \S+)", text, re.M)
	if m:
		d["version"] = m.group(1)
	m = re.search(r"^reset: (.*)$", text, re.M)
	if m:
		d["reset"] = m.group(1).strip()
	return d


def parse_lat_stop2(text):
	"""`lat`'s line "Stop2  S of U s up (P%), wake-ups: B by a button, O
	other" as a dict, or None (no trace build)."""
	m = re.search(r"Stop2\s+(\d+) of (\d+) s up \((\d+)%\), wake-ups: "
		r"(\d+) by a button, (\d+) other", text)
	if not m:
		return None
	return dict(stop2=int(m.group(1)), up=int(m.group(2)),
		pct=int(m.group(3)), wbtn=int(m.group(4)),
		wother=int(m.group(5)))


def clock_offset(mx, tries=40):
	"""The device clock minus this computer's, in seconds, to about
	+-0.1 s: `date +%s` is read until its second changes, and the change
	is put at the middle of the round trip that saw it."""
	prev = None
	for _ in range(tries):
		t0 = time.time()
		rc, out = mx.run("date -u +%s", 5)
		t1 = time.time()
		m = re.search(r"^(\d{9,})$", out, re.M)
		if rc != 0 or not m:
			return None
		s = int(m.group(1))
		if prev is not None and s != prev[0]:
			# the second turned over between prev's and this read
			edge = (prev[2] + t1) / 2
			return s - edge
		prev = (s, t0, t1)
	if prev is None:
		return None
	return prev[0] + 0.5 - (prev[1] + prev[2]) / 2


def snapshot(mx):
	snap = {"host_utc": time.time()}
	rc, out = mx.run("ver", 10)
	snap.update(parse_ver(out))
	rc, out = mx.run("cat /dev/power", 10)
	snap["power"] = parse_power(out)
	snap["power_line"] = POWER_RX.search(out).group(0) \
		if snap["power"] else None
	snap["clock_offset"] = clock_offset(mx)
	rc, out = mx.run("test -c /dev/lat && lat", 15)
	snap["lat"] = parse_lat_stop2(out) if rc == 0 else None
	snap["lat_text"] = out if rc == 0 else None
	return snap


def since_boot(b):
	"""A start reading made up for a night that was started without
	'start' (by hand, or on another computer): the counters at boot."""
	p = b.get("power") or {}
	up = p.get("up") or (b.get("lat") or {}).get("up") or 0
	a = {"host_utc": b["host_utc"] - up, "since_boot": True,
		"version": b.get("version"), "reset": b.get("reset"),
		"clock_offset": None, "power": {"up": 0, "stop2": 0,
		"wbtn": 0, "wother": 0}}
	if b.get("lat"):
		a["lat"] = dict(up=0, stop2=0, pct=0, wbtn=0, wother=0)
	return a


def judge(a, b):
	"""Compare the start snapshot a with the end snapshot b.  Returns
	(lines, verdict ok, porting.md table row)."""
	L = []
	ok = True
	hours = (b["host_utc"] - a["host_utc"]) / 3600
	if a.get("since_boot"):
		L.append("(since the last boot, %.2f h ago: the minutes on "
			"USB before the night count as awake)"
			% ((b["host_utc"] - a["host_utc"]) / 3600))
	L.append("Night: %.2f h (%s to %s UTC)" % (hours,
		time.strftime("%Y-%m-%d %H:%M", time.gmtime(a["host_utc"])),
		time.strftime("%Y-%m-%d %H:%M", time.gmtime(b["host_utc"]))))
	if hours < MIN_HOURS:
		L.append("  note: shorter than the %g h AC6.6 asks for"
			% MIN_HOURS)
	L.append("Build: %s" % b.get("version", "?"))
	if a.get("version") != b.get("version"):
		L.append("  note: the version changed (%s -> %s): not one run"
			% (a.get("version"), b.get("version")))
	pa, pb = a.get("power") or {}, b.get("power") or {}

	# 1. no reset
	reset_ok = True
	if "up" in pa and "up" in pb:
		want = pa["up"] + (b["host_utc"] - a["host_utc"]) - 120
		if pb["up"] < want:
			reset_ok = False
			L.append("FAIL reset: uptime %d s, expected about %d s: "
				"the Flipper restarted in the night (reset: %s)"
				% (pb["up"], want + 120, b.get("reset", "?")))
	if "watchdog" in b.get("reset", ""):
		if reset_ok and a.get("reset") == b.get("reset"):
			L.append("  note: reset cause still \"watchdog\" from "
				"before the start; uptime says no new reset")
		else:
			reset_ok = False
			L.append("FAIL reset: cause \"%s\"" % b["reset"])
	if reset_ok:
		L.append("PASS no reset (cause %s, up %s s)" % (
			b.get("reset", "?"), pb.get("up", "?")))
	ok &= reset_ok

	# 2. clock
	drift = None
	if a.get("since_boot"):
		L.append("???? clock: no start reading; compare `date -u` with "
			"a reference by hand (offset now %s s)" % ("%+.1f" %
			b["clock_offset"] if b.get("clock_offset") is not None
			else "?"))
	elif a.get("clock_offset") is None or b.get("clock_offset") is None:
		L.append("???? clock: could not read `date +%s`")
		ok = False
	else:
		drift = b["clock_offset"] - a["clock_offset"]
		ppm = drift / (b["host_utc"] - a["host_utc"]) * 1e6
		good = abs(drift) <= DRIFT_LIMIT
		L.append("%s clock: %+.1f s over the night (%+.1f ppm; offset "
			"%+.1f -> %+.1f s; limit +-%g s)" % ("PASS" if good
			else "FAIL", drift, ppm, a["clock_offset"],
			b["clock_offset"], DRIFT_LIMIT))
		L.append("  this computer's clock must be NTP-synchronised "
			"for this to mean anything")
		ok &= good

	# 3. Stop2
	pct = None
	src = None
	if all(k in pa and k in pb for k in ("up", "stop2")) and \
			pb["up"] > pa["up"]:
		up, st = pb["up"] - pa["up"], pb["stop2"] - pa["stop2"]
		wb = pb.get("wbtn", 0) - pa.get("wbtn", 0)
		wo = pb.get("wother", 0) - pa.get("wother", 0)
		src = "/dev/power"
	elif a.get("lat") and b.get("lat") and b["lat"]["up"] > a["lat"]["up"]:
		la, lb = a["lat"], b["lat"]
		up, st = lb["up"] - la["up"], lb["stop2"] - la["stop2"]
		wb, wo = lb["wbtn"] - la["wbtn"], lb["wother"] - la["wother"]
		src = "lat"
	if src:
		wb, wo, wrapped = unwrap_wakes(wb, wo, st)
		# On USB the Flipper never sleeps: the time since the cable went
		# back in is not part of the night.
		usb = 0
		t = b.get("usb_since")
		if t and (pb.get("usb") or pb.get("vbus")) and \
				a["host_utc"] < t <= b["host_utc"]:
			usb = int(b["host_utc"] - t)
			if 0 < usb < up - st:
				up -= usb
				L.append("(plugged in at %s UTC, %d s ago, when the "
					"port appeared: that time is left out)" % (
					time.strftime("%H:%M:%S", time.gmtime(t)), usb))
		pct = 100.0 * st / up
		good = pct > STOP2_LIMIT
		L.append("%s Stop2: %.2f %% (%d of %d s; from %s); wake-ups: "
			"%d button, %d other (%.1f per minute)" % ("PASS" if
			good else "FAIL", pct, st, up, src, wb, wo,
			wo / (up / 60.0)))
		if wrapped:
			L.append("  the kernel's u16 wake-up counter wrapped "
				"(#284): the count is corrected from the Stop2 time "
				"(each sleep <= %.2f s)" % MAX_SLEEP_S)
		if not good:
			L.append("  the minutes on USB before unplugging and "
				"after plugging back in count as awake: keep "
				"them short")
		ok &= good
	else:
		L.append("???? Stop2: no up=/stop2= in /dev/power (before "
			"R6.8, #207) and no `lat` (not a trace build)")
		ok = False

	# 4. charge
	ma = None
	if a.get("since_boot") and "soc" in pb:
		L.append("Charge now: %d %%, %s mV (no start reading)"
			% (pb["soc"], pb.get("mv", "?")))
	if "soc" in pa and "soc" in pb:
		used = pa["soc"] - pb["soc"]
		if hours > 0:
			ma = used / 100.0 * CAPACITY_MAH / hours
		L.append("Charge: %d %% -> %d %% (%d %%), %s -> %s mV; about "
			"%.2f mA average from the charge (1 %% = %d mAh; coarse)"
			% (pa["soc"], pb["soc"], used, pa.get("mv", "?"),
			pb.get("mv", "?"), ma or 0, CAPACITY_MAH // 100))
		if used <= 0:
			L.append("  the charge did not move: below the gauge's "
				"resolution, i.e. well under %.1f mA"
				% (1.0 / 100 * CAPACITY_MAH / max(hours, 0.1)))
	if "sby" in pb or "avg" in pb:
		L.append("Gauge at the end: avg %s mA, standby estimate %s "
			"mA (whole mA only)" % (pb.get("avg", "?"),
			pb.get("sby", "?")))
	if pb.get("usb") or pb.get("vbus"):
		L.append("  (read with USB connected: avg/ma show the "
			"charger, not the night)")

	L.append("")
	L.append("VERDICT: %s" % ("PASS (AC6.6)" if ok else
		"NOT PASSED: see the lines above"))
	row = "| %s | %s | %s | overnight %.1f h on battery (nightpower.py): " \
		"Stop2 %s, %d wake-ups other, clock %s, SoC %s -> %s %%, " \
		"no reset: %s |" % (
		time.strftime("%Y-%m-%d", time.gmtime(a["host_utc"])),
		b.get("version", "?"),
		"%.2f mA (SoC)" % ma if ma else "< gauge resolution",
		hours, "%.2f %%" % pct if pct is not None else "?",
		wo if src else -1,
		"%+.1f s" % drift if drift is not None else "?",
		pa.get("soc", "?"), pb.get("soc", "?"),
		"yes" if reset_ok else "NO")
	return L, ok, row


def open_minix(port, log):
	from twoflip import Minix
	mx = Minix(port, "mx", log)
	if not mx.sync():
		sys.exit("nightpower: no shell prompt on %s (log in first, "
			"and close screen/picocom/qFlipper)" % port)
	return mx


def cmd_start(args):
	mx = open_minix(args.port, args.log)
	s = snapshot(mx)
	mx.close()
	if s.get("power") is None:
		sys.exit("nightpower: no /dev/power line (QEMU has no gauge)")
	with open(args.state, "w") as f:
		json.dump(s, f, indent=1)
	p = s["power"]
	print("Start saved in %s" % args.state)
	print("  %s, reset: %s" % (s.get("version"), s.get("reset")))
	print("  charge %s %%, %s mV, up %s s, Stop2 %s s" % (p.get("soc"),
		p.get("mv"), p.get("up", "?"), p.get("stop2", "?")))
	print("  clock offset %s s" % ("%+.2f" % s["clock_offset"]
		if s["clock_offset"] is not None else "?"))
	if s.get("lat"):
		print("  lat: Stop2 %(stop2)d of %(up)d s" % s["lat"])
	print("\nNow unplug the USB cable at once and leave the Flipper alone "
		"(no keys).\nIn the morning plug it in and run:  nightpower.py "
		"end")


def cmd_end(args):
	try:
		with open(args.state) as f:
			a = json.load(f)
	except OSError:
		a = None
		print("nightpower: no %s (no 'start' on this computer): "
			"judging since the last boot; the clock and the charge "
			"cannot be judged\n" % args.state)
	mx = open_minix(args.port, args.log)
	b = snapshot(mx)
	mx.close()
	b["usb_since"] = usb_since(args.port)
	if a is None:
		a = since_boot(b)
	lines, ok, row = judge(a, b)
	out = "\n".join(lines)
	print(out)
	print("\nFor the Power table in flipper/PORTING.md:\n" + row)
	if args.report:
		with open(args.report, "w") as f:
			f.write("# Overnight battery test\n\n```\n%s\n```\n\n"
				"PORTING.md row:\n\n%s\n\nStart: `%s`\n\nEnd: `%s`\n"
				% (out, row, a.get("power_line"),
				b.get("power_line")))
			for name, s in (("start", a), ("end", b)):
				if s.get("lat_text"):
					f.write("\n`lat` at the %s:\n\n```\n%s\n```\n"
						% (name, s["lat_text"].strip()))
		print("Report written to %s" % args.report)
	return 0 if ok else 1


SELF = [
	# (start, end, expected verdict)
	(dict(host_utc=1000.0, version="Galtzagorriak 0.1.1+90.gabc",
		reset="software", clock_offset=0.3, lat=None,
		power=parse_power("soc=100 mv=4138 ma=-4 avg=-4 sby=-1 temp=230 "
		"flags=0x0181 usb=1 chg=3 vbus=1 vreg=4208 ichg=2048 otg=0 "
		"up=600 stop2=0 wbtn=0 wother=0")),
	 dict(host_utc=1000.0 + 10 * 3600, version="Galtzagorriak 0.1.1+90.gabc",
		reset="software", clock_offset=0.9, lat=None,
		power=parse_power("noise\r\nsoc=99 mv=4121 ma=-3 avg=-12 sby=-1 "
		"temp=228 flags=0x0181 usb=1 chg=0 vbus=1 vreg=4208 ichg=2048 "
		"otg=0 up=36660 stop2=35900 wbtn=1 wother=37200")), True),
	# a watchdog reset in the night
	(dict(host_utc=0.0, version="G 1", reset="software", clock_offset=0.0,
		power=dict(soc=90, up=100, stop2=0)),
	 dict(host_utc=9 * 3600.0, version="G 1", reset="watchdog",
		clock_offset=0.0, power=dict(soc=88, up=3000, stop2=2990)),
	 False),
	# the clock drifted 5 s
	(dict(host_utc=0.0, reset="power on", clock_offset=0.0,
		power=dict(soc=80, up=10, stop2=0)),
	 dict(host_utc=8 * 3600.0, reset="power on", clock_offset=-5.0,
		power=dict(soc=79, up=10 + 8 * 3600, stop2=8 * 3600 - 60)),
	 False),
	# no start file: since boot (a trace build without R6.8)
	(None, dict(host_utc=50000.0, version="G 2", reset="software",
		clock_offset=0.4, power=dict(soc=93, mv=4050),
		lat=parse_lat_stop2("Stop2  39800 of 40000 s up (99%), "
		"wake-ups: 0 by a button, 39700 other")), True),
	# an older kernel: Stop2 from lat only
	(dict(host_utc=0.0, reset="software", clock_offset=0.0,
		power=dict(soc=100), lat=parse_lat_stop2(
		"Stop2  0 of 50 s up (0%), wake-ups: 0 by a button, 0 other")),
	 dict(host_utc=11 * 3600.0, reset="software", clock_offset=1.0,
		power=dict(soc=100), lat=parse_lat_stop2(
		"Stop2  39500 of 39650 s up (99%), wake-ups: 2 by a button, "
		"40900 other")), True),
	# awake too much (USB left in, say)
	(dict(host_utc=0.0, reset="software", clock_offset=0.0,
		power=dict(soc=100, up=0, stop2=0)),
	 dict(host_utc=8 * 3600.0, reset="software", clock_offset=0.0,
		power=dict(soc=100, up=8 * 3600, stop2=4 * 3600)), False),
	# the first real run (2026-11-17/18): no start file, wother wrapped
	# (u16, #284), and 24 min on USB before `end`.  Without the USB
	# minutes it is 98.2 %, still under 99 %.
	(None, dict(host_utc=73428.0, version="Galtzagorriak 0.1.1+124.g0566ba1",
		reset="software", clock_offset=-1.6, usb_since=73428.0 - 1470,
		power=parse_power("soc=99 mv=4120 ma=0 avg=0 sby=-2 temp=240 "
		"flags=0x5028 usb=1 chg=3 vbus=2 vreg=4208 ichg=2048 otg=0 "
		"up=73428 stop2=70677 wbtn=151 wother=11607")), False),
	# a clean night across a u16 wrap: 65000 -> 30000 is 30536 wake-ups
	(dict(host_utc=0.0, reset="software", clock_offset=0.0,
		power=dict(soc=90, up=1000, stop2=0, wbtn=0, wother=65000)),
	 dict(host_utc=29200.0, reset="software", clock_offset=0.5,
		power=dict(soc=89, up=30200, stop2=29000, wbtn=0, wother=30000,
		usb=1), usb_since=29100.0), True),
]


def selftest():
	bad = 0
	assert parse_ver("Galtzagorriak 0.1.1+3.gx\nreset: watchdog (dev "
		"mode: a watchdog reset goes to DFU)\n") == {
		"version": "Galtzagorriak 0.1.1+3.gx",
		"reset": "watchdog (dev mode: a watchdog reset goes to DFU)"}
	assert parse_power("x soc=5 flags=0x0181 up=7")["flags"] == 0x181
	assert unwrap_wakes(151, 11607, 70677) == (151, 11607 + WRAP, True)
	assert unwrap_wakes(0, 30000 - 65000, 29000) == (0, 30536, True)
	assert unwrap_wakes(1, 37200, 35900) == (1, 37200, False)
	assert unwrap_wakes(0, 200000, 190000) == (0, 200000, False)	# u32
	for i, (a, b, want) in enumerate(SELF):
		lines, ok, row = judge(a or since_boot(b), b)
		if ok != want:
			bad += 1
			print("case %d: got %s, want %s\n%s" % (i, ok, want,
				"\n".join(lines)))
	print("selftest: %d of %d cases %s" % (len(SELF) - bad, len(SELF),
		"ok" if not bad else "FAILED"))
	return 1 if bad else 0


def main():
	ap = argparse.ArgumentParser(description="The overnight battery "
		"test (AC6.6) of a MINIX Flipper, over its USB console.")
	ap.add_argument("--port", default="/dev/ttyACM0",
		help="the MINIX Flipper's console (default /dev/ttyACM0; "
		"Windows: COM3 etc.)")
	ap.add_argument("--state", default="nightpower.json",
		help="where start keeps its readings (default nightpower.json)")
	ap.add_argument("--report", default=None,
		help="end: also write the report to this Markdown file")
	ap.add_argument("--log", default=None, help="log the console here")
	ap.add_argument("--selftest", action="store_true",
		help="check this script on canned text, no hardware")
	ap.add_argument("what", nargs="?", choices=["start", "end"])
	args = ap.parse_args()
	if args.selftest:
		return selftest()
	if not args.what:
		ap.error("say start or end (or --selftest)")
	args.log = open(args.log, "a") if args.log else None
	if args.what == "start":
		cmd_start(args)
		return 0
	return cmd_end(args)


if __name__ == "__main__":
	sys.exit(main())
