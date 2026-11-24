#!/usr/bin/env python3
"""rpc - a small client for the stock Flipper RPC (issue #388, spec R15.5,
R15.7), speaking the same frames as the mobile app and qFlipper: a varint
length, then a PB.Main message (src/lib/flipperpb/proto).  The encoder is
written out here (no protobuf package needed), only for the requests below.

    rpc.py [--exec CMD | --port DEV] COMMAND [ARGS]

    ping [DATA]        system_ping, prints the echo
    version            system_protobuf_version
    info               system_device_info, key: value per line
    power              system_power_info
    time               system_get_datetime
    settime [Y M D h m s]   system_set_datetime (default: this PC's local time)
    ls PATH            storage_list
    stat PATH          storage_stat
    df PATH            storage_info
    cat PATH           storage_read, the file to stdout
    raw TAG            an empty request with PB.Main field TAG (tests:
                       anything rpcd does not know gets NOT_IMPLEMENTED)

--exec runs CMD and talks over its stdin/stdout (the host rpcd of
flipper/tests/rpcd); --port opens a serial device (the Flipper's RPC
USB CDC; BLE later through a bridge).  Exit status 1 when the answer's
command_status is not OK.  Stock's CLI on the console needs `start_rpc_session`
first; MINIX's RPC transports are RPC only.
"""
import argparse
import os
import subprocess
import sys
import time

STATUS = {0: "OK", 1: "ERROR", 2: "ERROR_DECODE", 3: "ERROR_NOT_IMPLEMENTED",
          4: "ERROR_BUSY", 5: "ERROR_STORAGE_NOT_READY",
          6: "ERROR_STORAGE_EXIST", 7: "ERROR_STORAGE_NOT_EXIST",
          8: "ERROR_STORAGE_INVALID_PARAMETER", 9: "ERROR_STORAGE_DENIED",
          10: "ERROR_STORAGE_INVALID_NAME", 11: "ERROR_STORAGE_INTERNAL",
          15: "ERROR_INVALID_PARAMETERS"}
# PB.Main fields (flipper.proto)
PING_REQ, PING_RESP = 5, 6
LIST_REQ, LIST_RESP, READ_REQ, READ_RESP = 7, 8, 9, 10
STAT_REQ, STAT_RESP, INFO_REQ, INFO_RESP = 24, 25, 28, 29
DEVINFO_REQ, DEVINFO_RESP = 32, 33
GETDT_REQ, GETDT_RESP, SETDT_REQ = 35, 36, 37
PBVER_REQ, PBVER_RESP = 39, 40
POWER_REQ, POWER_RESP = 44, 45


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def field(num, val):
    if isinstance(val, int):
        return varint(num << 3) + varint(val)
    if isinstance(val, str):
        val = val.encode()
    return varint(num << 3 | 2) + varint(len(val)) + val


def decode(b):
    """{field: [values]}: ints for varints, bytes for the rest."""
    out, i = {}, 0
    while i < len(b):
        k, i = rvarint(b, i)
        num, wt = k >> 3, k & 7
        if wt == 0:
            v, i = rvarint(b, i)
        elif wt == 2:
            n, i = rvarint(b, i)
            v, i = b[i:i + n], i + n
        elif wt == 5:
            v, i = int.from_bytes(b[i:i + 4], "little"), i + 4
        elif wt == 1:
            v, i = int.from_bytes(b[i:i + 8], "little"), i + 8
        else:
            raise ValueError("wire type %d" % wt)
        out.setdefault(num, []).append(v)
    return out


def rvarint(b, i):
    n = s = 0
    while True:
        c = b[i]
        i += 1
        n |= (c & 0x7F) << s
        s += 7
        if not c & 0x80:
            return n, i


def one(d, k, default=0):
    return d.get(k, [default])[-1]


