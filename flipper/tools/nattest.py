#!/usr/bin/env python3
"""nattest - the native apps remind and alarm (spec 7b.1, 7b.2) in QEMU.

Usage: nattest.py [badusb|jiggle|teklatua]  (only the AC11.3.1, AC11.3.3
       or AC11.3.2 part)

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
  AC11.3.1 badusb (T28c): the package adds the user badusb (group usb),
           Bad USB on the launcher, the example scripts in /ext/badusb;
           badusb on each example script, US and DE, prints exactly the
           hid< lines the engine makes on the PC (tests/hid ducktest -r);
           badusb -n reports a bad line with its number; in the Bad USB
           app (window app, <= 4.5 KB open; badusb <= 3 KB running) the
           list -> confirm -> run window, Back stops a DELAY 10000
           script at once with every key up (the last report all zeros)
           and the rest never typed; Settings writes
           /etc/flipper/badusb.conf once, only on a change
  AC11.3.3 jiggle -i 2 (R11.3.6, T28e) writes its mouse reports every
           2 s +- 0.2 s (QEMU's "hid< mouse" lines), none while the host
           has unplugged us (tests package hidfake: HIDIOCFAKE), again
           once plugged in; the desktop's status line shows J while it
           runs and not after jiggle -k, which ends it; a second jiggle
           is refused; its RAM (free -v, memuse) is at most 1 KB

What needs the RTC alarm (AC7b.2.1, AC7b.R1) is on the device list.
Exit status: the number of failures.
"""
import importlib.util
import os
import re
import struct
import subprocess
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
	# Right after boot (a nattest part alone) the Console may take
	# longer to start than the 3 s here: then wait for it (its RAM
	# taken) before the Back, else the Back is lost and a later OK
	# lands in the Console instead of the app under test.
	before = freebytes(r)
	tap(r, "ok", 3, lit=True)
	for _ in range(5):
		if freebytes(r) < before - 256:
			break
		r.sh("sleep 2", 20)
	tap(r, "back", 3, lit=True)
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
	check(r.sh("pkg install /lib/pkg/usbrem_1.0.tar", 120) == 0,
		"install the usbrem package")
	out = r.output("grep -c '^usbremote:.:24:12:' /etc/passwd; "
		"grep -c '^Teklatua:usbremote:@usb:=/usr/bin/usbremote$' /etc/apps",
		60)
	check(out is not None and out.split()[:2] == [b"1", b"1"],
		"F.4: user usbremote (uid 24, group usb), Teklatua on the launcher")
	if fails: print(out)
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


def clean(out):
	"""Output without the console's other lines: memuse (free -t on) and
	QEMU's pictures of the LCD."""
	return b"\n".join(l for l in (out or b"").split(b"\n")
			  if l and not l.startswith((b"memuse:", b"|", b"+-")))


def lcd_status(r):
	"""The desktop's status line (the first 8 pixel rows of /dev/lcd) as
	a checksum, and the minute it was read in (its clock changes it)."""
	out = clean(r.output("date +%M; dd if=/dev/lcd bs=128 count=1 "
			     "2>/dev/null | cksum", 20))
	m = re.search(rb"(?m)^(\d\d)\n(\d+) 128$", out)
	return (m.group(1), m.group(2)) if m else (None, None)


def report_times(r, pat, n, secs):
	"""Host times of the next n console lines matching pat (bytes regex),
	waiting at most secs in all."""
	t, end = [], time.time() + secs
	while len(t) < n and time.time() < end:
		if r.wait(pat, max(0.1, end - time.time())) is None:
			break
		t.append(time.time())
	return t


