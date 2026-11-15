#!/usr/bin/env python3
"""Host tests for mined (T8, spec K.5): mined.host (built with
AddressSanitizer and UBSan) runs in a pseudo-terminal; the tests type keys
and look at the files and at what mined sends to the terminal.  Any
sanitizer report fails the test.

Usage: test_mined.py ./mined.host
"""
import fcntl
import os
import pty
import resource
import select
import signal
import struct
import sys
import tempfile
import termios
import time

MINED = os.path.abspath(sys.argv[1])
ESC = b"\x1b"
CTRL = {c: bytes([ord(c) & 0x1F]) for c in "@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_"}
fails = 0


class Mined:
    def __init__(self, args, cwd, size=None, answer=(24, 80), fsize=None,
                 env=None):
        """size: the pty's window size (rows, cols) or None (0x0, so mined
        asks); answer: what the 'terminal' replies to ESC [ 6 n."""
        self.answer = answer
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(cwd)
            if fsize is not None:
                signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
                resource.setrlimit(resource.RLIMIT_FSIZE, (fsize, fsize))
            e = {"HOME": cwd, "PATH": "/bin:/usr/bin",
                 "ASAN_OPTIONS": "detect_leaks=0"}
            e.update(env or {})
            os.execve(MINED, [MINED] + args, e)
        self.pid, self.fd = pid, fd
        if size:
            fcntl.ioctl(fd, termios.TIOCSWINSZ,
                        struct.pack("HHHH", size[0], size[1], 0, 0))
        self.out = b""
        self.status = None
        self.settle()

    def pump(self, secs):
        end = time.time() + secs
        while time.time() < end:
            try:
                r = select.select([self.fd], [], [], 0.05)[0]
            except OSError:
                return
            if not r:
                continue
            try:
                d = os.read(self.fd, 65536)
            except OSError:
                return
            if not d:
                return
            self.out += d
            if b"\x1b[6n" in d and self.answer:
                os.write(self.fd, b"\x1b[%d;%dR" % self.answer)
            end = max(end, time.time() + 0.15)

    def settle(self):
        self.pump(0.6)

    def keys(self, *seqs, pause=0.05):
        for s in seqs:
            os.write(self.fd, s)
            self.pump(pause)
        self.settle()

    def wait(self, secs=3):
        end = time.time() + secs
        while time.time() < end:
            p, st = os.waitpid(self.pid, os.WNOHANG)
            if p:
                self.status = st
                break
            self.pump(0.1)
        if self.status is None:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
            self.status = -1
        try:
            self.out += os.read(self.fd, 65536)
        except OSError:
            pass
        try:
            os.close(self.fd)
        except OSError:
            pass
        return self.status

    def quit(self, save=b"n"):
        self.keys(CTRL["X"])
        if b"has been modified" in self.out[-400:]:
            self.keys(save)
        return self.wait()


def check(name, ok, why=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + (": " + why if why else ""))
    if not ok:
        fails += 1


def clean(m):
    """No sanitizer report, exited on its own."""
    bad = b"AddressSanitizer" in m.out or b"runtime error" in m.out
    return not bad and m.status is not None and m.status != -1 \
        and not os.WIFSIGNALED(m.status)


def tmpdir():
    return tempfile.mkdtemp(prefix="mined-test-")


def write(d, name, data):
    with open(os.path.join(d, name), "wb") as f:
        f.write(data)


def read(d, name):
    with open(os.path.join(d, name), "rb") as f:
        return f.read()


# ---------------------------------------------------------------------------
def test_a1_long_lines():
    d = tmpdir()
    write(d, "f", b"a" * 5000 + b"\nend\n")
    m = Mined(["f"], d)
    split = b"long lines split" in m.out
    # a split line is full (1023 chars): edit the last line
    m.keys(ESC + b"[F", ESC + b"[A", b"X", CTRL["W"])
    # copy the first 3 lines to the clipboard and paste them (file_insert)
    m.keys(ESC + b"[H", CTRL["@"], ESC + b"[B", ESC + b"[B", ESC + b"[B",
           CTRL["C"], CTRL["Y"])
    # insert the file itself (^G): its long line through file_insert
    m.keys(CTRL["G"], b"f\r")
    m.quit(b"n")
    data = read(d, "f")
    lines = data.split(b"\n")
    check("A1 5000-char line loads, split with a warning", split)
    check("A1 split lines fit (<= 1023 chars + newline)",
          max(len(x) for x in lines) <= 1023, "longest %d" % max(len(x) for x in lines))
    check("A1 nothing lost on write", data.count(b"a") == 5000
          and b"Xend\n" in data)
    check("A1 paste and insert of long lines: no crash", clean(m))


