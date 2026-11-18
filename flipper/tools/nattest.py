#!/usr/bin/env python3
"""nattest - the native apps remind and alarm (spec 7b.1, 7b.2) in QEMU.

Usage: nattest.py

Boots a fresh copy of build/qemu/minix/disk.img (make minix disk, BOARD
qemu, the real INIT) like tools/powercut.py, logs in as root, installs the
timed, remind and alarm packages and checks:

  F.4      each package adds its own user (group timers) and its line on
           the launcher; apps_data/<app> belongs to that user
  7b.1     OK on a reminder's alert (Log: remind) appends "TIME remind-N"
           to apps_data/remind/log, written as the user remind; remind -s
           counts it today; Back does not count; timed -n fires a
           reminder and OK on it writes the log line
  7b.2     snooze makes the timer _ID with the tone, ring time, mode,
           stage, volume, sound type, outputs and log of the alert; the
           backlight is as before after a screen flash; /dev/speaker takes
           the volume note (0xFFFF) and beep -v; alarm -n gives the next alarm that is on (an
           alarm that is off, apps_data/alarm/alarm-N.off, is not); timed
           fires an alarm that has a tone; each alert gives the screen
           back when it ends (the next one shows)
  ACK8.1   Kanpaia (remind 1.1) is a window app (spec K.8): the
           launcher starts it as the user remind; main -> + New reminder
           -> the form -> the keyboard -> Save makes /var/timers/remind-1;
           Upcoming lists it, OK on it opens its form, Back closes all
  ACK8.3   free -v with Kanpaia open: at most 4.0 KB more than the
           launcher alone
  ACK8.2   Ordua (alarm 1.1) is a window app too: through the same
           launcher it opens the desk clock, makes an alarm through the
           edit form (time, days) and the ring settings (mode, volume,
           melody, vibro); Save brings the list back, the desk clock then
           shows the next alarm, Back closes it
  ACK8.3   free -v with Ordua open: at most 4.5 KB more than the
           launcher alone; free -v with alert open gives the RAM it takes
           (printed as "RAM:" lines, issue #201)
  AC11.3.2 Teklatua (usbrem 1.0, spec R11.3.5) as the user usbremote:
           typing "Kaixo!" on the Keyboard page (the desktop's keyboard
           in a LIVE window, a submit with the text after each key)
           prints its key reports and key-ups (hid< kbd), Del a
           Backspace, the Enter row Enter; Media "Volume +"
           prints one consumer report; the Mouse pad (K.8 pad window)
           prints a move report on the press and per key repeat, OK a
           click; a lone Back leaves the pad, Back again closes it
  ACK8.3   free -v with Teklatua open: at most 4.5 KB more than the
           launcher alone

What needs the RTC alarm (AC7b.2.1, AC7b.R1) is on the device list.
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
KEY = {"up": "001", "down": "002", "left": "004", "right": "010",
       "ok": "020", "back": "040"}


def check(ok, what):
	global fails
	print(("  ok   " if ok else "  FAIL ") + what, flush=True)
	if not ok:
		fails += 1


def keyfiles(r):
	"""/tmp/kNNN: one /dev/buttons byte each (the console takes no \\0)."""
	for k in list(KEY.values()) + ["000"]:
		r.sh("echo k | tr k '\\%s' | dd of=/tmp/k%s bs=1 count=1 "
		     "2>/dev/null" % (k, k), 20)
	# /tmp/kbl: one /dev/backlight byte, the light on at 255 (the
	# level QEMU has); the write restarts its idle timeout (tap(lit))
	r.sh("echo k | tr k '\\377' | dd of=/tmp/kbl bs=1 count=1 "
	     "2>/dev/null", 20)


def tap(r, key, wait=1, lit=False):
	"""A short press of a fake button (QEMU /dev/buttons): down, held
	for one cat (more than a clock tick, less than a long press), up.
	lit: the backlight on first, so that the press is a key even after
	a long wait: with the light off (its idle timeout, 15 s in QEMU) a
	press only turns it on, and every later key would be one off."""
	r.sh("%scat /tmp/k%s >/dev/buttons; cat /etc/motd >/dev/null; "
	     "cat /tmp/k000 >/dev/buttons; sleep %d"
	     % ("cat /tmp/kbl >/dev/backlight; " if lit else "", KEY[key],
		wait), 20)


def timer(r, tid, lines, owner):
	r.sh("rm -f /var/timers/%s" % tid, 20)
	for i, l in enumerate(lines):
		r.sh("echo '%s' %s /var/timers/%s" % (l, ">" if i == 0 else ">>",
			tid), 20)
	r.sh("chmod 600 /var/timers/%s; chown %s /var/timers/%s" % (tid, owner,
		tid), 20)


def freebytes(r):
	"""Free RAM in bytes: the sum of the holes free -v lists."""
	out = r.output("free -v", 20) or b""
	return sum(int(m.group(1)) for m in
		   re.finditer(rb"(?m)^  hole at \S+ +(\d+) bytes", out))


def ram(r, what, base):
	now = freebytes(r)
	print("  RAM: %-6s open: %6d bytes (free %d -> %d)" % (what,
		base - now, base, now), flush=True)


def seen(r, mark, pat, secs=30):
	"""Whether pat (bytes regex) shows on the console after r.out[mark],
	waiting up to secs for it (QEMU can be slow to start a program)."""
	old, r.seen = r.seen, mark
	m = r.wait(pat, secs)
	r.seen = max(old, r.seen)
	return m is not None


def alert_up(r, mark, secs=30):
	"""Wait until an alert started after mark has the screen: its console
	line, then its first frame.  A key sent before that goes to the
	launcher instead, and the alert would then stay up for its ring time."""
	return seen(r, mark, rb"alert: [^\n]*\n.*?\+-{40}", secs)


def win_tap(r, key, pat=rb"gui< show", secs=20):
	"""A tap that makes the app send a window: wait for its records."""
	mark = len(r.out)
	tap(r, key, lit=True)
	return seen(r, mark, pat, secs)


def keys(r, seq):
	"""Taps in order.  "key@id": the app answers it with the window id,
	wait for it; "key!": it closes the app, wait for the close event;
	plain keys the desktop handles by itself.  Each one with the light
	on (tap(lit)): a wait that runs long must not eat the next key."""
	for k in seq:
		if "@" in k:
			k, w = k.split("@")
			win_tap(r, k, rb"gui< win\t" + w.encode() + rb"\t.*?gui< show")
		elif k.endswith("!"):
			win_tap(r, k[:-1], rb"gui> close")
		else:
			tap(r, k, lit=True)


def file_out(r, cmd, ok, secs=20):
	"""The output of cmd once ok(output) holds, polled for up to secs
	(a file an app writes after its window: QEMU can be slow), else the
	last output."""
	end = time.time() + secs
	while True:
		out = r.output(cmd, 20)
		if (out is not None and ok(out)) or time.time() >= end:
			return out
		r.sh("sleep 1", 20)


def ram_back(r, base, secs=20):
	"""Free RAM once it is back to base (an app that ended: its memory
	is freed when the launcher has reaped it), polled for up to secs."""
	end = time.time() + secs
	while True:
		now = freebytes(r)
		if now >= base - 256 or time.time() >= end:
			return now
		r.sh("sleep 1", 20)


def hm(t):
	return time.strftime("%H:%M", time.gmtime(t))


LOGF = "/home/flipper/apps_data/remind/log"


def hold(r, key, secs=1):
	"""A fake button held down for secs, then let go (repeats)."""
	r.sh("cat /tmp/kbl >/dev/backlight; cat /tmp/k%s >/dev/buttons; "
	     "sleep %d; cat /tmp/k000 >/dev/buttons; sleep 1"
	     % (KEY[key], secs), 30)


def win_only(r, prog):
	"""The launcher of /etc/rc with one window app under its cursor.  It
	read /etc/apps at boot; it reads it again when an app it ran ends, so
	an /etc/apps of that app alone puts it under the cursor.  GuiLog: on
	puts the window records (gui<) and events (gui>) on the console.
	Returns the free RAM with the launcher alone (free -t on)."""
	r.sh("cp /etc/apps /tmp/apps.bak; rm -f /tmp/lc.bak; "
	     "test -f /etc/launcher.conf && cp /etc/launcher.conf /tmp/lc.bak; "
	     "grep ':=/usr/bin/%s$' /tmp/apps.bak > /etc/apps; "
	     "echo 'GuiLog: on' >> /etc/launcher.conf" % prog, 20)
	tap(r, "back")		# lights the screen; Back on a page: the list
	tap(r, "back")
	# whichever app the cursor is on (the Console after boot, Kanpaia
	# after kanpaia()); Back ends it and the launcher reads both files
	# again.  Wait until its RAM is free: a full-screen app can take a
	# few seconds in QEMU, and a key meanwhile would go to it.
	before = freebytes(r)
	tap(r, "ok", 3)
	tap(r, "back", 3)
	for _ in range(15):
		if freebytes(r) >= before - 256:
			break
		r.sh("sleep 2", 20)
	r.sh("free -t on", 20)
	return freebytes(r)


def win_restore(r):
	r.output("free -t off; cp /tmp/apps.bak /etc/apps; "
		 "if test -f /tmp/lc.bak; then cp /tmp/lc.bak /etc/launcher.conf; "
		 "else rm -f /etc/launcher.conf; fi", 20)


def kanpaia(r):
	"""ACK8.1, ACK8.3: Kanpaia through the launcher of /etc/rc."""
	base = win_only(r, "remind")
	mark = len(r.out)
	win_tap(r, "ok", rb"gui< win\tmain\t.*?gui< show")	# Kanpaia
	now = freebytes(r)
	print("  RAM: Kanpaia open: %d bytes (free %d -> %d)" % (base - now,
		base, now), flush=True)
	check(re.search(rb"gui< win\tmain\tlist\tKanpaia", r.out[mark:])
		is not None, "ACK8.1: the launcher starts Kanpaia, its main window")
	check(0 < base - now <= 4096,
		"ACK8.3: Kanpaia open takes <= 4.0 KB more than the launcher "
		"alone (%d B)" % (base - now))
	# main: Up wraps to + New reminder; OK: the form, the cursor on Text;
	# OK: the keyboard, OK types the key under its cursor ('a'), Up: the
	# command row, Right x3: Done, OK; Up wraps to Save; OK.
	keys(r, ("up", "ok@edit", "ok", "ok", "up", "right", "right", "right",
		 "ok", "up", "ok@main"))
	# The desktop sends the text and Save before Kanpaia answers with
	# main; the timer file is written by then, but give QEMU time.
	seen(r, mark, rb"gui> submit\tedit\ttext=.*?gui> ok\tedit\tsave", 20)
	out = file_out(r, "cat /var/timers/remind-1; ls -l /var/timers/remind-1",
		lambda o: b"Log: remind" in o and b"remind " in o)
	check(out is not None and b"Kind: interval" in out
		and b"Start: 09:00" in out and b"Every: 30" in out
		and b"Text: Drink a glass of watera" in out
		and b"Log: remind" in out and b"Category: reminder" in out
		and re.search(rb"-rw------- .*remind ", out) is not None,
		"ACK8.1: list -> form -> keyboard -> Save made remind-1 "
		"(09:00-17:00 every 30, the text typed, the user remind's)")
	if fails: print(out)
	check(re.search(rb"gui> submit\tedit\ttext=Drink a glass of watera",
		r.out[mark:]) is not None
		and re.search(rb"gui> ok\tedit\tsave", r.out[mark:]) is not None,
		"ACK8.1: the desktop sends the typed text, then Save")
	# Save shows main again: Down to Upcoming, OK: the list with
	# remind-1; OK on it: its form (Delete too); Back, Back, Back: closed
	mark = len(r.out)
	keys(r, ("down", "ok@next", "ok@edit"))
	check(re.search(rb"gui< win\tnext\tlist\tUpcoming", r.out[mark:])
		is not None and re.search(rb"gui< item\tremind-1\tDrink a glass "
		rb"of watera\t", r.out[mark:]) is not None,
		"Kanpaia: Upcoming lists remind-1 with its next time")
	check(re.search(rb"gui< win\tedit\tlist\tReminder", r.out[mark:])
		is not None and re.search(rb"gui< action\tdel\tDelete",
		r.out[mark:]) is not None,
		"Kanpaia: OK on it opens its form, with Delete")
	if fails: print(r.out[mark:].decode(errors="replace"))
	# Back, Back: main (sent again); Down x2: History, OK: its page
	# (a line as alert writes it on OK, so this does not depend on the
	# alert checks above)
	r.sh("echo '%d remind-1' >> %s; chown remind %s"
	     % (int(time.time()), LOGF, LOGF), 20)
	mark = len(r.out)
	keys(r, ("back", "back@main", "down", "down", "ok@hist"))
	check(re.search(rb"gui< win\thist\tlist\tHistory", r.out[mark:])
		is not None and re.search(rb"gui< item\tremind-1\tDrink a glass "
		rb"of watera\t", r.out[mark:]) is not None,
		"Kanpaia: History lists the reminders marked done")
	if fails: print(r.out[mark:].decode(errors="replace"))
	keys(r, ("back", "back!"))
	after = ram_back(r, base)
	check(after >= base - 256,	# (others may have ended meanwhile)
		"Kanpaia: Back closes it, its RAM is free again (%d -> %d)"
		% (base, after))
	for m in re.finditer(rb"memuse: remind .*", r.out):
		print("  " + m.group(0).decode().strip())
	win_restore(r)


def ordua(r):
	"""ACK8.2, ACK8.3: Ordua through the launcher of /etc/rc."""
	base = win_only(r, "alarm")
	mark = len(r.out)
	win_tap(r, "ok", rb"gui< win\tlist\t.*?gui< show")	# Ordua
	now = freebytes(r)
	print("  RAM: Ordua open: %d bytes (free %d -> %d)" % (base - now,
		base, now), flush=True)
	check(0 < base - now <= 4608,
		"ACK8.3: Ordua open takes <= 4.5 KB more than the launcher "
		"alone (%d B)" % (base - now))
	log = lambda: r.out[mark:].replace(b"\r", b"")
	check(re.search(rb"gui< win\tlist\tlist\tOrdua", log()) is not None
	      and b"gui< item\tclock\tDesk clock" in log(),
	      "ACK8.2: the launcher starts Ordua, its list window")
	win_tap(r, "ok", rb"gui< win\tclock\t.*?gui< show")	# Desk clock
	check(re.search(rb"gui< win\tclock\tclock\t", log()) is not None
	      and b"gui< line\tNo alarm on" in log(),
	      "ACK8.2: the desk clock window (no alarm yet)")
	keys(r, ("back",))			# the list, kept by the desktop
	keys(r, ("down", "ok@edit",		# + New alarm: the form
		 "ok", "up", "ok",		# Time: 07 -> 08
		 "down", "down", "ok",		# Days: edit
		 "right", "right", "right", "right", "right", "up", "ok",
						# Saturday on
		 "down", "down", "down", "ok@ring",	# Ring settings
		 "right",			# Mode: steady
		 "down", "down", "left",	# Volume: 90
		 "down", "down", "down", "right",	# Melody: chime
		 "down", "left",		# Vibro: off
		 "back",			# the form again (kept)
		 "down", "ok@list"))		# Save: the list
	r.sh("sleep 2", 20)
	out = log()
	check(b"gui< win\tedit\tlist\tNew alarm" in out
	      and b"gui< win\tring\tlist\tRing" in out
	      and re.search(rb"gui> submit\tedit\t[^\n]*at=08:00", out) is not None
	      and re.search(rb"gui> submit\tring\t[^\n]*vol=90", out) is not None
	      and b"gui> ok\tedit\tsave" in out,
	      "ACK8.2: list -> form -> ring settings -> Save (the desktop "
	      "changes the values, the app gets them in submit)")
	if fails: print(out.decode("latin-1")[-3000:])
	out = file_out(r, "cat /var/timers/alarm-1; ls -l /var/timers/alarm-1; "
		       "/usr/bin/alarm -n", lambda o: re.search(
		       rb"alarm-1 \d+ \d\d:\d\d ", o) is not None)
	check(out is not None and b"Kind: daily" in out
	      and b"Time: 08:00" in out and b"Days: Mo Tu We Th Fr Sa" in out
	      and b"Category: alarm" in out and b"Out: sound,led,screen" in out
	      and b"Mode: escalating" not in out and b"Volume: 90" in out
	      and b"Tone: chime" in out
	      and re.search(rb"-rw------- .*alarm ", out) is not None
	      and re.search(rb"alarm-1 \d+ 08:00 Alarm\n", out) is not None,
	      "ACK8.2: Ordua made alarm-1 as the user alarm (08:00 Mo-Sa, "
	      "steady, no vibro, Volume 90, chime), alarm -n")
	if fails: print(out)
	mark2 = len(r.out)
	win_tap(r, "ok", rb"gui< win\tclock\t.*?gui< show")	# Desk clock
						# (the list after Save starts
						# at its first row)
	out = r.out[mark2:].replace(b"\r", b"")
	check(re.search(rb"gui< line\tNext (\w\w\w )?08:00", out) is not None
	      and b"gui< line\tAlarm" in out,
	      "ACK8.2: the desk clock shows the next alarm")
	keys(r, ("back", "back!"))		# closes Ordua
	check(b"gui> close" in r.out[mark2:],
	      "ACK8.2: Back on the list closes Ordua")
	after = ram_back(r, base)
	check(after >= base - 256,
		"Ordua: Back closes it, its RAM is free again (%d -> %d)"
		% (base, after))
	for m in re.finditer(rb"memuse: alarm .*", r.out[mark:]):
		print("  " + m.group(0).decode().strip())
	win_restore(r)


def teklatua(r):
	"""AC11.3.2, ACK8.3: Teklatua through the launcher of /etc/rc."""
	base = win_only(r, "usbremote")
	mark = len(r.out)
	win_tap(r, "ok", rb"gui< win\tmain\t.*?gui< show")	# Teklatua
	now = freebytes(r)
	print("  RAM: Teklatua open: %d bytes (free %d -> %d)" % (base - now,
		base, now), flush=True)
	log = lambda: r.out[mark:].replace(b"\r", b"")
	check(re.search(rb"gui< win\tmain\tlist\tTeklatua\tLIVE", log())
	      is not None and b"gui< enum\tpage\tPage\t0\tKeyboard|" in log()
	      and b"gui< line\tUSB: ready" in log(),
	      "AC11.3.2: the launcher starts Teklatua, its Keyboard page, USB ready")
	check(0 < base - now <= 4608,
		"ACK8.3: Teklatua open takes <= 4.5 KB more than the launcher "
		"alone (%d B)" % (base - now))
	# The Keyboard page: Down x2 to Type, OK opens the keyboard (the
	# cursor on 'a').  K: Up to Aa, OK (shift), Down, Right x10, OK;
	# Up (Spc), Left x2 (Aa), OK (no shift); Down 'a' OK; Right x8 'i'
	# OK; Down 'y', Left 'x' OK; Up 'h', Right x7 'o' OK; Down x2 '@',
	# Left x2 '!' OK.  Each key is a submit with the text so far; the
	# app types what is new: one key report and its key-up.
	mk = len(r.out)
	keys(r, ("down", "down", "ok", "up", "ok", "down") + ("right",) * 10
	     + ("ok", "up", "left", "left", "ok", "down", "ok") + ("right",) * 8
	     + ("ok", "down", "left", "ok", "up") + ("right",) * 7
	     + ("ok", "down", "down", "left", "left", "ok"))
	r.sh("sleep 2", 20)
	out = r.out[mk:].replace(b"\r", b"")
	z = b"hid< kbd 00 00 00 00 00 00 00 00\n"
	want = []
	for mod, use in (("02", "0e"), ("00", "04"), ("00", "0c"), ("00", "1b"),
			 ("00", "12"), ("02", "1e")):	# K a i x o !
		want += [("hid< kbd %s 00 %s 00 00 00 00 00\n" % (mod, use)).encode(), z]
	got = re.findall(rb"hid< kbd [0-9a-f ]+\n", out)
	check(got == want
	      and re.search(rb"gui> submit\tmain\tkbd=K\n", out) is not None
	      and re.search(rb"gui> submit\tmain\tkbd=Kaixo!\n", out) is not None,
	      "AC11.3.2: \"Kaixo!\" on the Keyboard page: K a i x o ! and a "
	      "key-up after each (hid< kbd), a submit per key")
	if fails: print(b"".join(got).decode(errors="replace"))
	# Del on the keyboard (Down x2: the command row under '!', Done;
	# Left x2: Del): Backspace; Back closes it; Down: the Enter row, OK
	mk = len(r.out)
	keys(r, ("down", "down", "left", "left", "ok", "back", "down"))
	tap(r, "ok", 2)
	out = r.out[mk:].replace(b"\r", b"")
	check(re.search(rb"gui> submit\tmain\tkbd=Kaixo\n", out) is not None
	      and b"hid< kbd 00 00 2a 00 00 00 00 00\nhid< kbd 00 00 00 00 00 00 00 00"
	      in out and b"hid< kbd 00 00 28 00 00 00 00 00\nhid< kbd 00 00 00 00 00 00 00 00"
	      in out, "AC11.3.2: Del on the keyboard types Backspace, the Enter "
	      "row Enter")
	if fails: print(out.decode(errors="replace")[-1500:])
	# Up x3: the page row; Right x2: Media
	mk = len(r.out)
	keys(r, ("up", "up", "up", "right", "right@main", "down"))
	tap(r, "ok", 2)				# Volume +
	out = r.out[mk:].replace(b"\r", b"")
	check(out.count(b"hid< cons e9 00\n") == 1
	      and b"hid< cons e9 00\nhid< cons 00 00" in out
	      and b"gui< action\tvolup\tVolume +" in out,
	      "AC11.3.2: Media \"Volume +\" prints one consumer report (and "
	      "its release)")
	if fails: print(out.decode(errors="replace")[-1500:])
	# Up, Right: the Mouse page; Down, OK: the pad (K.8 kind pad)
	mk = len(r.out)
	keys(r, ("up", "right@main", "down", "ok@mouse"))
	out = r.out[mk:].replace(b"\r", b"")
	check(re.search(rb"gui< win\tmouse\tpad\tMouse\tREPEAT=10\n", out)
	      is not None, "K.8: the Mouse pad is a pad window (REPEAT=10)")
	mk = len(r.out)
	hold(r, "right")
	tap(r, "ok", 2)				# a left click
	out = r.out[mk:].replace(b"\r", b"")
	reps = out.count(b"gui> key\tright\trepeat\n")
	moves = len(re.findall(rb"hid< mouse 00 0[1248] 00 00\n", out))
	print("  pad: %d repeats, %d move reports" % (reps, moves), flush=True)
	check(b"gui> key\tright\tpress\n" in out and b"gui> key\tright\trelease\n"
	      in out and reps >= 5 and moves == reps + 1,
	      "AC11.3.2: the Mouse pad prints a move report on the press and "
	      "per key repeat (raw key events)")
	check(b"hid< mouse 01 00 00 00\nhid< mouse 00 00 00 00" in out,
	      "the Mouse pad: OK clicks the left button")
	if fails: print(out.decode(errors="replace")[-2500:])
	mk = len(r.out)
	keys(r, ("back",))
	r.sh("sleep 1", 20)
	check(b"gui> back\tmouse" in r.out[mk:],
	      "K.8: a lone Back leaves the pad (Back always works)")
	keys(r, ("back!",))
	after = ram_back(r, base)
	check(after >= base - 256,
		"Teklatua: Back closes it, its RAM is free again (%d -> %d)"
		% (base, after))
	for m in re.finditer(rb"memuse: usbremot .*", r.out[mark:]):
		print("  " + m.group(0).decode().strip())
	win_restore(r)


def main():
	r = powercut.Run(0, 0)
	try:
		if not r.login():
			check(False, "login")
			return 1
		for p in ("timed_1.2", "remind_1.1", "alarm_1.1", "usbrem_1.0"):
			check(r.sh("pkg install /lib/pkg/%s.tar" % p, 120) == 0,
				"install the %s package" % p)
		out = r.output("grep -c '^remind:.:21:11:' /etc/passwd; "
			"grep -c '^alarm:.:22:11:' /etc/passwd; "
			"grep -c ':remind:@bell:=/usr/bin/remind$' /etc/apps; "
			"grep -c '^Ordua:alarm:@alarm:=/usr/bin/alarm$' /etc/apps", 20)
		check(out is not None and out.split() == [b"1"] * 4,
			"F.4: users remind and alarm (group timers), launcher lines "
			"(Kanpaia and Ordua window apps as their users)")
		if fails: print(out)
		out = r.output("ls -ld /home/flipper/apps_data/remind", 20)
		check(out is not None and re.search(rb"drwx------ .*remind +timers",
			out) is not None, "apps_data/remind is remind's, mode 700")
		out = r.output("/usr/bin/remind -s", 20)
		check(out is not None and out.startswith(b"today 0\nweek"),
			"remind -s: nothing yet today")
		keyfiles(r)

		# 7b.1: OK on the alert -> one log line, written as remind.
		base = freebytes(r)
		mark = len(r.out)
		r.sh("/usr/lib/alert -c reminder -f 0 -u 21 -l remind remind-1 "
		     "Reminder Water & sleep 2", 20)
		alert_up(r, mark)
		ram(r, "alert", base)
		tap(r, "left")		# the first press only lights the screen
		tap(r, "ok", 2)
		out = r.output("cat /home/flipper/apps_data/remind/log", 20)
		check(out is not None and re.match(rb"\d+ remind-1\n$", out)
			is not None, "7b.1: OK on the alert appends TIME remind-1")
		out = r.output("ls -l /home/flipper/apps_data/remind/log", 20)
		check(out is not None and re.search(rb"-rw------- .*remind ", out)
			is not None, "7b.1: the log is written as the user remind")
		mark = len(r.out)
		r.sh("/usr/lib/alert -c reminder -f 0 -u 21 -l remind remind-1 "
		     "Reminder Water & sleep 2", 20)
		alert_up(r, mark)
		tap(r, "left")
		tap(r, "back", 2)
		out = r.output("/usr/bin/remind -s", 20)
		check(out is not None and re.match(rb"today 1\nweek( 0){6} 1\n", out)
			is not None, "7b.1: remind -s counts the OK, not the Back")
		if fails: print(out)

		# 7b.2: snooze keeps the alarm's ring settings.  -f 16: only
		# the screen flash; the backlight is as before afterwards.
		r.sh("rm -f /var/timers/_alarm-9", 20)
		bl0 = r.output("od -An -tx1 /dev/backlight", 20)
		mark = len(r.out)
		r.sh("/usr/lib/alert -c alarm -f 16 -t rise -r 15 -R -S 20 -1 "
		     "-v 40 -z 5 alarm-9 -u 22 Wake Wake & sleep 2", 20)
		alert_up(r, mark)
		tap(r, "left")
		tap(r, "up", 2)
		out = r.output("cat /var/timers/_alarm-9; ls -l /var/timers/_alarm-9",
			20)
		check(out is not None and b"Tone: rise" in out and b"Ring: 15" in out
			and b"Mode: escalating\nStep: 20" in out
			and b"Volume: 40" in out and b"Sound: single" in out
			and b"Out: screen" in out and b"Category: alarm" in out
			and re.search(rb"-rw------- .*alarm ", out) is not None,
			"7b.2: snooze: _alarm-9 keeps tone, ring, mode, stage, "
			"volume, sound type, outputs")
		if fails: print(out)
		bl1 = r.output("od -An -tx1 /dev/backlight", 20)
		check(bl0 is not None and bl0 == bl1,
			"7b.2: the backlight is as before after the screen flash")
		if bl0 != bl1: print(bl0, bl1)
		r.sh("rm -f /var/timers/_alarm-9", 20)

		# The speaker volume (/dev/speaker note 0xFFFF): a volume-only
		# write returns at once; beep -v plays.
		r.sh("echo xxyz | tr xyz '\\377\\036\\000' | dd of=/tmp/vol bs=1 "
		     "count=4 2>/dev/null", 20)
		out = r.output("cat /tmp/vol >/dev/speaker; echo st=$?; "
			"beep -v 30 1000 50 1500 50; echo st=$?; "
			"beep -v 101 1000 20; echo st=$?", 30)
		check(out is not None and out.count(b"st=0") == 3,
			"/dev/speaker: the volume note, beep -v")
		if fails: print(out)

		# 7b.2: the next alarm; one that is off does not count.
		now = int(time.time())
		at1, at2 = now + 3 * 3600, now + 3600
		timer(r, "alarm-1", ["Kind: daily", "Time: %s" % hm(at1),
			"Category: alarm", "Missed: fire-once", "Title: Wake",
			"Text: Wake", "Tone: classic"], "alarm")
		r.sh("echo 'Kind: daily' > /home/flipper/apps_data/alarm/alarm-2.off;"
		     " echo 'Time: %s' >> /home/flipper/apps_data/alarm/alarm-2.off;"
		     " echo 'Category: alarm' >> /home/flipper/apps_data/alarm/alarm-2.off;"
		     " echo 'Title: Off' >> /home/flipper/apps_data/alarm/alarm-2.off;"
		     " chown alarm /home/flipper/apps_data/alarm/alarm-2.off"
		     % hm(at2), 20)
		out = r.output("/usr/bin/alarm -n", 20)
		check(out is not None and re.match((r"alarm-1 \d+ %s Wake\n" %
			hm(at1)).encode(), out) is not None,
			"7b.2: alarm -n: the next alarm that is on")
		if fails: print(out)

		# timed fires an alarm with its tone (Tone: goes to alert -t).
		timer(r, "alarm-3", ["Kind: once", "At: %d" % now,
			"Category: alarm", "Title: Now", "Text: now", "Tone: pips",
			"Ring: 1"], "alarm")
		r.sh("echo %d > /var/timers/.last" % (now - 10), 20)
		mark = len(r.out)
		r.output("/usr/lib/timed -v -n %d; sleep 2" % (now + 1), 30)
		check(seen(r, mark, rb"alert: \d\d:\d\d alarm: Now: now"),
			"7b.2: timed fires the alarm (Tone: pips)")
		alert_up(r, mark)
		tap(r, "left")
		tap(r, "back", 2)	# dismiss it

		# 7b.1: timed fires a reminder (Log: remind); OK on its alert
		# writes the log line.
		timer(r, "remind-2", ["Kind: once", "At: %d" % now,
			"Category: reminder", "Title: Reminder", "Text: Stretch",
			"Log: remind"], "remind")
		r.sh("echo %d > /var/timers/.last" % (now - 10), 20)
		mark = len(r.out)
		r.output("/usr/lib/timed -v -n %d; sleep 2" % (now + 1), 30)
		check(seen(r, mark,
			rb"alert: \d\d:\d\d reminder: Reminder: Stretch"),
			"7b.1: timed fires the reminder")
		alert_up(r, mark)
		tap(r, "left")
		tap(r, "ok", 2)
		out = r.output("grep remind-2 /home/flipper/apps_data/remind/log",
			20)
		check(out is not None and re.match(rb"\d+ remind-2\n$", out)
			is not None, "7b.1: OK on it appends TIME remind-2 to the log")
		if fails: print(out)

		r.sh("rm -f /var/timers/remind-* /var/timers/alarm-* "
		     "/home/flipper/apps_data/alarm/*.off", 20)
		kanpaia(r)

		ordua(r)

		teklatua(r)
		if os.environ.get("NATTEST_LOG"):
			with open(os.environ["NATTEST_LOG"], "wb") as f:
				f.write(r.out)
	finally:
		r.close()
	print("%d failed" % fails)
	return fails


if __name__ == "__main__":
	sys.exit(main())