def jiggle(r):
	"""AC11.3.3 (R11.3.6, T28e): jiggle in QEMU, run as root from the
	console while the launcher of /etc/rc has the screen."""
	check(r.sh("pkg install /lib/pkg/tests_1.0.tar", 300) == 0,
		"install the tests package (hidfake)")
	r.sh("/usr/tests/hidfake 3; free -t on", 20)	# plugged in, memuse on
	base = freebytes(r)
	for _ in range(3):	# the clock on the status line: one minute
		before = lcd_status(r)
		mark = len(r.out)
		r.sh("jiggle -i 2 & sleep 1", 20)
		r.seen = mark
		first = r.wait(rb"hid< mouse 00 01 00 00", 10)
		on = lcd_status(r)
		if before[:1] == on[:1]:
			break
		r.output("jiggle -k; sleep 1", 20)
	check(first is not None and on[1] is not None and before[1] != on[1],
		"AC11.3.3: the desktop's status line changes when jiggle starts "
		"(J)")
	now = freebytes(r)
	# QEMU runs its text from RAM (the Flipper: from flash, base-flash)
	with open(os.path.join(powercut.MB, "bin", "jiggle"), "rb") as f:
		text = (struct.unpack_from("<I", f.read(12), 8)[0] + 255) & ~255
	print("  RAM: jiggle running: %d bytes, %d without its text (free "
	      "%d -> %d)" % (base - now, base - now - text, base, now),
	      flush=True)
	check(0 < base - now - text <= 1024,
		"R11.3.6: jiggle takes at most 1 KB of RAM besides its text "
		"(%d B)" % (base - now - text))
	out = clean(r.output("jiggle -i 5; echo st=$?", 20))
	check(out is not None and b"already running" in out
		and b"st=1" in out, "jiggle: a second one is refused")
	# the square: right, down, left, up, then nothing for 2 s
	r.seen = len(r.out)
	t = report_times(r, rb"hid< mouse 00 01 00 00\r?\n.*?"
		rb"hid< mouse 00 00 01 00\r?\n.*?hid< mouse 00 ff 00 00\r?\n"
		rb".*?hid< mouse 00 00 ff 00", 4, 12)
	d = [b - a for a, b in zip(t, t[1:])]
	print("  jiggle -i 2: report intervals %s s" % " ".join("%.2f" % x
		for x in d), flush=True)
	check(len(d) == 3 and all(abs(x - 2) <= 0.2 for x in d),
		"AC11.3.3: jiggle -i 2: a mouse report (the 1-px square) every "
		"2 s +- 0.2 s")
	# unplugged: no report for 6 s; plugged in again: they come back
	r.sh("/usr/tests/hidfake 0", 20)
	mark = len(r.out)
	r.sh("sleep 6", 20)
	quiet = b"hid< mouse" not in r.out[mark:]
	r.sh("/usr/tests/hidfake 3", 20)
	r.seen = len(r.out)
	back = r.wait(rb"hid< mouse 00 01 00 00", 6)
	check(quiet and back is not None, "AC11.3.3: no report while "
		"unplugged (hidfake 0), reports again once plugged in")
	r.sh("/usr/tests/hidfake 7", 20)		# suspended
	mark = len(r.out)
	r.sh("sleep 5", 20)
	quiet = b"hid< mouse" not in r.out[mark:]
	r.sh("/usr/tests/hidfake 3", 20)
	check(quiet, "jiggle: no report while the host has suspended the bus")
	out = clean(r.output("jiggle -k; echo st=$?; sleep 2; jiggle -k; "
			     "echo st=$?; ls /tmp/jiggle", 20))
	off = lcd_status(r)
	check(out.startswith(b"st=0\n") and b"not running" in out
		and b"st=1" in out and b"No such file" in out,
		"AC11.3.3: jiggle -k ends it (then: not running, its FIFO gone)")
	if fails: print(out)
	check(off[1] is not None and (off[1] == before[1]
				      or off[0] != before[0]),
		"AC11.3.3: J is gone from the status line after jiggle -k")
	for m in re.finditer(rb"memuse: jiggle .*", r.out):
		print("  " + m.group(0).decode().strip())
	mark = len(r.out)
	r.output("jiggle -i 1 -p zero & sleep 3; jiggle -k", 20)
	check(re.search(rb"hid< mouse 00 00 00 00", r.out[mark:]) is not None
		and b"hid< mouse 00 01" not in r.out[mark:],
		"jiggle -p zero: reports that do not move the pointer")
	out = clean(r.output("jiggle -i 0; echo st=$?; jiggle -p big; "
			     "echo st=$?", 20))
	check(out.count(b"st=2") == 2,
		"jiggle: a bad interval or pattern is a usage error")
	r.sh("free -t off", 20)

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
HIDRE = re.compile(rb"hid< ((?:kbd|mouse|cons)(?: [0-9a-f]{2})+)")
BU_EX = ("windows", "macos", "linux", "keys")


def ducktest():
	"""tests/hid's ducktest (the engine on the PC), built if needed."""
	d = os.path.join(ROOT, "flipper", "tests", "hid")
	subprocess.check_call(["make", "-s", "-C", d, "build/ducktest"],
			      stdout=subprocess.DEVNULL)
	return os.path.join(d, "build", "ducktest")


