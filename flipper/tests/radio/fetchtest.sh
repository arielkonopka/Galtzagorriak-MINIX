#!/bin/sh
# Offline test of tools/radio-fetch.py (T30a): a fake STM32CubeWB tree with
# release notes and images.  The images are made up, so `get` must refuse
# them (SHA-256 not the checked one); the notes agree with the table.
set -e
F=$1
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
D=$T/tree/Projects/STM32WB_Copro_Wireless_Binaries/STM32WB5x
mkdir -p "$D" "$T/card"
python3 -I - "$F" "$D" <<'PY'
import importlib.util, sys
spec = importlib.util.spec_from_file_location("rf", sys.argv[1])
rf = importlib.util.module_from_spec(spec); spec.loader.exec_module(rf)
rows = "".join("<tr><td>stm32wb5x_%s_fw.bin</td><td>0x%08X</td><td>0x0</td>"
               "<td>0x0</td><td>0x0</td><td>%s</td><td>V1.23.0</td></tr>\n"
               % (st, a, "FUS_v2 only" if f == "2.0.0" else "-")
               for _, st, a, f, _, _ in rf.TABLE)
open(sys.argv[2] + "/Release_Notes.html", "w").write(
    "<p>Wireless Coprocessor Binary Table : Provides</p><table>%s</table>"
    "<p>Firmware Safeboot Binary Table : x</p>" % rows)
open(sys.argv[2] + "/stm32wb5x_BLE_Stack_light_fw.bin", "wb").write(b"x" * 100)
PY
R="python3 -I $F --tree $T/tree"
$R list --check | grep -q 'table agrees with'
$R list --fus 1.2.0 | grep ble-light | grep -q 'needs FUS 2.0.0'
# made-up image: refused, nothing written
! $R get ble-light --to "$T/card" >"$T/out" || { echo "fake image accepted"; exit 1; }
grep -q 'SHA-256' "$T/out"; test ! -e "$T/card/ble-light"
# too-new FUS
$R get ble-light --fus 1.2.0 --to "$T/card" | grep -q 'needs FUS 2.0.0'
! $R get nosuch --to "$T/card" >/dev/null
# notes that disagree stop `list --check`
sed -i 's/0x080D7000/0x080D8000/' "$D/Release_Notes.html"
! $R list --check >/dev/null 2>&1 || { echo "disagreeing notes accepted"; exit 1; }
# from-fuf: a stock update directory, then no overwrite without --force
mkdir "$T/upd"
printf 'Filetype: Flipper firmware upgrade configuration\nRadio: radio.bin\nRadio address: 0x080D7000\nRadio version: 1 17 3\n' > "$T/upd/update.fuf"
printf 'radio' > "$T/upd/radio.bin"
$R from-fuf "$T/upd" --to "$T/card" --name ble-stock >/dev/null
grep -q '^addr	0x080D7000$' "$T/card/ble-stock/manifest"
grep -q '^version	1.17.3$' "$T/card/ble-stock/manifest"
! $R from-fuf "$T/upd" --to "$T/card" --name ble-stock >/dev/null
$R from-fuf "$T/upd" --to "$T/card" --name ble-stock --force >/dev/null
echo "fetchtest: all passed"
