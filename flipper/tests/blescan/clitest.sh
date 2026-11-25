#!/bin/sh
# The blescan command on a recorded HCI stream (H4 events).  usage:
# clitest.sh ./build/blescan
B=${1:-./build/blescan}
T=${TMPDIR:-/tmp}/blescan.$$
fails=0
ok() { echo "  ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails+1)); }
trap 'rm -f $T $T.out' 0

python3 -I - > $T <<'PY'
import sys
def rep(addr, at, ad, rssi):
    a = bytes(int(x, 16) for x in addr.split(":"))[::-1]
    p = bytes([0x02, 1, 0, at]) + a + bytes([len(ad)]) + ad + bytes([rssi & 255])
    return bytes([0x04, 0x3E, len(p)]) + p
out = sys.stdout.buffer
findmy = bytes([2, 1, 6, 6, 0xFF, 0x4C, 0, 0x12, 0x19, 0])
tile = bytes([3, 3, 0xED, 0xFE, 5, 9]) + b"Tile"
out.write(rep("C0:11:22:33:44:55", 1, findmy, -80))
out.write(bytes([0x04, 0x05, 4, 0, 1, 0, 0x13]))	# another event
out.write(rep("C0:11:22:33:44:55", 1, findmy, -70))
out.write(rep("DE:AD:BE:EF:00:01", 0, tile, -60))
out.write(rep("01:02:03:04:05:06", 0, b"", -40))
PY

$B $T > $T.out; [ $? -eq 0 ] && ok "list: exit 0" || bad "list: exit status"
head -1 $T.out | grep -q '^ADDRESS' && ok "list: header" || bad "list: header"
sed -n 2p $T.out | grep -q '^DE:AD:BE:EF:00:01 .*Tile' && ok "list: Tile first (a tracker, stronger)" || bad "list: first line"
sed -n 3p $T.out | grep -q '^C0:11:22:33:44:55 .*-78 .*Find My' && ok "list: Find My, averaged -78" || bad "list: second line"
sed -n 4p $T.out | grep -q '^01:02:03:04:05:06 ' && ok "list: the plain device last" || bad "list: third line"
$B -t $T > $T.out; [ "$(wc -l < $T.out)" = 3 ] && ok "-t: header and two trackers" || bad "-t: lines"
cat $T | $B -t > $T.out; [ "$(wc -l < $T.out)" = 3 ] && ok "standard input" || bad "standard input"
$B -F c0:11:22:33:44:55 $T > $T.out; [ "$(wc -l < $T.out)" = 2 ] && ok "-F: a line for each report of that device" || bad "-F: lines"
head -1 $T.out | grep -q -- '-80 dBm' && ok "-F: the first RSSI" || bad "-F: first line"
sed -n 2p $T.out | grep -q '\[####    \].*-70 dBm (avg -78)' && ok "-F: bars and average" || { bad "-F: second line"; cat $T.out; }
$B -F 11:22 $T 2>/dev/null; [ $? -eq 2 ] && ok "-F: a bad address is a usage error" || bad "-F: bad address"
$B -F 00:00:00:00:00:00 $T >/dev/null; [ $? -eq 1 ] && ok "-F: a device never seen: exit 1" || bad "-F: not seen"
$B /nonexistent 2>/dev/null; [ $? -eq 1 ] && ok "no file: exit 1" || bad "no file"
: > $T.out; $B $T.out >/dev/null; [ $? -eq 1 ] && ok "an empty stream: exit 1" || bad "empty stream"
[ $fails -eq 0 ] && echo "blescan cli: PASS" || { echo "blescan cli: $fails FAIL"; exit 1; }
