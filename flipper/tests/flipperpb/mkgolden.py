#!/usr/bin/env python3
"""mkgolden - the golden RPC frames of flipper/tests/flipperpb (#388),
encoded by Google's protobuf from src/lib/flipperpb/proto, so that the
nanopb side is checked against an independent encoder.  Writes golden.h.
Needs `pip install grpcio-tools protobuf`; the output is committed.

A frame is what goes over the wire in stock RPC: the varint length, then
a PB.Main message.
"""
import importlib
import os
import sys
import tempfile

from grpc_tools import protoc

HERE = os.path.dirname(os.path.abspath(__file__))
PROTO = os.path.join(HERE, "../../../src/lib/flipperpb/proto")


def varint(n):
    out = b""
    while True:
        b = n & 0x7F
        n >>= 7
        out += bytes([b | (0x80 if n else 0)])
        if not n:
            return out


def main():
    tmp = tempfile.mkdtemp()
    files = sorted(f for f in os.listdir(PROTO) if f.endswith(".proto"))
    if protoc.main(["protoc", "-I" + PROTO, "--python_out=" + tmp] + files):
        raise SystemExit("mkgolden: protoc failed")
    sys.path.insert(0, tmp)
    fl = importlib.import_module("flipper_pb2")
    st = importlib.import_module("storage_pb2")

    frames = []

    def add(name, m):
        b = m.SerializeToString()
        frames.append((name, varint(len(b)) + b))

    m = fl.Main(command_id=1)
    m.system_ping_request.data = b"\x01\x02\x03"
    add("ping_req", m)
    m = fl.Main(command_id=1)
    m.system_ping_response.data = b"\x01\x02\x03"
    add("ping_resp", m)
    m = fl.Main(command_id=2)
    m.system_protobuf_version_request.SetInParent()
    add("pbver_req", m)
    m = fl.Main(command_id=2)
    m.system_protobuf_version_response.major = 0
    m.system_protobuf_version_response.minor = 25
    add("pbver_resp", m)
    m = fl.Main(command_id=3, has_next=True)
    m.system_device_info_response.key = "hardware_model"
    m.system_device_info_response.value = "Flipper Zero"
    add("devinfo_resp", m)
    m = fl.Main(command_id=7, command_status=fl.ERROR_NOT_IMPLEMENTED)
    m.empty.SetInParent()
    add("notimpl_resp", m)
    m = fl.Main(command_id=4)
    m.storage_list_request.path = "/ext"
    add("list_req", m)
    m = fl.Main(command_id=4)
    f = m.storage_list_response.file.add()
    f.type = st.File.FILE
    f.name = "a.txt"
    f.size = 12
    f = m.storage_list_response.file.add()
    f.type = st.File.DIR
    f.name = "radio"
    add("list_resp", m)
    m = fl.Main(command_id=5)
    d = m.system_set_datetime_request.datetime
    d.hour, d.minute, d.second = 20, 30, 0
    d.day, d.month, d.year, d.weekday = 20, 11, 2026, 5
    add("setdt_req", m)
    m = fl.Main(command_id=6)
    m.storage_read_response.file.data = b"hello"
    add("read_resp", m)

    with open(os.path.join(HERE, "golden.h"), "w") as out:
        out.write("/* Made by mkgolden.py (Google protobuf) from "
                  "src/lib/flipperpb/proto: do not edit. */\n")
        for name, b in frames:
            out.write("static const unsigned char g_%s[] = {" % name)
            out.write(",".join("0x%02x" % c for c in b))
            out.write("};\n")
        out.write("static const struct golden { const char *name; "
                  "const unsigned char *b; unsigned n; } goldens[] = {\n")
        for name, b in frames:
            out.write("  { \"%s\", g_%s, sizeof(g_%s) },\n" % (name, name, name))
        out.write("};\n")
    print("mkgolden: %d frames" % len(frames))


if __name__ == "__main__":
    main()
