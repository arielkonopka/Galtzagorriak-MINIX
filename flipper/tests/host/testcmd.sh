#!/bin/sh
# test(1) from src/commands/simple/test.c, built for the host (#118): the
# file tests -e -b -c -p -L -h -u -g -k, and that an unknown file does not
# count as a non-empty string any more.
#	sh testcmd.sh <the host build of test.c>
T=$1
D=$(mktemp -d) || exit 1
trap 'rm -rf "$D"' 0
fails=0

# want 0|1 args...: the exit status of $T args must be want
want() {
	w=$1; shift
	"$T" "$@"; r=$?
	if [ "$r" = "$w" ]; then echo "  ok   test $* -> $r"
	else echo "  FAIL test $* -> $r, want $w"; fails=$((fails + 1)); fi
}

touch "$D/f"; mkdir "$D/d"; mkfifo "$D/p"; ln -s f "$D/l"
touch "$D/u" "$D/g"; chmod 4755 "$D/u"; chmod 2755 "$D/g"
mkdir "$D/k"; chmod 1777 "$D/k"

# The issue: "test -c /dev/power" with no /dev/power was true.
want 1 -c "$D/none"
for op in -e -b -c -p -L -h -u -g -k; do want 1 $op "$D/none"; done

want 0 -e "$D/f";	want 0 -e "$D/d";	want 0 -e /dev/null
want 0 -c /dev/null;	want 1 -c "$D/f";	want 1 -c "$D/d"
want 1 -b /dev/null;	want 1 -b "$D/f"
B=$(find /dev -maxdepth 1 -type b 2>/dev/null | head -n 1)
if [ -n "$B" ]; then want 0 -b "$B"; want 1 -c "$B"
else echo "  skip -b on a block device: none in /dev"; fi
want 0 -p "$D/p";	want 1 -p "$D/f";	want 1 -p /dev/null
want 0 -L "$D/l";	want 0 -h "$D/l";	want 1 -L "$D/f"
want 0 -u "$D/u";	want 1 -u "$D/g";	want 1 -u "$D/f"
want 0 -g "$D/g";	want 1 -g "$D/u"
want 0 -k "$D/k";	want 1 -k "$D/d"
want 0 ! -c "$D/none"
want 0 -c /dev/null -a -e "$D/f"
want 0 -c "$D/none" -o -p "$D/p"
# the old operators and the one-argument rule still work
want 0 -f "$D/f";	want 0 -d "$D/d";	want 1 -d "$D/f"
want 0 -n x;		want 0 x;		want 1 ""
want 0 a = a;		want 0 3 -gt 2

[ $fails = 0 ] && echo "testcmd: PASS" && exit 0
echo "testcmd: $fails FAIL"; exit 1
