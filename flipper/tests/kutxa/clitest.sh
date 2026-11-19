#!/bin/sh
# clitest.sh KUTXA - the `kutxa` command (spec RR.8, ACR.10): the use cases
# from the console, with the tools the Flipper has (grep, cut, sort, head,
# wc; no awk).  Written before the command (test first, 2026-11-18).
# Times are UTC.  Basque in the data: bilera = meeting, lana = work,
# mendia = mountain, etxea = home, ohartu = notice.
K=${1:?usage: clitest.sh path/to/kutxa}
case $K in /*) ;; *) K=$(pwd)/$K ;; esac
T=$(mktemp -d /tmp/clitest.XXXXXX) || exit 1
trap 'rm -rf "$T"' 0
cd "$T" || exit 1
fails=0
ok() { :; }
bad() { echo "FAIL: $*"; fails=$((fails + 1)); }
same() {	# same NAME WANT GOT
	if [ "$2" = "$3" ]; then ok; else
		bad "$1"; printf '  want: %s\n  got:  %s\n' "$2" "$3"; fi
}
TAB=$(printf '\t')

# ---- calendar: time keys, typed fields, a week, grep and cut ----
$K create cal.kutxa || bad "create"
$K put cal.kutxa -k time 2026-11-19T09:00 title=bilera tag=lana room#=12 || bad "put 1"
$K put cal.kutxa -k time 2026-11-21T07:30 title=mendia tag=etxea || bad "put 2"
$K put cal.kutxa -k time 2026-11-24T15:00 title=dentista tag=etxea end@=2026-11-24T16:00 || bad "put 3"
$K put cal.kutxa -k time 2026-12-02T10:00 title="urteko bilera" tag=lana || bad "put 4"
same "week, chosen columns" \
"2026-11-19T09:00:00Z${TAB}bilera${TAB}lana
2026-11-21T07:30:00Z${TAB}mendia${TAB}etxea
2026-11-24T15:00:00Z${TAB}dentista${TAB}etxea" \
"$($K range cal.kutxa -k time -f title,tag 2026-11-19 2026-11-26)"
same "dump with field names" \
"2026-11-19T09:00:00Z${TAB}title=bilera${TAB}tag=lana${TAB}room#=12" \
"$($K dump cal.kutxa -k time | head -1)"
same "a time field" \
"2026-11-24T15:00:00Z${TAB}2026-11-24T16:00:00Z" \
"$($K get cal.kutxa -k time -f end 2026-11-24T15:00)"
same "missing field = empty column" \
"2026-11-21T07:30:00Z${TAB}mendia${TAB}" \
"$($K get cal.kutxa -k time -f title,end 2026-11-21T07:30)"
same "work only, grep and cut" \
"bilera
urteko bilera" \
"$($K dump cal.kutxa -k time -f title,tag | grep "${TAB}lana\$" | cut -f2)"

# ---- an index added later ----
$K index cal.kutxa main tag || bad "index"
same "trees" "main
main.tag" "$($K trees cal.kutxa)"
same "find via the index" \
"2026-11-21T07:30:00Z${TAB}mendia
2026-11-24T15:00:00Z${TAB}dentista" \
"$($K find cal.kutxa -k time -f title main tag etxea)"
$K put cal.kutxa -k time 2026-11-21T07:30 title=mendia tag=lana || bad "retag"
same "the index follows a change" "1" \
"$($K find cal.kutxa -k time main tag etxea | wc -l | tr -d ' ')"

# ---- reminders: the next one from now ----
$K create rem.kutxa
for t in 2026-11-19T08:00 2026-11-19T12:30 2026-11-20T07:00; do
	$K put rem.kutxa -k time $t text=ohartu
done
same "next >= now" "2026-11-19T12:30:00Z" \
"$($K range rem.kutxa -k time -n 1 2026-11-19T09:00 | cut -f1)"
$K del rem.kutxa -k time 2026-11-19T12:30 || bad "fired"
same "after it fired" "2026-11-20T07:00:00Z" \
"$($K range rem.kutxa -k time -n 1 2026-11-19T09:00 | cut -f1)"
$K del rem.kutxa -k time 2026-11-19T12:30 2>/dev/null && bad "delete twice"

# ---- holidays: import a TSV, a prefix per country, a get ----
printf 'key\tname\nPL/01-01\tNowy Rok\nPL/11-11\tNarodowe Święto Niepodległości\nPL/E+60\tBoże Ciało\nDE/10-03\tTag der Deutschen Einheit\nDE-BY/E+60\tFronleichnam\n' >hol.tsv
$K create hol.kutxa
$K import hol.kutxa <hol.tsv || bad "import"
same "PL by prefix" "3" "$($K range hol.kutxa -p PL/ | wc -l | tr -d ' ')"
same "DE-BY own rules" "DE-BY/E+60${TAB}name=Fronleichnam" "$($K range hol.kutxa -p DE-BY/)"
same "UTF-8 value" "Narodowe Święto Niepodległości" "$($K get hol.kutxa -f name PL/11-11 | cut -f2)"
$K get hol.kutxa PL/12-31 >/dev/null 2>&1; same "not found exits 1" "1" "$?"
printf 'key\tname\nb\tx\na\ty\n' | $K import hol.kutxa -t bad || bad "unsorted import"
same "unsorted import sorted" "a" "$($K dump hol.kutxa -t bad | head -1 | cut -f1)"

# ---- logs: a day of readings, the newest three, keep the last hour ----
$K create log.kutxa
i=0
{
	printf 'key\tc#\n'
	while [ $i -lt 1440 ]; do
		h=$((i / 60)); m=$((i % 60))
		printf '2026-11-18T%02d:%02d\t%d\n' $h $m $((200 + i % 7))
		i=$((i + 1))
	done
} | $K import log.kutxa -k time || bad "import log"
same "1440 readings" "1440" "$($K dump log.kutxa | wc -l | tr -d ' ')"
same "newest three" \
"2026-11-18T23:59:00Z
2026-11-18T23:58:00Z
2026-11-18T23:57:00Z" \
"$($K range log.kutxa -k time -r -n 3 | cut -f1)"
$K delrange log.kutxa -k time 2026-11-18 2026-11-18T23:00 || bad "delrange"
same "an hour left" "60" "$($K dump log.kutxa | wc -l | tr -d ' ')"
before=$(wc -c <log.kutxa)
$K compact log.kutxa || bad "compact"
after=$(wc -c <log.kutxa)
[ "$after" -lt "$before" ] || bad "compact: $after B is not below $before B"
same "same after compaction" "60" "$($K dump log.kutxa | wc -l | tr -d ' ')"

# ---- text that needs escaping in TSV ----
$K create esc.kutxa
$K put esc.kutxa k1 "note=bat${TAB}bi" || bad "put tab"
$K put esc.kutxa k2 "note=lerro
berria" || bad "put newline"
same "escapes" "k1${TAB}bat\\tbi
k2${TAB}lerro\\nberria" "$($K dump esc.kutxa -f note)"

# ---- check, info, a second writer ----
same "check" "ok" "$($K check cal.kutxa)"
$K info cal.kutxa | grep -q '^records 4$' || bad "info: records"
$K put esc.kutxa k3 note=x 2>/dev/null &
$K put esc.kutxa k4 note=y 2>/dev/null
wait
same "both writers got in, one after the other" "4" "$($K dump esc.kutxa | wc -l | tr -d ' ')"

if [ $fails -ne 0 ]; then echo "clitest: $fails FAILED"; exit 1; fi
echo "clitest: all passed"