def want_reports(dt, layout, name):
	kl = "us" if layout == "en-US" else os.path.join(ROOT, "src", "lib",
		"hid", "layouts", layout + ".kl")
	ex = os.path.join(ROOT, "flipper", "apps", "badusb", "examples",
			  name + ".txt")
	out = subprocess.run([dt, "-r", kl, ex], stdout=subprocess.PIPE,
			     check=True).stdout
	return out.splitlines()


def badusb(r):
	"""AC11.3.1: badusb and the Bad USB app (spec R11.3.3, R11.3.4)."""
	check(r.sh("pkg install /lib/pkg/badusb_1.0.tar", 120) == 0,
		"install the badusb package")
	out = r.output("grep -c '^badusb:.:23:12:' /etc/passwd; "
		"grep -c '^Bad USB:badusb:@usb:=/usr/bin/badusb-gui$' /etc/apps",
		60)
	out2 = r.output("ls /home/flipper/badusb; "
		"ls -l /etc/flipper/badusb.conf /usr/share/hid/layouts/de-DE.kl",
		60)
	check(out is not None and out.split()[:2] == [b"1", b"1"]
	      and out2 is not None
	      and all(("%s.txt" % e).encode() in out2 for e in BU_EX)
	      and re.search(rb"-rw-r--r-- .*badusb .* 0 .*badusb.conf", out2)
	      is not None and b"de-DE.kl" in out2,
	      "AC11.3.1: user badusb (group usb), Bad USB on the launcher, the "
	      "examples in /ext/badusb, an empty badusb.conf of badusb's, DE")
	if fails: print(out, out2)
	dt = ducktest()
	for layout in ("en-US", "de-DE"):
		for e in BU_EX:
			want = want_reports(dt, layout, e)
			out = r.output("echo | badusb -l %s /home/flipper/badusb/"
				"%s.txt; echo st=$?" % (layout, e), 120)
			got = HIDRE.findall(out or b"")
			ok = out is not None and b"st=0" in out and got == want
			check(ok, "AC11.3.1: badusb -l %s %s.txt: the %d hid< "
			      "reports the engine makes on the PC" % (layout, e,
			      len(want)))
			if not ok:
				print("    got %d, want %d" % (len(got), len(want)))
				for i, (g, w) in enumerate(zip(got, want)):
					if g != w:
						print("    first difference at %d: "
						      "%s / %s" % (i, g, w))
						break
				if out is not None and len(got) == 0:
					print(out.decode("latin-1")[-600:])
	r.sh("echo 'STRING ok' > /tmp/bad.txt; echo 'REM' >> /tmp/bad.txt; "
	     "echo 'FOO bar' >> /tmp/bad.txt", 20)
	out = r.output("badusb -n /tmp/bad.txt; echo st=$?", 30)
	check(out is not None and b"st=1" in out and re.search(
	      rb"/tmp/bad.txt: line 3: unknown command or key: FOO", out)
	      is not None and HIDRE.search(out) is None,
	      "AC11.3.1: badusb -n reports the bad line with its number and "
	      "types nothing")
	if fails: print(out)
	out = r.output("badusb -n /home/flipper/badusb/keys.txt; echo st=$?", 30)
	check(out is not None and out.strip() == b"st=0",
	      "badusb -n: an example script checks clean")

	# The app: a script that would wait 10 s, last in the list
	r.sh("echo 'STRING a' > /home/flipper/badusb/zwait.txt; "
	     "echo 'DELAY 10000' >> /home/flipper/badusb/zwait.txt; "
	     "echo 'STRING b' >> /home/flipper/badusb/zwait.txt", 20)
	base = win_only(r, "badusb-gui")
	mark = len(r.out)
	win_tap(r, "ok", rb"gui< win\tlist\t.*?gui< show")	# Bad USB
	now = freebytes(r)
	print("  RAM: Bad USB open: %d bytes (free %d -> %d)" % (base - now,
		base, now), flush=True)
	log = lambda: r.out[mark:].replace(b"\r", b"")
	check(re.search(rb"gui< win\tlist\tlist\tBad USB", log()) is not None
	      and b"gui< item\tset\tSettings\ten-US" in log()
	      and all(("\t%s\n" % e).encode() in log() for e in BU_EX)
	      and b"gui< item\ts4\tzwait" in log(),
	      "AC11.3.1: the launcher starts Bad USB as a window app: "
	      "Settings, then the scripts")
	if fails:
		print(b"\n".join(re.findall(rb"gui[<>] [^\n]*",
		      log())[:20]).decode("latin-1"))
	check(0 < base - now <= 4608,
	      "R11.3.4: Bad USB open takes <= 4.5 KB more than the launcher "
	      "alone (%d B)" % (base - now))
	# Settings: Layout -> de-DE, Back: one write of badusb.conf
	keys(r, ("ok@set", "right", "back"))
	r.sh("sleep 1", 20)
	out = file_out(r, "cat /etc/flipper/badusb.conf",
		       lambda o: b"Layout: de-DE" in o)
	check(out is not None and b"Layout: de-DE\nDelay: 0\n" in out
	      and re.search(rb"gui> submit\tset\tlay=1", log()) is not None,
	      "R11.3.4: Settings: the layout changed, badusb.conf written")
	if fails: print(out)
	r.sh("ls -l /etc/flipper/badusb.conf > /tmp/conf1; sleep 60", 90)
	keys(r, ("ok@set", "back"))		# no change: no write
	r.sh("sleep 1", 20)
	out = r.output("ls -l /etc/flipper/badusb.conf | cmp -s - /tmp/conf1 "
		       "&& echo same", 20)
	check(out is not None and b"same" in out,
	      "K.7: Settings left without a change writes nothing")
	# Up (wraps): zwait; OK: the confirm window; OK: run; Back: stop
	keys(r, ("up", "ok@conf"))
	check(b"gui< win\tconf\tconfirm\tBad USB" in log()
	      and b"gui< line\tzwait" in log()
	      and b"gui< line\tLayout: de-DE" in log()
	      and b"gui< line\tConnect the USB cable" in log(),
	      "R11.3.4: the confirm window (script, layout, connect and run?)")
	m2 = len(r.out)
	win_tap(r, "ok", rb"hid< kbd 00 00 04")		# 'a' typed
	busy = freebytes(r)
	print("  RAM: badusb running: %d bytes (free %d -> %d)" % (now - busy,
		now, busy), flush=True)
	check(0 < now - busy <= 3072,
	      "R11.3.3: badusb running takes <= 3 KB (%d B)" % (now - busy))
	t0 = time.time()
	win_tap(r, "back", rb"gui< win\tlist\t.*?gui< show")
	dt_back = time.time() - t0
	after = r.out[m2:].replace(b"\r", b"")
	reps = HIDRE.findall(after)
	backat = after.find(b"gui> back\trun")
	check(re.search(rb"gui< win\trun\ttext\tBad USB", after) is not None
	      and backat >= 0 and len(reps) >= 3
	      and reps[-1] == b"kbd 00 00 00 00 00 00 00 00"
	      and b"kbd 00 00 05" not in b" ".join(reps)
	      and HIDRE.search(after, backat) is not None,
	      "AC11.3.1: Back stops the DELAY 10000 script: every key up "
	      "(the last report all zeros), 'b' never typed, the list again "
	      "(%.1f s in QEMU with the fake key)" % dt_back)
	if fails: print(after.decode("latin-1")[-2500:])
	keys(r, ("back!",))
	end = ram_back(r, base)
	check(end >= base - 256, "Bad USB: Back closes it, its RAM is free "
	      "again (%d -> %d)" % (base, end))
	for m in re.finditer(rb"memuse: badusb.*", r.out[mark:]):
		print("  " + m.group(0).decode().strip())
	r.sh("rm -f /home/flipper/badusb/zwait.txt", 20)
	win_restore(r)


PARTS = {"badusb": badusb, "jiggle": jiggle, "teklatua": teklatua}


def main():
	if len(sys.argv) > 2 or sys.argv[1:] and sys.argv[1] not in PARTS:
		print(__doc__.split("\n\n")[1], file=sys.stderr)
		return 2
	r = powercut.Run(0, 0)
	try:
		if not r.login():
			check(False, "login")
			return 1
		part = sys.argv[1] if len(sys.argv) > 1 else None
		if part is not None:
			if part != "jiggle":
				keyfiles(r)
			PARTS[part](r)
			if os.environ.get("NATTEST_LOG"):
				with open(os.environ["NATTEST_LOG"], "wb") as f:
					f.write(r.out)
			print("%d failed" % fails)
			return fails
		for p in ("timed_1.2", "remind_1.1", "alarm_1.1"):
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
		jiggle(r)
		badusb(r)
		if os.environ.get("NATTEST_LOG"):
			with open(os.environ["NATTEST_LOG"], "wb") as f:
				f.write(r.out)
	finally:
		r.close()
	print("%d failed" % fails)
	return fails


if __name__ == "__main__":
	sys.exit(main())