class Link:
    def __init__(self, args):
        self.p = None
        if args.exec:
            self.p = subprocess.Popen(args.exec, shell=True,
                                      stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE)
            self.wfd, self.rfd = self.p.stdin.fileno(), self.p.stdout.fileno()
        else:
            fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY)
            try:
                import termios
                import tty
                tty.setraw(fd)
                termios.tcflush(fd, termios.TCIOFLUSH)
            except (ImportError, OSError):
                pass
            self.wfd = self.rfd = fd
        self.buf = b""
        self.id = 0

    def request(self, tag, body=b""):
        """Send one request; yield the answers until has_next is 0."""
        self.id += 1
        m = field(1, self.id) + field(tag, body)
        os.write(self.wfd, varint(len(m)) + m)
        while True:
            a = self.frame()
            yield a
            if not one(a, 3):
                return

    def frame(self):
        while True:
            try:
                n, i = rvarint(self.buf, 0)
                if i + n <= len(self.buf):
                    f, self.buf = self.buf[i:i + n], self.buf[i + n:]
                    return decode(f)
            except IndexError:
                pass
            c = os.read(self.rfd, 4096)
            if not c:
                raise SystemExit("rpc: the link closed")
            self.buf += c

    def close(self):
        if self.p:
            self.p.stdin.close()
            self.p.wait()


def check(a):
    st = one(a, 2)
    if st:
        print("rpc: %s" % STATUS.get(st, st), file=sys.stderr)
        sys.exit(1)


def main():
    ap = argparse.ArgumentParser(description="stock Flipper RPC client")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--exec")
    g.add_argument("--port")
    ap.add_argument("cmd")
    ap.add_argument("args", nargs="*")
    args = ap.parse_args()
    ln = Link(args)
    c, a = args.cmd, args.args
    if c == "ping":
        for r in ln.request(PING_REQ, field(1, (a[0] if a else "ping"))):
            check(r)
            print(decode(one(r, PING_RESP, b"")).get(1, [b""])[0].decode(
                "utf-8", "replace"))
    elif c == "version":
        for r in ln.request(PBVER_REQ):
            check(r)
            v = decode(one(r, PBVER_RESP, b""))
            print("%d.%d" % (one(v, 1), one(v, 2)))
    elif c in ("info", "power"):
        req, resp = (DEVINFO_REQ, DEVINFO_RESP) if c == "info" else \
                    (POWER_REQ, POWER_RESP)
        for r in ln.request(req):
            check(r)
            kv = decode(one(r, resp, b""))
            print("%s: %s" % (one(kv, 1, b"").decode(),
                              one(kv, 2, b"").decode()))
    elif c == "time":
        for r in ln.request(GETDT_REQ):
            check(r)
            d = decode(one(decode(one(r, GETDT_RESP, b"")), 1, b""))
            print("%04d-%02d-%02d %02d:%02d:%02d weekday %d" % (
                one(d, 6), one(d, 5), one(d, 4), one(d, 1), one(d, 2),
                one(d, 3), one(d, 7)))
    elif c == "settime":
        if a:
            y, mo, dd, h, mi, s = (int(x) for x in a)
            wd = 0
        else:
            t = time.localtime()
            y, mo, dd, h, mi, s = t[:6]
            wd = t.tm_wday + 1
        dt = b"".join(field(n, v) for n, v in ((1, h), (2, mi), (3, s),
                      (4, dd), (5, mo), (6, y), (7, wd)) if v)
        for r in ln.request(SETDT_REQ, field(1, dt)):
            check(r)
        print("ok")
    elif c in ("ls", "stat"):
        req, resp = (LIST_REQ, LIST_RESP) if c == "ls" else (STAT_REQ, STAT_RESP)
        for r in ln.request(req, field(1, a[0] if a else "/ext")):
            check(r)
            for f in decode(one(r, resp, b"")).get(1, []):
                f = decode(f)
                kind = "d" if one(f, 1) == 1 else "-"
                print("%s %10d %s" % (kind, one(f, 3),
                                      one(f, 2, b"").decode()))
    elif c == "df":
        for r in ln.request(INFO_REQ, field(1, a[0] if a else "/ext")):
            check(r)
            v = decode(one(r, INFO_RESP, b""))
            print("total %d free %d" % (one(v, 1), one(v, 2)))
    elif c == "cat":
        for r in ln.request(READ_REQ, field(1, a[0])):
            check(r)
            f = decode(one(decode(one(r, READ_RESP, b"")), 1, b""))
            sys.stdout.buffer.write(one(f, 4, b""))
        sys.stdout.flush()
    elif c == "raw":
        for r in ln.request(int(a[0])):
            check(r)
    else:
        ap.error("unknown command %s" % c)
    ln.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
