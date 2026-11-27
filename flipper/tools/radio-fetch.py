#!/usr/bin/env python3
"""radio-fetch - get ST's CPU2 wireless stack images for the SD card
(spec R16.0.1a, task T30a).  Runs on a PC: the Flipper has no internet.

    radio-fetch.py [--tag TAG] list [--check] [--fus X.Y.Z]
    radio-fetch.py [--tag TAG] get NAME... --to DIR [--force] [--fus X.Y.Z]
    radio-fetch.py from-fuf UPDATE_DIR --to DIR [--name NAME] [--force]

Source: ST's public STM32CubeWB repository, directory
Projects/STM32WB_Copro_Wireless_Binaries/STM32WB5x/, at the release tag
PINNED_TAG (the one place to bump).  The images are ST's encrypted, signed
binaries, used unchanged, under ST's licence; none is shipped in the MINIX
image.  For every image TABLE carries what was checked by hand against
that tag's Release_Notes.html: install address on 1 MB parts (the
STM32WB55RG the Flipper has), minimum FUS version, size and SHA-256.
`list --check` reads the release notes of the tag again and stops when they
and TABLE disagree; `get` compares the SHA-256 of every download with TABLE.

`get` writes DIR/<name>/ with the image and the R16.0.1 manifest (TSV, one
key per line: name type version fus addr size sha256 note); it never
replaces an existing directory without --force.  On the device, `radio add`
does the same checks again, so a hand-copied image is fine too.

--tag v1.20.0 takes the images of that older release instead (TABLE_1_20):
a device on FUS 1.2.0 cannot install v1.23.0's BLE light or full stack
("FUS_v2 only"), v1.20.0's install on it, and v1.20.0 light is what stock
leaves on a Flipper (the way back).

--fus X.Y.Z (the FUS version `radio` prints on the device) refuses stacks
that need a newer FUS.  --tree DIR reads the "repository" from a local
directory instead of HTTPS (the offline test, tests/radio/fetchtest.sh).
"""
import argparse
import hashlib
import html
import os
import re
import shutil
import sys
import urllib.request

PINNED_TAG = "v1.23.0"
REPO = "https://raw.githubusercontent.com/STMicroelectronics/STM32CubeWB"
SUBDIR = "Projects/STM32WB_Copro_Wireless_Binaries/STM32WB5x"
NOTES = "Release_Notes.html"

