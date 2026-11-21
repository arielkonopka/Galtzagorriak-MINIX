#!/bin/sh
# tools/rpc.py against the host rpcd (issue #388): the phone profile's
# steps 1-3 end to end over stdin/stdout.  The same client later talks to
# the Flipper over USB CDC (--port) or BLE.
set -e
H=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
C=$(cd "$(dirname "$0")/../../tools" && pwd)/rpc.py
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir "$T/radio"
printf 'kaixo\n' > "$T/agurra.txt"
head -c 3000 /dev/urandom > "$T/big"
R() { python3 -I "$C" --exec "RPCD_EXT=$T $H" "$@"; }
fail() { echo "rpcclient: $*"; exit 1; }
[ "$(R ping kaixo)" = kaixo ] || fail ping
[ "$(R version)" = 0.25 ] || fail version
R info | grep -q '^firmware_origin: Galtzagorriak MINIX$' || fail info
R info | grep -q '^hardware_model: Flipper Zero$' || fail info
R power | grep -q '^charge_level: 77$' || fail power
R time | grep -q '^20[0-9][0-9]-' || fail time
R settime 2026 11 20 21 30 0 2>/dev/null | grep -q ok || fail settime
! R settime 2026 2 30 12 0 0 2>"$T/e" >/dev/null || fail "Feb 30 accepted"
grep -q ERROR_INVALID_PARAMETERS "$T/e"
R ls /ext | grep -q '^d          0 radio$' || fail ls
R ls /ext | grep -q '^-          6 agurra.txt$' || fail ls
R stat /ext/big | grep -q ' 3000 big$' || fail stat
R df /ext | grep -q '^total [0-9]* free [0-9]*$' || fail df
R cat /ext/big > "$T/out"; cmp -s "$T/big" "$T/out" || fail cat
! R ls /etc 2>"$T/e" || fail "/etc listed"
grep -q ERROR_STORAGE_INVALID_NAME "$T/e"
! R cat /ext/nothing 2>"$T/e" || fail "missing file read"
grep -q ERROR_STORAGE_NOT_EXIST "$T/e"
# system_reboot_request (31), gui_start_screen_stream (20): not implemented
! R raw 31 2>"$T/e" || fail "reboot answered"
grep -q ERROR_NOT_IMPLEMENTED "$T/e"
echo "rpcclient: all passed"