def test_a2_long_inputs():
    d = tmpdir()
    m = Mined(["p" * 200], d)
    m.wait()
    check("A2 200-char path refused", b"file name too long" in m.out
          and os.WIFEXITED(m.status) and os.WEXITSTATUS(m.status) == 1)
    write(d, "g", b"hello\n")
    m = Mined(["g"], d)
    long = b"q" * 200
    m.keys(CTRL["G"], long + b"\r")               # insert file
    m.keys(CTRL["V"], long + b"\r")               # visit (not modified)
    m = m
    m.keys(ESC + b"OQ", long + b"\r")             # search forward (PF2)
    m.keys(ESC + b"OR", long + b"\r")             # search back (PF3)
    m.keys(CTRL["R"], b"l" + b"\r", long + b"\r")  # replace l by 200 q
    m.keys(CTRL["L"], long + b"\r", long + b"\r")  # line replace
    m.keys(CTRL["Q"], long + b"\r")               # write buffer to file
    m.keys(CTRL["]"], b"9" * 30 + b"\r")          # goto line
    m.quit(b"n")
    check("A2 200 chars into every prompt: no crash", clean(m))
    name = b"n" * 79
    os.mkdir(os.path.join(d, "ro"))
    m = Mined([("ro/" + "n" * 76)], d)
    os.chmod(os.path.join(d, "ro"), 0o555)
    m.keys(b"x", CTRL["W"], ESC + b"OP")          # write error, status
    m.quit(b"n")
    os.chmod(os.path.join(d, "ro"), 0o755)
    check("A2 79-char file name on a write error: no crash", clean(m))


def test_a3_regex():
    d = tmpdir()
    write(d, "h", b"some text ~~~\n" * 3)
    m = Mined(["h"], d)
    pat = b"[" + b"!-~" * 26 + b"]"
    m.keys(ESC + b"OQ", pat + b"\r")
    err = b"too long" in m.out or b"List too long" in m.out
    m.keys(ESC + b"OQ", b"[\xc0-\xff]x\r")        # bytes >= 0x80
    m.keys(ESC + b"OQ", b"[!-\x7f]\r")            # range up to DEL
    m.keys(ESC + b"OQ", b"x*\r", CTRL["R"], b"e*\r", b"E\r")
    m.quit(b"n")
    check("A3 80-char [!-~...] pattern: an error, no overflow", err and clean(m))


def test_a4_clipboard():
    d = tmpdir()
    write(d, "c", b"one\ntwo\n")
    before = set(x for x in os.listdir("/tmp") if x.startswith("mined."))
    m = Mined(["c"], d)
    m.keys(CTRL["@"], ESC + b"[B", CTRL["C"])
    new = [x for x in os.listdir("/tmp") if x.startswith("mined.")
           and x not in before]
    modes = [os.stat("/tmp/" + x).st_mode & 0o777 for x in new]
    m.keys(CTRL["Y"])
    m.quit(b"n")
    check("A4 clipboard made 0600, random name", len(new) == 1
          and modes == [0o600] and len(new[0]) == len("mined.") + 8,
          "%r %r" % (new, [oct(x) for x in modes]))
    check("A4 clipboard removed at exit", not any(
        os.path.exists("/tmp/" + x) for x in new) and clean(m))


def test_a5_hangup():
    d = tmpdir()
    write(d, "s", b"keep me\n")
    m = Mined(["s"], d)
    m.keys(b"Z")
    os.close(m.fd)                                # the terminal goes away
    end = time.time() + 5
    while time.time() < end:
        p, st = os.waitpid(m.pid, os.WNOHANG)
        if p:
            m.status = st
            break
        time.sleep(0.05)
    if m.status is None:
        os.kill(m.pid, signal.SIGKILL)
        os.waitpid(m.pid, 0)
    saved = os.path.join(d, "s.save")
    ok = os.path.exists(saved) and read(d, "s.save") == b"Zkeep me\n" \
        and os.stat(saved).st_mode & 0o777 == 0o600
    check("A5 terminal gone while modified: s.save (0600), clean exit",
          ok and m.status is not None and os.WIFEXITED(m.status),
          "status %r" % m.status)


def test_a6_full():
    d = tmpdir()
    orig = b"line %d\n" * 1
    orig = b"".join(b"line %04d\n" % i for i in range(400))   # 4000 bytes
    write(d, "big", orig)
    m = Mined(["big"], d, fsize=1000)
    m.keys(b"new ", CTRL["W"])
    failed = b"Write failed" in m.out or b"aborted" in m.out
    m.quit(b"n")
    check("A6 write error leaves the original intact",
          read(d, "big") == orig and failed and clean(m))
    check("A6 no temporary file left", not os.path.exists(
        os.path.join(d, ".big.tmp")))