# name, ST file (stm32wb5x_<file>_fw.bin), addr on 1 MB parts, FUS needed
# ("-" in the notes: any FUS from 1.2.0, the oldest one that installs a
# current stack), size, SHA-256.  Checked against v1.23.0, 2026-11-20.
TABLE = [
    ("ble-hci-ext", "BLE_HCILayer_extended", 0x080DB000, "1.2.0", 98948,
     "ab3356bb561bb5407f214a3797d42ffcdaba72a761bd8c52e9dd473be9b9d215"),
    ("ble-hci", "BLE_HCILayer", 0x080E1000, "1.2.0", 77820,
     "bf604fa8a98368585b5d53067c8fa9508785662719246a102185c2def7a8267e"),
    ("ble-hci-advscan", "BLE_HCI_AdvScan", 0x080EB000, "1.2.0", 36052,
     "d46e77cb751202a8ebd90b6f763c65fe61c2287d84e8db420ca2ff4ecf171c59"),
    ("ble-lld", "BLE_LLD", 0x080EC000, "1.2.0", 30528,
     "2fcc10ec1a008109e4f55b789c4f3dcd0e526b47234cf774651316c3703b154d"),
    ("ble-mac", "BLE_Mac_802_15_4", 0x080BF000, "2.0.0", 196772,
     "da850effbdec0c88e0840a76ce3c21a1e00e452d07a060769f2e5b3d32558dc8"),
    ("ble-full-ext", "BLE_Stack_full_extended", 0x080C7000, "2.0.0", 166416,
     "35b067e2019556edbf407174fbcecf3c5cb40bd34cfd9f429b4e325883df100e"),
    ("ble-full", "BLE_Stack_full", 0x080D0000, "2.0.0", 130488,
     "8a89d8a8499e99b8d4996f478923aff5e8d3e39a3d6f48948168705b2771932b"),
    ("ble-light", "BLE_Stack_light", 0x080D7000, "2.0.0", 100540,
     "8c1713a8685b764c4c71eeee39629f9fc67accf52e98df7d64bc3b46a385a89a"),
    ("ble-thread-dyn", "BLE_Thread_dynamic", 0x08069000, "2.0.0", 551032,
     "a03ac7d716f2f30d2c9fa22ee9b729cf393ab6f73b6cf39bc6c68375830855ed"),
    ("ble-thread", "BLE_Thread_static", 0x0806B000, "2.0.0", 541652,
     "8e450cca51e53ea3f2731e782b87a0d7a21bcd326d9cd6f516c19f5263b4181b"),
    ("ble-zigbee-ffd-dyn", "BLE_Zigbee_FFD_dynamic", 0x0807D000, "2.0.0", 467964,
     "c41c8a7c3ebb01364f2aead225795c677cee39117fdc916f0b9ba58930a8cbd4"),
    ("ble-zigbee-ffd", "BLE_Zigbee_FFD_static", 0x0807F000, "2.0.0", 460680,
     "c2a9e3cac62ff92492a18da4778cada7d4183539fb66387fea2878cb0299336c"),
    ("ble-zigbee-rfd-dyn", "BLE_Zigbee_RFD_dynamic", 0x0808C000, "2.0.0", 405956,
     "9a9626dcc47e793275d1f0eba33dd226adc1ffdb0a0e04db47fa13cbe2d94072"),
    ("ble-zigbee-rfd", "BLE_Zigbee_RFD_static", 0x0808D000, "2.0.0", 401572,
     "e0fb83f5d0a724dc8f42b4fcf895f99cda13a3237a7f1a5104c0d5254f2a7826"),
    ("mac", "Mac_802_15_4", 0x080E1000, "1.2.0", 73892,
     "5ed1a3246d5dea0f16a13720a1a4da77c0b3c80bfd1545905872bb7d21d8cfcb"),
    ("phy", "Phy_802_15_4", 0x080DD000, "1.2.0", 91296,
     "01eb60ae23c3e6d3c626737555e918085c790a231d81ff88b390b46cdde8137b"),
    ("thread-ftd", "Thread_FTD", 0x0808B000, "1.2.0", 426148,
     "a801e39288a8a96107e1f91ba69ddd80f98f932c999b06ed85b1d6994e9f0841"),
    ("thread-mtd", "Thread_MTD", 0x080A0000, "1.2.0", 340132,
     "e2f9ea7f63cf80d102c5c3b6d823bba01c18d1383d0625bf84c9c3d2f81463ce"),
    ("thread-rcp", "Thread_RCP", 0x080DE000, "1.2.0", 86180,
     "9320e31e6c10ae08306634742c9a2adbd8469b0dee9e64399f95403b4bdb055a"),
    ("zigbee-ffd", "Zigbee_FFD", 0x080A0000, "1.2.0", 342516,
     "ce9813ee0eaf5a915900b436bfb896121465e56c3734c4757b3c78ad47289d4f"),
    ("zigbee-rfd", "Zigbee_RFD", 0x080AE000, "1.2.0", 282788,
     "249e16e4a6f62e0b27d8f060f4300a6e181032fe39594213724c14a549dd761b"),
]
STACK_VERSION = "1.23.0"	# the stacks of the tag, except:
VERSIONS = {"ble-lld": "1.18.0"}

# An older tag, for devices whose FUS is 1.2.0: in v1.23.0 the BLE stacks
# with a host (light, full) are "FUS_v2 only", in v1.20.0 none is (its
# notes have no FUS column and call FUS 1.2.0 up to date).  v1.20.0 is
# also the BLE light stack stock leaves on a Flipper, so ble-light from
# here is the way back.  Only the images checked by hand, 2026-11-26.
TABLE_1_20 = [
    ("ble-hci-advscan", "BLE_HCI_AdvScan", 0x080EB000, "1.2.0", 36072,
     "3ab2f6756f3fe167d9e02493914e1fddafef56b88abb1da7cda55a9c534844f7"),
    ("ble-full", "BLE_Stack_full", 0x080CE000, "1.2.0", 151732,
     "1145b7d2a7d21df6f33ddb62c6a6535f8d13a7390e307ae31ba9eeecda3c0834"),
    ("ble-light", "BLE_Stack_light", 0x080D7000, "1.2.0", 116956,
     "134a9af448d2bc35995600c9e95723a387c144c2d1adea671086f16f95d327df"),
]
# tag -> (table, version of its stacks, exceptions)
TAGS = {
    PINNED_TAG: (TABLE, STACK_VERSION, VERSIONS),
    "v1.20.0": (TABLE_1_20, "1.20.0", {}),
}


