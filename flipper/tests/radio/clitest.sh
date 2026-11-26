#!/bin/sh
# The radio command on the PC (T30a): add / rm / list and /etc/radio.slot
# with fake images (their manifests carry the fake images' SHA-256).
set -e
R=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export RADIO_ROOT=$T
mkdir -p "$T/etc" "$T/home/flipper/radio"
img() {	# name addr fus [bytes]
	mkdir -p "$T/home/flipper/radio/$1"
	printf '%s' "${4:-image of $1}" > "$T/home/flipper/radio/$1/stm32wb5x_$1_fw.bin"
	h=$(sha256sum "$T/home/flipper/radio/$1/stm32wb5x_$1_fw.bin" | cut -c1-64)
	n=$(wc -c < "$T/home/flipper/radio/$1/stm32wb5x_$1_fw.bin")
	printf 'name\t%s\ntype\t%s\nversion\t1.23.0\nfus\t%s\naddr\t%s\nsize\t%s\nsha256\t%s\n' \
		"$1" "$1" "$3" "$2" $n "$h" > "$T/home/flipper/radio/$1/manifest"
}
fail() { echo "clitest: $*"; exit 1; }
$R | grep -q 'radio slot: none'
$R list | grep -q 'no images'
img ble-light 0x080D7000 2.0.0
img zigbee-baserria 0x0807F000 2.0.0
$R add -F 2.0.0 ble-light | grep -q 'ok'
grep -q '^0x0807F000$' "$T/etc/radio.slot" || fail "slot not the lowest"
# too-new FUS refused
$R add -F 1.2.0 ble-light 2>&1 | grep -q 'FUS too old' || fail "old FUS accepted"
# bad hash: refused and left out of the slot
printf 'x' >> "$T/home/flipper/radio/zigbee-baserria/stm32wb5x_zigbee-baserria_fw.bin"
! $R add zigbee-baserria 2>"$T/e" || fail "bad image accepted"
grep -q 'size differs' "$T/e"
$R list | grep zigbee-baserria | grep -q 'size differs'
$R add ble-light >/dev/null
grep -q '^0x080D7000$' "$T/etc/radio.slot" || fail "bad image counted"
# an image that would reach the system, libc or launcher
img huge 0x08060000 1.2.0
! $R add huge 2>"$T/e" || fail "slot over the base accepted"
grep -q 'slot too low' "$T/e"
# a manifest whose name is not its directory
img other 0x080D7000 1.2.0
sed -i 's/^name\tother/name\tnot-other/' "$T/home/flipper/radio/other/manifest"
! $R add other 2>/dev/null || fail "renamed manifest accepted"
# rm: the slot follows, and goes when the last good image goes
$R rm zigbee-baserria >/dev/null
test ! -e "$T/home/flipper/radio/zigbee-baserria" || fail "rm left the directory"
$R rm ble-light >/dev/null
test ! -e "$T/etc/radio.slot" || fail "slot left without images"
! $R rm ../etc 2>/dev/null || fail "rm outside /home/flipper/radio"
echo "clitest: all passed"
