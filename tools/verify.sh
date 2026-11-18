#!/usr/bin/env bash
# End-to-end check for the line-first editor.
set -euo pipefail
cd "$(dirname "$0")/.."
. tools/xvfb.sh

out=/tmp/ink-ribbon-line
rm -f "$out"-*.png /tmp/ink-ribbon-line-*.lvl
echo "== build =="
if ! make -B -j4 >/tmp/ink-ribbon-build.log 2>&1; then tail -30 /tmp/ink-ribbon-build.log; echo "FAIL build"; exit 1; fi
if grep -E "warning:|error:" /tmp/ink-ribbon-build.log; then echo "FAIL build warnings"; exit 1; fi
echo "PASS build without warnings"
if ! make -s test >/tmp/ink-ribbon-test.log 2>&1; then tail -10 /tmp/ink-ribbon-test.log; echo "FAIL core tests"; exit 1; fi
grep -q "ALL CORE TESTS PASSED" /tmp/ink-ribbon-test.log || { echo "FAIL core tests"; exit 1; }
echo "PASS core tests ($(grep -c '^PASS' /tmp/ink-ribbon-test.log) checks)"

xvfb_start
( exec -a "$EDITOR_TAG" ./build/editor --level levels/re2-library.lvl ) >/tmp/ink-ribbon-line.log 2>&1 &
pid=$!
cleanup() { kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; xvfb_stop; _release_display; test_reap; _run_unregister; }
trap cleanup EXIT INT TERM HUP
win=""
for _ in $(seq 1 80); do win=$(xdotool search --name "Ink Ribbon Level Editor" 2>/dev/null | head -1 || true); [ -n "$win" ] && break; sleep 0.1; done
[ -n "$win" ] || { echo "FAIL window"; exit 1; }
eval "$(xdotool getwindowgeometry --shell "$win" | tr -d '\r')"
WX=$X; WY=$Y
xdotool windowfocus "$win" 2>/dev/null || true; sleep 0.5

shot() { import -window "$win" "$1"; }
click() { xdotool mousemove --sync $((WX + $1)) $((WY + $2)) click 1; sleep 0.25; }
rclick() { xdotool mousemove --sync $((WX + $1)) $((WY + $2)) click 3; sleep 0.4; }
pick() { xdotool mousemove --sync $((WX + $1)) $((WY + $2)); sleep 0.4; xdotool click 1; sleep 0.4; }
drag() { xdotool mousemove --sync $((WX + $1)) $((WY + $2)); xdotool mousedown 1; sleep 0.15; xdotool mousemove --sync $((WX + $3)) $((WY + $4)); sleep 0.3; xdotool mouseup 1; sleep 0.4; }
new_level() { xdotool mousemove --sync $((WX + 35)) $((WY + 11)); xdotool click 1; sleep 0.4; xdotool mousemove --sync $((WX + 30)) $((WY + 35)); xdotool click 1; sleep 0.6; }
_save_once() {
    local path=$1
    xdotool keydown ctrl; sleep 0.1; xdotool keydown shift; sleep 0.1; xdotool key s; sleep 0.1
    xdotool keyup shift; sleep 0.1; xdotool keyup ctrl; sleep 0.6
    xdotool keydown ctrl; sleep 0.1; xdotool key a; sleep 0.1; xdotool keyup ctrl; sleep 0.1
    xdotool type --clearmodifiers --delay 20 "$path"; sleep 0.3
    xdotool key --clearmodifiers Return; sleep 0.8
    [ -f "$path" ]
}
save_as() { local p=$1; if _save_once "$p"; then return 0; fi; rm -f "$p"; xdotool key Escape; sleep 0.4; _save_once "$p" || { echo "FAIL save"; exit 1; }; }

echo "== the demo is a line map with filled shapes =="
python3 - levels/re2-library.lvl <<'PYEOF'
import sys
want = {"wall": 76, "shape": 17, "room": 0, "door": 10, "label": 6, "stairs": 4}
have = {k: 0 for k in want}
for line in open(sys.argv[1]):
    t = line.split(" ", 1)[0]
    if t in have: have[t] += 1
if have != want:
    print("counts", have); raise SystemExit(1)
PYEOF
echo "PASS demo counts 76 walls, 17 shapes, 10 doors, 6 labels, 4 stairs"
python3 tools/check_map.py levels/re2-library.lvl
echo "PASS built map bounds, stair flush, fills, doors, labels, and open shapes"

echo "== default tool is Line, not Select =="
shot "$out-default.png"
line_px=$(convert "$out-default.png" -format "%[hex:p{30,102}]" info:)
select_px=$(convert "$out-default.png" -format "%[hex:p{30,76}]" info:)
python3 - "$line_px" "$select_px" <<'PYEOF'
import sys
def lum(h):
    h = h.strip("#")
    return int(h[0:2], 16) + int(h[2:4], 16) + int(h[4:6], 16)
if lum(sys.argv[1]) <= lum(sys.argv[2]):
    print(f"Line button is not active: line {sys.argv[1]} select {sys.argv[2]}")
    raise SystemExit(1)
PYEOF
echo "PASS Line is the active default ($line_px vs $select_px)"

