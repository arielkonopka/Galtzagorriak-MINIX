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
  GUI      alarm takes the foreground, becomes its user, makes an alarm
           through its form with fake keys (and opens the desk clock) and
           exits 0; free -t shows what it used; free -v before and with
           each of alarm and alert open gives the RAM each one takes
           (printed as "RAM:" lines, issue #201)

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


def tap(r, key, wait=1):
	"""A short press of a fake button (QEMU /dev/buttons): down, held
	for one cat (more than a clock tick, less than a long press), up."""
	r.sh("cat /tmp/k%s >/dev/buttons; cat /etc/motd >/dev/null; "
	     "cat /tmp/k000 >/dev/buttons; sleep %d" % (KEY[key], wait), 20)


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


def hm(t):
	return time.strftime("%H:%M", time.gmtime(t))


def kanpaia(r):
	"""ACK8.1, ACK8.3: Kanpaia through the launcher of /etc/rc.  It read
	/etc/apps at boot; it reads it again when an app it ran ends, so an
	/etc/apps of Kanpaia alone puts it under the cursor.  GuiLog: on
	puts the window records (gui<) and events (gui>) on the console."""
	r.sh("cp /etc/apps /tmp/apps.bak; cp /etc/launcher.conf /tmp/lc.bak; "
	     "grep ':=/usr/bin/remind$' /tmp/apps.bak > /etc/apps; "
	     "echo 'GuiLog: on' >> /etc/launcher.conf", 20)
	tap(r, "back")		# lights the screen; Back on a page: the list
	tap(r, "back")
	tap(r, "ok", 3)		# whichever app the cursor is on
	tap(r, "back", 3)	# it ends: the launcher reads both files again
	r.sh("free -t on", 20)
	base = freebytes(r)
	mark = len(r.out)
	tap(r, "ok", 3)		# Kanpaia: its main window
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
	for k in ("up", "ok", "ok", "ok", "up", "right", "right", "right",
		  "ok", "up", "ok"):
		tap(r, k)
	r.sh("sleep 2", 20)
	out = r.output("cat /var/timers/remind-1; ls -l /var/timers/remind-1",
		20)
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
	for k in ("down", "ok", "ok"):
		tap(r, k)
	check(re.search(rb"gui< win\tnext\tlist\tUpcoming", r.out[mark:])
		is not None and re.search(rb"gui< item\tremind-1\tDrink a glass "
		rb"of watera\t", r.out[mark:]) is not None,
		"Kanpaia: Upcoming lists remind-1 with its next time")
	check(re.search(rb"gui< win\tedit\tlist\tReminder", r.out[mark:])
		is not None and re.search(rb"gui< action\tdel\tDelete",
		r.out[mark:]) is not None,
		"Kanpaia: OK on it opens its form, with Delete")
	if fails: print(r.out[mark:].decode(errors="replace"))
	for k in ("back", "back", "back"):
		tap(r, k)
	r.sh("sleep 3", 20)
	check(abs(freebytes(r) - base) < 256,
		"Kanpaia: Back closes it, its RAM is free again")
	for m in re.finditer(rb"memuse: remind .*", r.out):
		print("  " + m.group(0).decode().strip())
	r.output("free -t off; cp /tmp/apps.bak /etc/apps; "
		 "cp /tmp/lc.bak /etc/launcher.conf", 20)


def main():
	r = powercut.Run(0, 0)
	try:
		if not r.login():
			check(False, "login")
			return 1
		for p in ("timed_1.2", "remind_1.1", "alarm_1.0"):
			check(r.sh("pkg install /lib/pkg/%s.tar" % p, 120) == 0,
				"install the %s package" % p)
		out = r.output("grep -c '^remind:.:21:11:' /etc/passwd; "
			"grep -c '^alarm:.:22:11:' /etc/passwd; "
			"grep -c ':remind:@bell:=/usr/bin/remind$' /etc/apps; "
			"grep -c ':root:@alarm:/usr/bin/alarm$' /etc/apps", 20)
		check(out is not None and out.split() == [b"1"] * 4,
			"F.4: users remind and alarm (group timers), launcher lines "
			"(Kanpaia a window app as remind)")
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
		r.sh("/usr/lib/alert -c reminder -f 0 -u 21 -l remind remind-1 "
		     "Reminder Water 2>/dev/null & sleep 2", 20)
		ram(r, "alert", base)
		tap(r, "left")		# the first press only lights the screen
		tap(r, "ok", 2)
		out = r.output("cat /home/flipper/apps_data/remind/log", 20)
		check(out is not None and re.match(rb"\d+ remind-1\n$", out)
			is not None, "7b.1: OK on the alert appends TIME remind-1")
		out = r.output("ls -l /home/flipper/apps_data/remind/log", 20)
		check(out is not None and re.search(rb"-rw------- .*remind ", out)
			is not None, "7b.1: the log is written as the user remind")
		r.sh("/usr/lib/alert -c reminder -f 0 -u 21 -l remind remind-1 "
		     "Reminder Water 2>/dev/null & sleep 2", 20)
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
		r.sh("/usr/lib/alert -c alarm -f 16 -t rise -r 15 -R -S 20 -1 "
		     "-v 40 -z 5 alarm-9 -u 22 Wake Wake 2>/dev/null & sleep 2", 20)
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
		check(re.search(rb"alert: \d\d:\d\d alarm: Now: now", r.out[mark:])
			is not None, "7b.2: timed fires the alarm (Tone: pips)")
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
		check(re.search(rb"alert: \d\d:\d\d reminder: Reminder: Stretch",
			r.out[mark:]) is not None, "7b.1: timed fires the reminder")
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

		# alarm: it takes the foreground, becomes its user, makes a
		# timer through the form and exits 0.
		mark = len(r.out)
		r.sh("free -t on", 20)
		# alarm: OK (desk clock), Back, Down (+ New), OK, Down x7
		# (Volume), Left (90 %), Up x8 (Save), OK, Back
		for app, keys in (("alarm", ("ok", "back", "down", "ok")
					    + ("down",) * 7 + ("left",)
					    + ("up",) * 8 + ("ok", "back")),):
			r.sh("rm -f /tmp/st; (/usr/bin/%s; echo $? > /tmp/st) & "
			     "sleep 4" % app, 30)
			tap(r, "left")
			for k in keys:
				tap(r, k)
			r.sh("sleep 2", 20)
			out = r.output("cat /tmp/st", 20)
			# (free -t prints its memuse lines in between)
			out = out and re.sub(rb"(?m)^memuse: .*\n", b"", out)
			check(out == b"0\n", "GUI: %s takes the keys and exits 0"
				% app)
		r.output("sleep 1; free -t off", 30)
		out = r.output("cat /var/timers/alarm-1; /usr/bin/alarm -n", 20)
		check(out is not None and b"Kind: daily" in out
			and b"Time: 07:00" in out and b"Category: alarm" in out
			and b"Out: sound,vibro,led,screen" in out
			and b"Mode: escalating" in out and b"Volume: 90" in out
			and re.search(rb"alarm-1 \d+ 07:00 Alarm\n", out) is not None,
			"GUI: alarm's form made alarm-1 (07:00 Mo-Fr, escalating, "
			"every output, Volume 90), alarm -n")
		if fails: print(out)
		for m in re.finditer(rb"memuse: (alarm|alert) .*",
				     r.out[mark:]):
			print("  " + m.group(0).decode().strip())
		# The RAM each app takes while open (issue #201): started
		# straight from this shell, so no subshell is counted, and
		# killed after the count (the last thing this test does).
		for app in ("alarm",):
			base = freebytes(r)
			r.sh("/usr/bin/%s & sleep 4" % app, 30)
			ram(r, app, base)
			r.sh("kill $!; sleep 2", 30)
		if os.environ.get("NATTEST_LOG"):
			with open(os.environ["NATTEST_LOG"], "wb") as f:
				f.write(r.out)
	finally:
		r.close()
	print("%d failed" % fails)
	return fails


if __name__ == "__main__":
	sys.exit(main())