def tag_of(args):
    t = getattr(args, "tag", None) or PINNED_TAG
    if t not in TAGS:
        raise SystemExit("radio-fetch: no checked table for tag %s (have: %s)"
                         % (t, ", ".join(sorted(TAGS))))
    return t


def fname(st):
    return "stm32wb5x_%s_fw.bin" % st


def vtuple(v):
    try:
        t = tuple(int(x) for x in v.split("."))
    except ValueError:
        raise SystemExit("radio-fetch: bad version %r (want X.Y.Z)" % v)
    if len(t) != 3:
        raise SystemExit("radio-fetch: bad version %r (want X.Y.Z)" % v)
    return t


def fetch(args, name):
    """The bytes of SUBDIR/name at the pinned tag (or from --tree)."""
    if args.tree:
        p = os.path.join(args.tree, SUBDIR, name)
        with open(p, "rb") as f:
            return f.read()
    url = "%s/%s/%s/%s" % (REPO, tag_of(args), SUBDIR, name)
    with urllib.request.urlopen(url, timeout=60) as r:
        return r.read()


def notes_table(text):
    """From the release notes: {ST file: (addr on 1 MB, fus)} of the
    "Wireless Coprocessor Binary Table" (the newest release, listed
    first)."""
    t = re.sub(r"<[^>]+>", " ", text)
    t = re.sub(r"\s+", " ", html.unescape(t))
    i = t.find("Wireless Coprocessor Binary Table :")
    if i < 0:
        raise SystemExit("radio-fetch: no binary table in the release notes")
    j = t.find("Binary Table", i + 40)
    part = t[i:j if j > 0 else len(t)]
    out = {}
    # the FUS column came with v1.23.0; older notes go straight on to the
    # version (V1.20.0)
    for m in re.finditer(r"stm32wb5x_(\w+?)_fw\.bin (0x[0-9A-Fa-f]{8}) "
                         r"\S+ \S+ \S+ (?:(FUS_v2 only|-) )?V\d", part):
        out[m.group(1)] = (int(m.group(2), 16),
                           "2.0.0" if m.group(3) == "FUS_v2 only" else "1.2.0")
    return out


def check(args):
    got = notes_table(fetch(args, NOTES).decode("utf-8", "replace"))
    bad = 0
    tag = tag_of(args)
    table = TAGS[tag][0]
    for name, st, addr, fus, _, _ in table:
        if st not in got:
            print("radio-fetch: %s (%s) not in the release notes" % (name, st))
            bad += 1
        elif got[st] != (addr, fus):
            print("radio-fetch: %s: notes say 0x%08X FUS %s, table 0x%08X "
                  "FUS %s" % (name, got[st][0], got[st][1], addr, fus))
            bad += 1
    if bad:
        raise SystemExit("radio-fetch: the checked table and the release "
                         "notes of %s disagree; fix TABLE" % tag)
    print("radio-fetch: table agrees with %s release notes (%d stacks)"
          % (tag, len(table)))


def fus_ok(args, fus):
    return args.fus is None or vtuple(args.fus) >= vtuple(fus)


def cmd_list(args):
    if args.check:
        check(args)
    tag = tag_of(args)
    print("STM32CubeWB %s, install addresses for 1 MB parts" % tag)
    print("%-20s %8s %10s %6s  %s" % ("name", "KB", "addr", "fus", "file"))
    for name, st, addr, fus, size, _ in TAGS[tag][0]:
        mark = "" if fus_ok(args, fus) else "  (needs FUS %s: radio fus)" % fus
        print("%-20s %8d 0x%08X %6s  %s%s" % (name, (size + 1023) // 1024,
              addr, fus, fname(st), mark))
    return 0


def manifest(name, typ, version, fus, addr, size, sha, note):
    s = ("name\t%s\ntype\t%s\nversion\t%s\nfus\t%s\naddr\t0x%08X\n"
         "size\t%d\nsha256\t%s\n" % (name, typ, version, fus, addr, size, sha))
    if note:
        s += "note\t%s\n" % note
    return s