echo "== draw a shape, fill it red from the context menu =="
new_level
click 585 412; click 785 412; click 785 612; click 585 612; click 585 412
xdotool key --clearmodifiers Escape; sleep 0.3
shot "$out-before.png"
rclick 685 512
pick 725 529
pick 890 550
xdotool key --clearmodifiers Escape; sleep 0.3
shot "$out-filled.png"
save_as /tmp/ink-ribbon-line-fill.lvl
python3 - /tmp/ink-ribbon-line-fill.lvl <<'PYEOF'
import sys
shapes = [l.split() for l in open(sys.argv[1]) if l.startswith("shape ")]
if len(shapes) != 1 or shapes[0][2] != "1" or shapes[0][3] != "2":
    print("wrong shape", shapes); raise SystemExit(1)
PYEOF
before=$(convert "$out-before.png" -format "%[hex:p{700,450}]" info:)
after=$(convert "$out-filled.png" -format "%[hex:p{700,450}]" info:)
if [ "$before" != "172832" ] || [ "$after" != "391010" ]; then
    echo "FAIL fill mode did not paint the shape red: $before -> $after"
    exit 1
fi
echo "PASS Mode menu closed the shape red and its fill changed ($before -> $after)"

echo "== marquee a line section and add a white door =="
xdotool keydown shift; drag 620 400 720 430; xdotool keyup shift
shot "$out-span.png"
rclick 670 416
pick 710 433
pick 875 433
save_as /tmp/ink-ribbon-line-door.lvl
python3 - /tmp/ink-ribbon-line-door.lvl <<'PYEOF'
import sys
doors = [l for l in open(sys.argv[1]) if l.startswith("door ")]
if len(doors) != 1: print("doors", doors); raise SystemExit(1)
PYEOF
echo "PASS marquee span added a door"

echo "== marquee an empty box and add stairs =="
xdotool keydown shift; drag 600 650 760 730; xdotool keyup shift
rclick 680 690
pick 720 707
save_as /tmp/ink-ribbon-line-stairs.lvl
python3 - /tmp/ink-ribbon-line-stairs.lvl <<'PYEOF'
import sys
stairs = [l.split() for l in open(sys.argv[1]) if l.startswith("stairs ")]
if len(stairs) != 1: print("stairs", stairs); raise SystemExit(1)
x, y, w, h = float(stairs[0][1]), float(stairs[0][2]), float(stairs[0][5]), float(stairs[0][6])
if w <= 0 or h <= 0: raise SystemExit(1)
PYEOF
echo "PASS empty marquee added stairs"

echo "== Select click-selects the whole shape without a marquee =="
xdotool key --clearmodifiers 1; sleep 0.4
click 635 462
xdotool key --clearmodifiers Delete; sleep 0.5
save_as /tmp/ink-ribbon-select.lvl
python3 - /tmp/ink-ribbon-select.lvl <<'PYEOF'
import sys
want = {"wall": 0, "shape": 0, "door": 0, "stairs": 1, "label": 0}
have = {k: 0 for k in want}
for line in open(sys.argv[1]):
    t = line.split(" ", 1)[0]
    if t in have:
        have[t] += 1
if have != want:
    print("counts after select delete", have); raise SystemExit(1)
PYEOF
xdotool keydown ctrl; sleep 0.1; xdotool key z; sleep 0.1; xdotool keyup ctrl; sleep 0.6
save_as /tmp/ink-ribbon-select-undo.lvl
python3 - /tmp/ink-ribbon-select-undo.lvl <<'PYEOF'
import sys
want = {"wall": 4, "shape": 1, "door": 1, "stairs": 1, "label": 0}
have = {k: 0 for k in want}
for line in open(sys.argv[1]):
    t = line.split(" ", 1)[0]
    if t in have:
        have[t] += 1
if have != want:
    print("counts after undo", have); raise SystemExit(1)
PYEOF
echo "PASS Select deleted the clicked shape and undo restored it"

echo "== Label tool places a label and typing renames it =="
xdotool key --clearmodifiers 3; sleep 0.4
click 400 650
xdotool type --clearmodifiers --delay 20 "Suite Label"; sleep 0.2
xdotool key --clearmodifiers Return; sleep 0.5
save_as /tmp/ink-ribbon-label.lvl
python3 - /tmp/ink-ribbon-label.lvl <<'PYEOF'
import sys
labels = [line.split(" ", 4) for line in open(sys.argv[1]) if line.startswith("label ")]
if len(labels) != 1 or labels[0][4].strip() != "Suite Label":
    print("labels", labels); raise SystemExit(1)
PYEOF
echo "PASS Label tool placed and renamed a label"

echo "== losing focus during a line draft must not freeze =="
xdotool key --clearmodifiers 2; sleep 0.3
click 400 400
xdotool mousemove --sync $((WX + 520)) $((WY + 300)); sleep 0.3
python3 tools/focus_drop.py >/dev/null; sleep 0.4
kill -0 "$pid" 2>/dev/null || { echo "FAIL editor died"; exit 1; }
rss=$(ps -o rss= -p "$pid" | tr -d ' ')
xdotool windowfocus "$win" 2>/dev/null || true; sleep 1
shot "$out-focus-a.png"; sleep 1; shot "$out-focus-b.png"
xdotool key --clearmodifiers Escape; sleep 0.6; shot "$out-focus-c.png"
metric=$(compare -metric AE "$out-focus-b.png" "$out-focus-c.png" null: 2>&1 || true)
if [ "${metric%% *}" = "0" ]; then echo "FAIL no redraw after focus"; exit 1; fi
echo "PASS focus loss survived (rss=${rss} KB)"

echo "== no leftovers =="
cleanup
overflow=$(test_leftovers)
[ -z "$overflow" ] || { echo "FAIL leftover:"; echo "$overflow"; exit 1; }
echo "PASS no leftover processes"
echo "ALL FOUR-TOOL CHECKS PASSED"