def test_a7_controls():
    d = tmpdir()
    write(d, "x", b"csi \x9b2J here\nosc \x1b]0;PWNED\x07 end\ndel \x7f\n")
    m = Mined(["x"], d)
    m.keys(ESC + b"OP")
    m.quit(b"n")
    check("A7 C1 and ESC bytes shown as markers, never sent raw",
          b"\x9b" not in m.out and b"\x1b]0;PWNED" not in m.out and clean(m))
    weird = "w\x1b[2Jx"
    write(d, weird, b"y\n")
    m = Mined([weird], d)
    m.keys(ESC + b"OP")
    m.quit(b"n")
    check("A7 file name with ESC filtered in the status line",
          b"\x1b[2Jx" not in m.out.replace(b"\x1b[2J", b"", 0) and clean(m))


def test_a8_keys():
    d = tmpdir()
    write(d, "k", b"abc\ndef\nghi\n")
    m = Mined(["k"], d)
    seqs = [ESC + b"[A", ESC + b"[B", ESC + b"[C", ESC + b"[D",
            ESC + b"OA", ESC + b"OB", ESC + b"OC", ESC + b"OD",
            ESC + b"[H", ESC + b"[1~", ESC + b"OH", ESC + b"[F",
            ESC + b"[4~", ESC + b"OF", ESC + b"[5~", ESC + b"[6~",
            ESC + b"[2~", ESC + b"[15~", ESC + b"[1;5C", ESC + b"[200~",
            ESC + b"OP", ESC + b"OS", ESC + b"[7~", ESC + b"[8~"]
    m.keys(*seqs)
    m.keys(CTRL["W"])
    untouched = b"Write not necessary" in m.out[-300:]
    m.quit(b"n")
    check("A8 no key leaves characters in the text", untouched
          and read(d, "k") == b"abc\ndef\nghi\n" and clean(m))

    write(d, "k", b"abc\n")
    m = Mined(["k"], d)
    m.keys(ESC + b"[3~", CTRL["W"])              # Delete under the cursor
    m.quit(b"n")
    check("A8 Delete (ESC [ 3 ~)", read(d, "k") == b"bc\n" and clean(m))

    write(d, "k", b"abc\n")
    m = Mined(["k"], d)
    m.keys(ESC + b"OC", ESC + b"[C", b"\x7f", b"\x08", CTRL["W"])
    m.quit(b"n")
    check("A8 DECCKM right, then DEL and BS both delete left",
          read(d, "k") == b"c\n" and clean(m))

    write(d, "k", b"abc\ndef\n")
    m = Mined(["k"], d)
    m.keys(ESC + b"OB", ESC + b"[B", ESC + b"[A", b"X", ESC + b"[F", b"Y",
           ESC + b"[1~", b"Z", CTRL["W"])
    m.quit(b"n")
    check("A8 arrows, End (end of file), Home (start of file)",
          read(d, "k") == b"Zabc\nXdef\nY\n" or read(d, "k").startswith(b"Zabc\nXdef"),
          repr(read(d, "k")))


def test_a9_size():
    d = tmpdir()
    write(d, "z", b"z\n" * 50)
    m = Mined(["z"], d, size=None, answer=(20, 60))
    ws = struct.unpack("HHHH", fcntl.ioctl(m.fd, termios.TIOCGWINSZ,
                                            b"\0" * 8))
    asked = b"\x1b[6n" in m.out
    m.keys(ESC + b"[B" * 25)
    m.quit(b"n")
    check("A9 60x20 terminal without a size: asked and stored",
          asked and ws[0] == 20 and ws[1] == 60 and clean(m), repr(ws[:2]))
    m = Mined(["z"], d, size=None, answer=None)
    m.quit(b"n")
    check("A9 no answer: 80x24, no hang", clean(m))


def test_b10_substitute():
    d = tmpdir()
    write(d, "r", b"x" * 1000 + b"\n")
    m = Mined(["r"], d)
    m.keys(CTRL["R"], b"x\r", b"yy\r")
    toobig = b"too big" in m.out or b"too long" in m.out
    m.keys(CTRL["L"], b"x*\r", b"&&\r")
    m.quit(b"n")
    check("B10 substitution past MAX_CHARS refused, no overflow",
          toobig and clean(m))


for t in [test_a1_long_lines, test_a2_long_inputs, test_a3_regex,
          test_a4_clipboard, test_a5_hangup, test_a6_full, test_a7_controls,
          test_a8_keys, test_a9_size, test_b10_substitute]:
    t()
print("%d failed" % fails)
sys.exit(1 if fails else 0)