def write_dir(args, name, image_name, data, man):
    d = os.path.join(args.to, name)
    if os.path.exists(d):
        if not args.force:
            print("radio-fetch: %s exists (use --force)" % d)
            return False
        shutil.rmtree(d)
    os.makedirs(d)
    with open(os.path.join(d, image_name), "wb") as f:
        f.write(data)
    with open(os.path.join(d, "manifest"), "w") as f:
        f.write(man)
    print("radio-fetch: wrote %s (%d B)" % (d, len(data)))
    return True


def cmd_get(args):
    tag = tag_of(args)
    table, stack_version, versions = TAGS[tag]
    by = {t[0]: t for t in table}
    rc = 0
    for name in args.names:
        if name not in by:
            print("radio-fetch: no stack %r in %s (radio-fetch.py --tag %s "
                  "list)" % (name, tag, tag))
            rc = 1
            continue
        _, st, addr, fus, size, sha = by[name]
        if not fus_ok(args, fus):
            print("radio-fetch: %s needs FUS %s, the device has %s; "
                  "run `radio fus` first" % (name, fus, args.fus))
            rc = 1
            continue
        data = fetch(args, fname(st))
        h = hashlib.sha256(data).hexdigest()
        if h != sha or len(data) != size:
            print("radio-fetch: %s: SHA-256 %s, %d B; the table says %s, "
                  "%d B: not written" % (name, h, len(data), sha, size))
            rc = 1
            continue
        if not write_dir(args, name, fname(st), data,
                         manifest(name, name, versions.get(name, stack_version), fus, addr, size,
                                  h, "STM32CubeWB " + tag)):
            rc = 1
    return rc


def cmd_from_fuf(args):
    """The radio image a stock/Unleashed update directory carries: its
    update.fuf (text, "Key: value") names the file and the address."""
    man = os.path.join(args.dir, "update.fuf")
    keys = {}
    with open(man, encoding="utf-8", errors="replace") as f:
        for line in f:
            k, sep, v = line.partition(":")
            if sep:
                keys[k.strip()] = v.strip()
    img = keys.get("Radio")
    if not img:
        raise SystemExit("radio-fetch: %s names no radio image" % man)
    try:
        addr = int(keys.get("Radio address", ""), 16)
    except ValueError:
        raise SystemExit("radio-fetch: %s: no or bad 'Radio address'" % man)
    with open(os.path.join(args.dir, os.path.basename(img)), "rb") as f:
        data = f.read()
    h = hashlib.sha256(data).hexdigest()
    ver = keys.get("Radio version", "0.0.0").replace(" ", ".")
    typ = "ble-light"			# what stock ships
    for name, _, a, _, size, sha in TABLE + TABLE_1_20:
        if sha == h:			# known: take its name and type
            typ = name
    name = args.name or typ
    return 0 if write_dir(args, name, os.path.basename(img), data,
                          manifest(name, typ, ver, "1.2.0", addr, len(data),
                                   h, "from " + os.path.basename(args.dir))) else 1


def main():
    ap = argparse.ArgumentParser(description="ST CPU2 stack images for "
                                 "/ext/radio (spec R16.0.1a)")
    ap.add_argument("--tree", help="local copy of the repository (tests)")
    ap.add_argument("--tag", help="release tag with a checked table "
                    "(default %s; v1.20.0 for a device on FUS 1.2.0)"
                    % PINNED_TAG)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list")
    p.add_argument("--check", action="store_true",
                   help="compare the table with the tag's release notes")
    p.add_argument("--fus")
    p = sub.add_parser("get")
    p.add_argument("names", nargs="+")
    p.add_argument("--to", required=True)
    p.add_argument("--force", action="store_true")
    p.add_argument("--fus")
    p = sub.add_parser("from-fuf")
    p.add_argument("dir")
    p.add_argument("--to", required=True)
    p.add_argument("--name")
    p.add_argument("--force", action="store_true")
    args = ap.parse_args()
    if getattr(args, "fus", None):
        vtuple(args.fus)
    return {"list": cmd_list, "get": cmd_get,
            "from-fuf": cmd_from_fuf}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
